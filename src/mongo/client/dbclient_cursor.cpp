/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/client/dbclient_cursor.h"

#include <boost/cstdint.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;

namespace {
void addMetadata(DBClientBase* client, BSONObjBuilder* bob) {
    if (client->getRequestMetadataWriter()) {
        auto opCtx = (haveClient() ? cc().getOperationContext() : nullptr);
        uassertStatusOK(client->getRequestMetadataWriter()(opCtx, bob));
    }
}

template <typename T>
Message assembleCommandRequest(DBClientBase* client,
                               const DatabaseName& dbName,
                               const T& command,
                               const ReadPreferenceSetting& readPref) {
    // Add the $readPreference and other metadata to the request.
    BSONObjBuilder builder;
    command.serialize(&builder);
    readPref.toContainingBSON(&builder);
    addMetadata(client, &builder);

    auto vts = [&]() {
        auto tenantId = dbName.tenantId();
        return tenantId
            ? auth::ValidatedTenancyScopeFactory::create(
                  *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{})
            : auth::ValidatedTenancyScope::kNotRequired;
    }();
    auto opMsgRequest = OpMsgRequestBuilder::create(vts, dbName, builder.obj());
    return opMsgRequest.serialize();
}
}  // namespace

Message DBClientCursor::assembleInit() {
    if (_cursorId) {
        return assembleGetMore();
    }

    // We haven't gotten a cursorId yet so we need to issue the initial find command.
    tassert(9279705, "Find request is invalid", _findRequest);
    return assembleCommandRequest<FindCommandRequest>(
        _client, _ns.dbName(), *_findRequest, _readPref);
}

Message DBClientCursor::assembleGetMore() {
    tassert(9279706, "CursorId is unexpectedly zero", _cursorId);
    auto getMoreRequest = GetMoreCommandRequest(_cursorId, _ns.coll().toString());
    getMoreRequest.setBatchSize(
        boost::make_optional(_batchSize != 0, static_cast<int64_t>(_batchSize)));
    getMoreRequest.setMaxTimeMS(boost::make_optional(
        tailableAwaitData(),
        static_cast<std::int64_t>(durationCount<Milliseconds>(_awaitDataTimeout))));
    if (_term) {
        getMoreRequest.setTerm(static_cast<std::int64_t>(*_term));
    }
    getMoreRequest.setLastKnownCommittedOpTime(_lastKnownCommittedOpTime);
    auto msg = assembleCommandRequest<GetMoreCommandRequest>(
        _client, _ns.dbName(), getMoreRequest, _readPref);

    // Set the exhaust flag if needed.
    if (_isExhaust) {
        OpMsg::setFlag(&msg, OpMsg::kExhaustSupported);
    }
    return msg;
}

bool DBClientCursor::init() {
    tassert(
        9279707, "Connection should not have any pending replies", !_connectionHasPendingReplies);
    Message toSend = assembleInit();
    MONGO_verify(_client);
    Message reply;
    try {
        reply = _client->call(toSend, &_originalHost);
    } catch (const DBException&) {
        // log msg temp?
        LOGV2(20127, "DBClientCursor::init call() failed");
        // We always want to throw on network exceptions.
        throw;
    }
    if (reply.empty()) {
        // log msg temp?
        LOGV2(20128, "DBClientCursor::init message from call() was empty");
        return false;
    }
    dataReceived(reply);
    _isInitialized = true;
    return true;
}

void DBClientCursor::requestMore() {
    // For exhaust queries, once the stream has been initiated we get data blasted to us
    // from the remote server, without a need to send any more 'getMore' requests.
    if (_isExhaust && _connectionHasPendingReplies) {
        return exhaustReceiveMore();
    }

    tassert(
        9279708, "Connection should not have any pending replies", !_connectionHasPendingReplies);
    MONGO_verify(_cursorId && _batch.pos == _batch.objs.size());

    auto doRequestMore = [&] {
        Message toSend = assembleGetMore();
        Message response = _client->call(toSend);
        dataReceived(response);
    };
    if (_client)
        return doRequestMore();

    tassert(9279709, "Scoped host size can not be zero", _scopedHost.size());
    DBClientBase::withConnection_do_not_use(_scopedHost, [&](DBClientBase* conn) {
        ON_BLOCK_EXIT([&, origClient = _client] { _client = origClient; });
        _client = conn;
        doRequestMore();
    });
}

/**
 * For exhaust cursors, the server just blasts data at us. The end of a stream is marked with a
 * cursor id of 0.
 */
void DBClientCursor::exhaustReceiveMore() {
    MONGO_verify(_cursorId);
    MONGO_verify(_batch.pos == _batch.objs.size());
    Message response;
    MONGO_verify(_client);
    try {
        auto response = _client->recv(_lastRequestId);
        dataReceived(response);
    } catch (DBException& e) {
        e.addContext("recv failed while exhausting cursor");
        throw;
    }
}

BSONObj DBClientCursor::commandDataReceived(const Message& reply) {
    NetworkOp op = reply.operation();
    tassert(9279710,
            str::stream() << "Operation should either be 'opReply' or 'dbMsg', but got "
                          << networkOpToString(op),
            op == opReply || op == dbMsg);

    // Check if the reply indicates that it is part of an exhaust stream.
    const auto isExhaustReply = OpMsg::isFlagSet(reply, OpMsg::kMoreToCome);
    _connectionHasPendingReplies = isExhaustReply;
    if (isExhaustReply) {
        _lastRequestId = reply.header().getId();
    }

    auto commandReply = _client->parseCommandReplyMessage(_client->getServerAddress(), reply);
    auto commandStatus = getStatusFromCommandResult(commandReply->getCommandReply());

    if (commandStatus == ErrorCodes::StaleConfig) {
        uassertStatusOK(
            commandStatus.withContext("stale config in DBClientCursor::dataReceived()"));
    } else if (!commandStatus.isOK()) {
        _wasError = true;
    }

    return commandReply->getCommandReply().getOwned();
}

void DBClientCursor::dataReceived(const Message& reply, bool& retry, string& host) {
    _batch.objs.clear();
    _batch.pos = 0;

    const auto replyObj = commandDataReceived(reply);
    _cursorId = 0;  // Don't try to kill cursor if we get back an error.

    auto cr = uassertStatusOK(CursorResponse::parseFromBSON(replyObj, nullptr, _ns.tenantId()));
    _cursorId = cr.getCursorId();
    uassert(50935,
            "Received a getMore response with a cursor id of 0 and the moreToCome flag set.",
            !(_connectionHasPendingReplies && _cursorId == 0));

    _ns = cr.getNSS();  // find command can change the ns to use for getMores.
    // Store the resume token, if we got one.
    _postBatchResumeToken = cr.getPostBatchResumeToken();
    _batch.objs = cr.releaseBatch();

    if (replyObj.hasField(LogicalTime::kOperationTimeFieldName)) {
        _operationTime = LogicalTime::fromOperationTime(replyObj).asTimestamp();
    }
}

/** If true, safe to call next().  Requests more from server if necessary. */
bool DBClientCursor::more() {
    tassert(9279711, "Cursor is not initialized", _isInitialized);
    if (!_putBack.empty())
        return true;

    if (_batch.pos < _batch.objs.size())
        return true;

    if (_cursorId == 0)
        return false;

    requestMore();
    return _batch.pos < _batch.objs.size();
}

BSONObj DBClientCursor::next() {
    tassert(9279712, "Cursor is not initialized", _isInitialized);
    if (!_putBack.empty()) {
        BSONObj ret = _putBack.top();
        _putBack.pop();
        return ret;
    }

    uassert(
        13422, "DBClientCursor next() called but more() is false", _batch.pos < _batch.objs.size());

    return std::move(_batch.objs[_batch.pos++]);
}

BSONObj DBClientCursor::nextSafe() {
    BSONObj o = next();

    // Only convert legacy errors ($err) to exceptions. Otherwise, just return the response and the
    // caller will interpret it as a command error.
    if (_wasError && strcmp(o.firstElementFieldName(), "$err") == 0) {
        uassertStatusOK(getStatusFromCommandResult(o));
    }

    return o;
}

void DBClientCursor::peek(vector<BSONObj>& v, int atMost) {
    tassert(9279713, "Cursor is not initialized", _isInitialized);
    auto end = atMost >= static_cast<int>(_batch.objs.size() - _batch.pos)
        ? _batch.objs.end()
        : _batch.objs.begin() + _batch.pos + atMost;
    v.insert(v.end(), _batch.objs.begin() + _batch.pos, end);
}

BSONObj DBClientCursor::peekFirst() {
    vector<BSONObj> v;
    peek(v, 1);

    if (v.size() > 0)
        return v[0];
    else
        return BSONObj();
}

bool DBClientCursor::peekError(BSONObj* error) {
    tassert(9279714, "Cursor is not initialized", _isInitialized);
    if (!_wasError)
        return false;

    vector<BSONObj> v;
    peek(v, 1);

    MONGO_verify(v.size() == 1);
    // We check both the legacy error format, and the new error format. hasErrField checks for
    // $err, and getStatusFromCommandResult checks for modern errors of the form '{ok: 0.0, code:
    // <...>, errmsg: ...}'.
    MONGO_verify(hasErrField(v[0]) || !getStatusFromCommandResult(v[0]).isOK());

    if (error)
        *error = v[0].getOwned();
    return true;
}

void DBClientCursor::attach(AScopedConnection* conn) {
    MONGO_verify(_scopedHost.size() == 0);
    MONGO_verify(conn);
    MONGO_verify(conn->get());

    if (conn->get()->type() == ConnectionString::ConnectionType::kReplicaSet) {
        if (_client)
            _scopedHost = _client->getServerAddress();
        else
            massert(14821, "No client specified, cannot store multi-host connection.", false);
    } else {
        _scopedHost = conn->getHost();
    }

    conn->done();
    _client = nullptr;
}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const NamespaceStringOrUUID& nsOrUuid,
                               long long cursorId,
                               bool isExhaust,
                               std::vector<BSONObj> initialBatch,
                               boost::optional<Timestamp> operationTime,
                               boost::optional<BSONObj> postBatchResumeToken)
    : _batch{std::move(initialBatch)},
      _client(client),
      _originalHost(_client->getServerAddress()),
      _nsOrUuid(nsOrUuid),
      _isInitialized(true),
      _ns(nsOrUuid.isNamespaceString() ? nsOrUuid.nss() : NamespaceString{nsOrUuid.dbName()}),
      _cursorId(cursorId),
      _isExhaust(isExhaust),
      _operationTime(operationTime),
      _postBatchResumeToken(postBatchResumeToken) {}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               FindCommandRequest findRequest,
                               const ReadPreferenceSetting& readPref,
                               bool isExhaust)
    : _client(client),
      _originalHost(_client->getServerAddress()),
      _nsOrUuid(findRequest.getNamespaceOrUUID()),
      _ns(_nsOrUuid.isNamespaceString() ? _nsOrUuid.nss() : NamespaceString{_nsOrUuid.dbName()}),
      _batchSize(findRequest.getBatchSize().value_or(0)),
      _findRequest(std::move(findRequest)),
      _readPref(readPref),
      _isExhaust(isExhaust) {
    // Internal clients should always pass an explicit readConcern. If the caller did not already
    // pass a readConcern than we must explicitly initialize an empty readConcern so that it ends up
    // in the serialized version of the find command which will be sent across the wire.
    if (!_findRequest->getReadConcern()) {
        _findRequest->setReadConcern(repl::ReadConcernArgs());
    }
}

StatusWith<std::unique_ptr<DBClientCursor>> DBClientCursor::fromAggregationRequest(
    DBClientBase* client, AggregateCommandRequest aggRequest, bool secondaryOk, bool useExhaust) {
    BSONObj ret;
    try {
        if (!client->runCommand(aggRequest.getNamespace().dbName(),
                                aggregation_request_helper::serializeToCommandObj(aggRequest),
                                ret,
                                secondaryOk ? QueryOption_SecondaryOk : 0)) {
            return getStatusFromCommandResult(ret);
        }
    } catch (...) {
        return exceptionToStatus();
    }
    long long cursorId = ret["cursor"].Obj()["id"].Long();
    std::vector<BSONObj> firstBatch;
    for (BSONElement elem : ret["cursor"].Obj()["firstBatch"].Array()) {
        firstBatch.emplace_back(elem.Obj().getOwned());
    }
    boost::optional<BSONObj> postBatchResumeToken;
    if (auto postBatchResumeTokenElem = ret["cursor"].Obj()["postBatchResumeToken"];
        postBatchResumeTokenElem.type() == BSONType::Object) {
        postBatchResumeToken = postBatchResumeTokenElem.Obj().getOwned();
    } else if (ret["cursor"].Obj().hasField("postBatchResumeToken")) {
        return Status(ErrorCodes::Error(5761702),
                      "Expected field 'postbatchResumeToken' to be of object type");
    }

    boost::optional<Timestamp> operationTime = boost::none;
    if (ret.hasField(LogicalTime::kOperationTimeFieldName)) {
        operationTime = LogicalTime::fromOperationTime(ret).asTimestamp();
    }

    return {std::make_unique<DBClientCursor>(client,
                                             aggRequest.getNamespace(),
                                             cursorId,
                                             useExhaust,
                                             firstBatch,
                                             operationTime,
                                             postBatchResumeToken)};
}

DBClientCursor::~DBClientCursor() {
    kill();
}

void DBClientCursor::kill() {
    DESTRUCTOR_GUARD({
        if (_cursorId && !globalInShutdownDeprecated()) {
            auto killCursor = [&](auto&& conn) {
                conn->killCursor(_ns, _cursorId);
            };

            // We only need to kill the cursor if there aren't pending replies. Pending replies
            // indicates that this is an exhaust cursor, so the connection must be closed and the
            // cursor will automatically be cleaned up by the upstream server.
            if (_client && !_connectionHasPendingReplies) {
                killCursor(_client);
            }
        }
    });

    // Mark this cursor as dead since we can't do any getMores.
    _cursorId = 0;
    _isInitialized = false;
}

}  // namespace mongo