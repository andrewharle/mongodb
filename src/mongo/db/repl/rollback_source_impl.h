
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

#include <string>

#include "mongo/db/repl/oplog_interface_remote.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class DBClientBase;

namespace repl {

/**
 * Rollback source implementation using a connection.
 */

class RollbackSourceImpl : public RollbackSource {
public:
    /**
     * Type of function to return a connection to the sync source.
     */
    using GetConnectionFn = stdx::function<DBClientBase*()>;

    RollbackSourceImpl(GetConnectionFn getConnection,
                       const HostAndPort& source,
                       const std::string& collectionName,
                       int batchSize);

    const OplogInterface& getOplog() const override;

    const HostAndPort& getSource() const override;

    int getRollbackId() const override;

    BSONObj getLastOperation() const override;

    BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const override;

    std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                      UUID uuid,
                                                      const BSONObj& filter) const override;

    void copyCollectionFromRemote(OperationContext* opCtx,
                                  const NamespaceString& nss) const override;

    StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db,
                                                const UUID& uuid) const override;

    StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const override;

private:
    GetConnectionFn _getConnection;
    HostAndPort _source;
    std::string _collectionName;
    OplogInterfaceRemote _oplog;
};


}  // namespace repl
}  // namespace mongo
