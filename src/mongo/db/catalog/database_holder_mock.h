
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

#pragma once

#include <set>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class Database;
class OperationContext;

/**
 * Registry of opened databases.
 */
class DatabaseHolderMock : public DatabaseHolder::Impl {
public:
    DatabaseHolderMock() = default;

    /**
     * Retrieves an already opened database or returns nullptr. Must be called with the database
     * locked in at least IS-mode.
     */
    Database* get(OperationContext* opCtx, StringData ns) const override {
        return nullptr;
    }

    /**
     * Retrieves a database reference if it is already opened, or opens it if it hasn't been
     * opened/created yet. Must be called with the database locked in X-mode.
     *
     * justCreated Returns whether the database was newly created (true) or it already
     * existed (false). Can be nullptr if this information is not necessary.
     */
    Database* openDb(OperationContext* opCtx, StringData ns, bool* justCreated = nullptr) override {
        return nullptr;
    }

    /**
     * Closes the specified database. Must be called with the database locked in X-mode.
     */
    void close(OperationContext* opCtx, StringData ns, const std::string& reason) override {}

    /**
     * Closes all opened databases. Must be called with the global lock acquired in X-mode.
     *
     * reason The reason for close.
     */
    void closeAll(OperationContext* opCtx, const std::string& reason) override {}

    /**
     * Returns the set of existing database names that differ only in casing.
     */
    std::set<std::string> getNamesWithConflictingCasing(StringData name) override {
        return std::set<std::string>();
    }
};
}  // namespace mongo
