/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * Interface to separate router role and shard role implementations.
 */
class SearchIndexProcessInterface {
public:
    virtual ~SearchIndexProcessInterface() = default;

    static SearchIndexProcessInterface* get(Service* service);
    static SearchIndexProcessInterface* get(OperationContext* opCtx);

    static void set(Service* service, std::unique_ptr<SearchIndexProcessInterface> impl);
    /*
     * TODO SERVER-93637 remove fetchCollectionUUIDOrThrow and fetchCollectionUUID from the
     * interface and all derived classes once all search index commands can support sharded views.
     */

    /**
     * Returns the collection UUID or throws a NamespaceNotFound error.
     */
    virtual UUID fetchCollectionUUIDOrThrow(OperationContext* opCtx,
                                            const NamespaceString& nss) = 0;

    /**
     * Returns the collection UUID or boost::none if no collection is found.
     */
    virtual boost::optional<UUID> fetchCollectionUUID(OperationContext* opCtx,
                                                      const NamespaceString& nss) = 0;

    /**
     * Returns the collection UUID and optionally an underlying NSS (if query is on a view). If no
     * UUID, throws a NamespaceNotFound error.
     */
    virtual std::pair<UUID, boost::optional<NamespaceString>>
    fetchCollectionUUIDAndResolveViewOrThrow(OperationContext* opCtx,
                                             const NamespaceString& nss) = 0;
    /**
     * Returns the collection UUID (or boost::none if no collection is found) and the underlying
     * source collection NSS if query is on a view (or boost::none if query is on a normal
     * collection).
     */
    virtual std::pair<boost::optional<UUID>, boost::optional<NamespaceString>>
    fetchCollectionUUIDAndResolveView(OperationContext* opCtx, const NamespaceString& nss) = 0;
};

}  // namespace mongo