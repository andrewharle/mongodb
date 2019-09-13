
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/kv/kv_database_catalog_entry.h"

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

auto mongo::defaultDatabaseCatalogEntryFactory(const StringData name, KVStorageEngine* const engine)
    -> std::unique_ptr<KVDatabaseCatalogEntryBase> {
    return stdx::make_unique<KVDatabaseCatalogEntry>(name, engine);
}

namespace mongo {

IndexAccessMethod* KVDatabaseCatalogEntry::getIndex(OperationContext* opCtx,
                                                    const CollectionCatalogEntry* collection,
                                                    IndexCatalogEntry* index) {
    IndexDescriptor* desc = index->descriptor();

    const std::string& type = desc->getAccessMethodName();

    std::string ident =
        _engine->getCatalog()->getIndexIdent(opCtx, collection->ns().ns(), desc->indexName());

    SortedDataInterface* sdi =
        _engine->getEngine()->getGroupedSortedDataInterface(opCtx, ident, desc, index->getPrefix());

    if ("" == type)
        return new BtreeAccessMethod(index, sdi);

    if (IndexNames::HASHED == type)
        return new HashAccessMethod(index, sdi);

    if (IndexNames::GEO_2DSPHERE == type)
        return new S2AccessMethod(index, sdi);

    if (IndexNames::TEXT == type)
        return new FTSAccessMethod(index, sdi);

    if (IndexNames::GEO_HAYSTACK == type)
        return new HaystackAccessMethod(index, sdi);

    if (IndexNames::GEO_2D == type)
        return new TwoDAccessMethod(index, sdi);

    log() << "Can't find index for keyPattern " << desc->keyPattern();
    MONGO_UNREACHABLE;
}
}  // namespace mongo
