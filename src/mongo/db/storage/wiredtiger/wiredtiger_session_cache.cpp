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


#include <cerrno>
#include <cstdlib>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <wiredtiger.h>

#include "mongo/base/error_codes.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

WiredTigerSession::WiredTigerSession(WT_CONNECTION* conn, uint64_t epoch, uint64_t cursorEpoch)
    : _epoch(epoch),
      _session(nullptr),
      _cursorGen(0),
      _cursorsOut(0),
      _compiled(nullptr),
      _idleExpireTime(Date_t::min()) {
    invariantWTOK(conn->open_session(conn, nullptr, "isolation=snapshot", &_session), nullptr);
}

WiredTigerSession::WiredTigerSession(WT_CONNECTION* conn,
                                     WiredTigerSessionCache* cache,
                                     uint64_t epoch,
                                     uint64_t cursorEpoch)
    : _epoch(epoch),
      _session(nullptr),
      _cursorGen(0),
      _cursorsOut(0),
      _cache(cache),
      _compiled(nullptr),
      _idleExpireTime(Date_t::min()) {
    invariantWTOK(conn->open_session(conn, nullptr, "isolation=snapshot", &_session), nullptr);
    setCompiledConfigurationsPerConnection(cache->getCompiledConfigurations());
}

WiredTigerSession::~WiredTigerSession() {
    if (_session) {
        invariantWTOK(_session->close(_session, nullptr), nullptr);
    }
}

namespace {
void _openCursor(WT_SESSION* session,
                 const std::string& uri,
                 const char* config,
                 WT_CURSOR** cursorOut) {
    int ret = session->open_cursor(session, uri.c_str(), nullptr, config, cursorOut);
    if (ret == 0) {
        return;
    }

    auto status = wtRCToStatus(ret, session);

    if (ret == EBUSY) {
        // This may happen when there is an ongoing full validation, with a call to WT::verify.
        // Other operations which may trigger this include salvage, rollback_to_stable, upgrade,
        // alter, or if there is a bulk cursor open. Mongo (currently) does not run any of
        // these operations concurrently with this code path, except for validation.

        uassertStatusOK(status);
    } else if (ret == ENOENT) {
        uasserted(ErrorCodes::CursorNotFound,
                  str::stream() << "Failed to open a WiredTiger cursor. Reason: " << status
                                << ", uri: " << uri << ", config: " << config);
    }

    LOGV2_FATAL_NOTRACE(50882,
                        "Failed to open WiredTiger cursor. This may be due to data corruption",
                        "uri"_attr = uri,
                        "config"_attr = config,
                        "error"_attr = status,
                        "message"_attr = kWTRepairMsg);
}
}  // namespace

WT_CURSOR* WiredTigerSession::getCachedCursor(uint64_t id, const std::string& config) {
    // Find the most recently used cursor
    for (CursorCache::iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
        // Ensure that all properties of this cursor are identical to avoid mixing cursor
        // configurations. Note that this uses an exact string match, so cursor configurations with
        // parameters in different orders will not be considered equivalent.
        if (i->_id == id && i->_config == config) {
            WT_CURSOR* c = i->_cursor;
            _cursors.erase(i);
            _cursorsOut++;
            return c;
        }
    }
    return nullptr;
}

WT_CURSOR* WiredTigerSession::getNewCursor(const std::string& uri, const char* config) {
    WT_CURSOR* cursor = nullptr;
    _openCursor(_session, uri, config, &cursor);
    _cursorsOut++;
    return cursor;
}

void WiredTigerSession::releaseCursor(uint64_t id, WT_CURSOR* cursor, std::string config) {
    // When releasing the cursor, we would want to check if the session cache is already in shutdown
    // and prevent the race condition that the shutdown starts after the check.
    WiredTigerSessionCache::BlockShutdown blockShutdown(_cache);

    // Avoids the cursor already being destroyed during the shutdown.
    if (_cache->isShuttingDown()) {
        return;
    }

    invariant(_session);
    invariant(cursor);
    _cursorsOut--;

    invariantWTOK(cursor->reset(cursor), _session);

    // Cursors are pushed to the front of the list and removed from the back
    _cursors.push_front(WiredTigerCachedCursor(id, _cursorGen++, cursor, std::move(config)));

    // A negative value for wiredTigercursorCacheSize means to use hybrid caching.
    std::uint32_t cacheSize = abs(gWiredTigerCursorCacheSize.load());

    while (!_cursors.empty() && _cursorGen - _cursors.back()._gen > cacheSize) {
        cursor = _cursors.back()._cursor;
        _cursors.pop_back();
        invariantWTOK(cursor->close(cursor), _session);
    }
}

void WiredTigerSession::closeCursor(WT_CURSOR* cursor) {
    invariant(_session);
    invariant(cursor);
    _cursorsOut--;

    invariantWTOK(cursor->close(cursor), _session);
}

void WiredTigerSession::closeAllCursors(const std::string& uri) {
    invariant(_session);

    bool all = (uri == "");
    for (auto i = _cursors.begin(); i != _cursors.end();) {
        WT_CURSOR* cursor = i->_cursor;
        if (cursor && (all || uri == cursor->uri)) {
            invariantWTOK(cursor->close(cursor), _session);
            i = _cursors.erase(i);
        } else
            ++i;
    }
}

void WiredTigerSession::reconfigure(const std::string& newConfig, std::string undoConfig) {
    if (newConfig == undoConfig) {
        // The undoConfig string is the config string that resets our session back to default
        // settings. If our new configuration is the same as the undoconfig string, then that means
        // that we are either setting our configuration back to default, or that the newConfig
        // string does not change our default values. In this case, we can erase the undoConfig
        // string from our set of undo config strings, since we no longer need to do any work to
        // restore the session to its default configuration.
        _undoConfigStrings.erase(undoConfig);
    } else {
        // Store the config string that will reset our session to its default configuration.
        _undoConfigStrings.emplace(std::move(undoConfig));
    }
    auto wtSession = getSession();
    invariantWTOK(wtSession->reconfigure(wtSession, newConfig.c_str()), wtSession);
}

void WiredTigerSession::resetSessionConfiguration() {
    auto wtSession = getSession();
    for (const std::string& undoConfigString : _undoConfigStrings) {
        invariantWTOK(wtSession->reconfigure(wtSession, undoConfigString.c_str()), wtSession);
    }
    _undoConfigStrings.clear();
}

namespace {
AtomicWord<unsigned long long> nextTableId(WiredTigerSession::kLastTableId);
}
// static
uint64_t WiredTigerSession::genTableId() {
    return nextTableId.fetchAndAdd(1);
}

// -----------------------

WiredTigerSessionCache::WiredTigerSessionCache(WiredTigerKVEngine* engine)
    : WiredTigerSessionCache(engine->getConnection(), engine->getClockSource(), engine) {}

WiredTigerSessionCache::WiredTigerSessionCache(WT_CONNECTION* conn,
                                               ClockSource* cs,
                                               WiredTigerKVEngine* engine)
    : _conn(conn), _clockSource(cs), _engine(engine) {
    uassertStatusOK(_compiledConfigurations.compileAll(_conn));
}

WiredTigerSessionCache::~WiredTigerSessionCache() {
    shuttingDown();
}

void WiredTigerSessionCache::shuttingDown() {
    // Try to atomically set _shuttingDown flag, but just return if another thread was first.
    if (_shuttingDown.fetchAndBitOr(kShuttingDownMask) & kShuttingDownMask)
        return;

    // Spin as long as there are threads blocking shutdown.
    while (_shuttingDown.load() != kShuttingDownMask) {
        sleepmillis(1);
    }

    closeAll();
}

void WiredTigerSessionCache::restart() {
    _shuttingDown.fetchAndBitAnd(~kShuttingDownMask);
}

bool WiredTigerSessionCache::isShuttingDown() {
    return _shuttingDown.load() & kShuttingDownMask;
}

void WiredTigerSessionCache::waitUntilPreparedUnitOfWorkCommitsOrAborts(
    Interruptible& interruptible, std::uint64_t lastCount) {
    // It is possible for a prepared transaction to block on bonus eviction inside WiredTiger after
    // it commits or rolls-back, but this delays it from signalling us to wake up. In the very
    // worst case that the only evictable page is the one pinned by our cursor, AND there are no
    // other prepared transactions committing or aborting, we could reach a deadlock. Since the
    // caller is already expecting spurious wakeups, we impose a large timeout to periodically force
    // the caller to retry its operation.
    const auto deadline = Date_t::now() + Seconds(1);
    stdx::unique_lock<stdx::mutex> lk(_prepareCommittedOrAbortedMutex);
    if (lastCount == _prepareCommitOrAbortCounter.loadRelaxed()) {
        interruptible.waitForConditionOrInterruptUntil(
            _prepareCommittedOrAbortedCond, lk, deadline, [&] {
                return _prepareCommitOrAbortCounter.loadRelaxed() > lastCount;
            });
    }
}

void WiredTigerSessionCache::notifyPreparedUnitOfWorkHasCommittedOrAborted() {
    stdx::unique_lock<stdx::mutex> lk(_prepareCommittedOrAbortedMutex);
    _prepareCommitOrAbortCounter.fetchAndAdd(1);
    _prepareCommittedOrAbortedCond.notify_all();
}


void WiredTigerSessionCache::closeAllCursors(const std::string& uri) {
    stdx::lock_guard<stdx::mutex> lock(_cacheLock);
    for (SessionCache::iterator i = _sessions.begin(); i != _sessions.end(); i++) {
        (*i)->closeAllCursors(uri);
    }
}

size_t WiredTigerSessionCache::getIdleSessionsCount() {
    stdx::lock_guard<stdx::mutex> lock(_cacheLock);
    return _sessions.size();
}

void WiredTigerSessionCache::closeExpiredIdleSessions(int64_t idleTimeMillis) {
    // Do nothing if session close idle time is set to 0 or less
    if (idleTimeMillis <= 0) {
        return;
    }

    auto cutoffTime = _clockSource->now() - Milliseconds(idleTimeMillis);
    SessionCache sessionsToClose;

    {
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        // Discard all sessions that became idle before the cutoff time
        for (auto it = _sessions.begin(); it != _sessions.end();) {
            auto session = *it;
            invariant(session->getIdleExpireTime() != Date_t::min());
            if (session->getIdleExpireTime() < cutoffTime) {
                it = _sessions.erase(it);
                sessionsToClose.push_back(session);
            } else {
                ++it;
            }
        }
    }

    // Closing expired idle sessions is expensive, so do it outside of the cache mutex. This helps
    // to avoid periodic operation latency spikes as seen in SERVER-52879.
    for (auto session : sessionsToClose) {
        delete session;
    }
}

void WiredTigerSessionCache::closeAll() {
    // Increment the epoch as we are now closing all sessions with this epoch.
    SessionCache swap;

    {
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        _epoch.fetchAndAdd(1);
        _sessions.swap(swap);
    }

    for (SessionCache::iterator i = swap.begin(); i != swap.end(); i++) {
        delete (*i);
    }
}

bool WiredTigerSessionCache::isEphemeral() {
    return _engine && _engine->isEphemeral();
}

UniqueWiredTigerSession WiredTigerSessionCache::getSession() {
    // We should never be able to get here after _shuttingDown is set, because no new
    // operations should be allowed to start.
    invariant(!(_shuttingDown.load() & kShuttingDownMask));

    {
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        if (!_sessions.empty()) {
            // Get the most recently used session so that if we discard sessions, we're
            // discarding older ones
            WiredTigerSession* cachedSession = _sessions.back();
            _sessions.pop_back();
            // Reset the idle time
            cachedSession->setIdleExpireTime(Date_t::min());
            return UniqueWiredTigerSession(cachedSession);
        }
    }

    // Outside of the cache partition lock, but on release will be put back on the cache
    return UniqueWiredTigerSession(new WiredTigerSession(_conn, this, _epoch.load()));
}

void WiredTigerSessionCache::releaseSession(WiredTigerSession* session) {
    invariant(session);
    // We might have skipped releasing some cursors during the shutdown.
    invariant(session->cursorsOut() == 0 || isShuttingDown());

    BlockShutdown blockShutdown(this);

    if (isShuttingDown()) {
        // There is a race condition with clean shutdown, where the storage engine is ripped from
        // underneath OperationContexts, which are not "active" (i.e., do not have any locks), but
        // are just about to delete the recovery unit. See SERVER-16031 for more information. Since
        // shutting down the WT_CONNECTION will close all WT_SESSIONS, we shouldn't also try to
        // directly close this session.
        session->_session = nullptr;  // Prevents calling _session->close() in destructor.
        delete session;
        return;
    }

    {
        WT_SESSION* ss = session->getSession();
        uint64_t range;
        // This checks that we are only caching idle sessions and not something which might hold
        // locks or otherwise prevent truncation.
        invariantWTOK(ss->transaction_pinned_range(ss, &range), ss);
        invariant(range == 0);

        // Release resources in the session we're about to cache.
        // If we are using hybrid caching, then close cursors now and let them
        // be cached at the WiredTiger level.
        if (gWiredTigerCursorCacheSize.load() < 0) {
            session->closeAllCursors("");
        }

        session->resetSessionConfiguration();
        invariantWTOK(ss->reset(ss), ss);
    }

    bool returnedToCache = false;
    uint64_t currentEpoch = _epoch.load();

    // Set the time this session got idle at.
    session->setIdleExpireTime(_clockSource->now());

    if (session->_getEpoch() == currentEpoch) {  // check outside of lock to reduce contention
        stdx::lock_guard<stdx::mutex> lock(_cacheLock);
        if (session->_getEpoch() == _epoch.load()) {  // recheck inside the lock for correctness
            returnedToCache = true;
            _sessions.push_back(session);
        }
    } else
        invariant(session->_getEpoch() < currentEpoch);

    if (!returnedToCache)
        delete session;

    if (_engine) {
        _engine->sizeStorerPeriodicFlush();
    }
}

bool WiredTigerSessionCache::isEngineCachingCursors() {
    return gWiredTigerCursorCacheSize.load() <= 0;
}

void WiredTigerSessionCache::WiredTigerSessionDeleter::operator()(
    WiredTigerSession* session) const {
    session->_cache->releaseSession(session);
}

}  // namespace mongo