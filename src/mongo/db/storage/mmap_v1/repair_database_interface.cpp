
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

#include "mongo/db/storage/mmap_v1/repair_database_interface.h"

namespace mongo {
namespace {
stdx::function<Status(StorageEngine*, OperationContext*, const std::string&, bool, bool)>
    repairDatabaseMmapv1Func;
}  // namespace

void setRepairDatabaseMmapv1Impl(
    stdx::function<Status(StorageEngine* engine, OperationContext*, const std::string&, bool, bool)>
        impl) {
    repairDatabaseMmapv1Func = std::move(impl);
}

Status repairDatabaseMmapv1(StorageEngine* engine,
                            OperationContext* opCtx,
                            const std::string& dbName,
                            bool preserveClonedFilesOnFailure,
                            bool backupOriginalFiles) {
    return repairDatabaseMmapv1Func(
        engine, opCtx, dbName, preserveClonedFilesOnFailure, backupOriginalFiles);
}
}  // namespace mongo
