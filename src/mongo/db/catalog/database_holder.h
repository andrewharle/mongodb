
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

#include "mongo/base/shim.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/string_map.h"

namespace mongo {
class Database;
class OperationContext;

/**
 * Registry of opened databases.
 */
class DatabaseHolder {
public:
    class Impl {
    public:
        virtual ~Impl() = 0;

        virtual Database* get(OperationContext* opCtx, StringData ns) const = 0;

        virtual Database* openDb(OperationContext* opCtx, StringData ns, bool* justCreated) = 0;

        virtual void close(OperationContext* opCtx, StringData ns, const std::string& reason) = 0;

        virtual void closeAll(OperationContext* opCtx, const std::string& reason) = 0;

        virtual std::set<std::string> getNamesWithConflictingCasing(StringData name) = 0;
    };

public:
    static MONGO_DECLARE_SHIM(()->DatabaseHolder&) getDatabaseHolder;

    static MONGO_DECLARE_SHIM((PrivateTo<DatabaseHolder>)->std::unique_ptr<Impl>) makeImpl;

    inline ~DatabaseHolder() = default;

    inline explicit DatabaseHolder() : _pimpl(makeImpl(PrivateCall<DatabaseHolder>{})) {}

    /**
     * Retrieves an already opened database or returns NULL. Must be called with the database
     * locked in at least IS-mode.
     */
    inline Database* get(OperationContext* const opCtx, const StringData ns) const {
        return this->_impl().get(opCtx, ns);
    }

    /**
     * Retrieves a database reference if it is already opened, or opens it if it hasn't been
     * opened/created yet. Must be called with the database locked in X-mode.
     *
     * @param justCreated Returns whether the database was newly created (true) or it already
     *          existed (false). Can be NULL if this information is not necessary.
     */
    inline Database* openDb(OperationContext* const opCtx,
                            const StringData ns,
                            bool* const justCreated = nullptr) {
        return this->_impl().openDb(opCtx, ns, justCreated);
    }

    /**
     * Closes the specified database. Must be called with the database locked in X-mode.
     * No background jobs must be in progress on the database when this function is called.
     */
    inline void close(OperationContext* const opCtx,
                      const StringData ns,
                      const std::string& reason) {
        return this->_impl().close(opCtx, ns, reason);
    }

    /**
     * Closes all opened databases. Must be called with the global lock acquired in X-mode.
     * Will uassert if any background jobs are running when this function is called.
     *
     * @param reason The reason for close.
     */
    inline void closeAll(OperationContext* const opCtx, const std::string& reason) {
        this->_impl().closeAll(opCtx, reason);
    }

    /**
     * Returns the set of existing database names that differ only in casing.
     */
    inline std::set<std::string> getNamesWithConflictingCasing(const StringData name) {
        return this->_impl().getNamesWithConflictingCasing(name);
    }

private:
    // This structure exists to give us a customization point to decide how to force users of this
    // class to depend upon the corresponding `database_holder.cpp` Translation Unit (TU).  All
    // public forwarding functions call `_impl(), and `_impl` creates an instance of this structure.
    struct TUHook {
        static void hook() noexcept;

        explicit inline TUHook() noexcept {
            if (kDebugBuild)
                this->hook();
        }
    };

    inline const Impl& _impl() const {
        TUHook{};
        return *this->_pimpl;
    }

    inline Impl& _impl() {
        TUHook{};
        return *this->_pimpl;
    }

    std::unique_ptr<Impl> _pimpl;
};
}  // namespace mongo
