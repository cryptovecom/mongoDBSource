/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <string>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

void ReadHedgingModeParameter::append(OperationContext*,
                                      BSONObjBuilder* builder,
                                      StringData name,
                                      const boost::optional<TenantId>&) {
    return;
}

Status ReadHedgingModeParameter::set(const BSONElement& newValueElement,
                                     const boost::optional<TenantId>&) {
    LOGV2_WARNING(
        9206300,
        "Hedged reads have been deprecated and the readHedgingMode parameter has no effect. "
        "For more information please see "
        "https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

Status ReadHedgingModeParameter::setFromString(StringData modeStr,
                                               const boost::optional<TenantId>&) {
    LOGV2_WARNING(
        9206301,
        "Hedged reads have been deprecated and the readHedgingMode parameter has no effect. "
        "For more information please see "
        "https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

void MaxTimeMSForHedgedReadsParameter::append(OperationContext*,
                                              BSONObjBuilder* builder,
                                              StringData name,
                                              const boost::optional<TenantId>&) {
    return;
}

Status MaxTimeMSForHedgedReadsParameter::set(const BSONElement& newValueElement,
                                             const boost::optional<TenantId>&) {
    LOGV2_WARNING(9206302,
                  "Hedged reads have been deprecated and the maxTimeMSForHedgedReads parameter has "
                  "no effect. "
                  "For more information please see "
                  "https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

Status MaxTimeMSForHedgedReadsParameter::setFromString(StringData modeStr,
                                                       const boost::optional<TenantId>&) {
    LOGV2_WARNING(9206303,
                  "Hedged reads have been deprecated and the maxTimeMSForHedgedReads parameter has "
                  "no effect. "
                  "For more information please see "
                  "https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

}  // namespace mongo