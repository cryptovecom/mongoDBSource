/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <map>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {

/**
 * Utility functions to get or set boolean flags from/to a storage engine options object
 * (see `CollectionOptions::storageEngine`).
 *
 * The idea is that for exceptional (workaround) purposes, we can use the storage engine
 * options object as a flexible structure where new fields can be added retroactively,
 * unlike the other parts of the catalog which generally have non-flexible / strict validations.
 * For more information, see: SERVER-91195, SERVER-92186.
 */

std::map<StringData, boost::optional<bool>> getFlagsFromStorageEngineBson(
    const BSONObj& storageEngineOptions, const std::vector<StringData>& flagNames);

boost::optional<bool> getFlagFromStorageEngineBson(const BSONObj& storageEngineOptions,
                                                   StringData flagName);

[[nodiscard]] BSONObj setFlagsToStorageEngineBson(
    const BSONObj& storageEngineOptions, const std::map<StringData, boost::optional<bool>>& flags);

[[nodiscard]] BSONObj setFlagToStorageEngineBson(const BSONObj& storageEngineOptions,
                                                 StringData flagName,
                                                 boost::optional<bool> flagValue);

}  // namespace mongo