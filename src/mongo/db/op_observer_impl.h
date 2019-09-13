
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

#include "mongo/db/op_observer.h"

namespace mongo {

class OpObserverImpl : public OpObserver {
    MONGO_DISALLOW_COPYING(OpObserverImpl);

public:
    OpObserverImpl() = default;
    virtual ~OpObserverImpl() = default;

    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       OptionalCollectionUUID uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) final;
    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) final;
    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) final;
    void aboutToDelete(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const BSONObj& doc) final;
    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  OptionalCollectionUUID uuid,
                  StmtId stmtId,
                  bool fromMigrate,
                  const boost::optional<BSONObj>& deletedDoc) final;
    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID> uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj) final;
    void onCreateCollection(OperationContext* opCtx,
                            Collection* coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime) final;
    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<TTLCollModInfo> ttlInfo) final;
    void onDropDatabase(OperationContext* opCtx, const std::string& dbName) final;
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid) final;
    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     OptionalCollectionUUID uuid,
                     const std::string& indexName,
                     const BSONObj& indexInfo) final;
    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     OptionalCollectionUUID uuid,
                                     OptionalCollectionUUID dropTargetUUID,
                                     bool stayTemp) final;
    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              OptionalCollectionUUID uuid,
                              OptionalCollectionUUID dropTargetUUID,
                              bool stayTemp) final;
    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            OptionalCollectionUUID uuid,
                            OptionalCollectionUUID dropTargetUUID,
                            bool stayTemp) final;
    void onApplyOps(OperationContext* opCtx,
                    const std::string& dbName,
                    const BSONObj& applyOpCmd) final;
    void onEmptyCapped(OperationContext* opCtx,
                       const NamespaceString& collectionName,
                       OptionalCollectionUUID uuid);
    void onTransactionCommit(OperationContext* opCtx) final;
    void onTransactionPrepare(OperationContext* opCtx) final;
    void onTransactionAbort(OperationContext* opCtx) final;
    void onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo) final;

    // Contains the fields of the document that are in the collection's shard key, and "_id".
    static BSONObj getDocumentKey(OperationContext* opCtx,
                                  NamespaceString const& nss,
                                  BSONObj const& doc);

private:
    virtual void shardObserveAboutToDelete(OperationContext* opCtx,
                                           NamespaceString const& nss,
                                           BSONObj const& doc) {}
    virtual void shardObserveInsertOp(OperationContext* opCtx,
                                      const NamespaceString nss,
                                      const BSONObj& insertedDoc,
                                      const repl::OpTime& opTime,
                                      const bool fromMigrate) {}
    virtual void shardObserveUpdateOp(OperationContext* opCtx,
                                      const NamespaceString nss,
                                      const BSONObj& updatedDoc,
                                      const repl::OpTime& opTime,
                                      const repl::OpTime& prePostImageOpTime) {}
    virtual void shardObserveDeleteOp(OperationContext* opCtx,
                                      const NamespaceString nss,
                                      const BSONObj& documentKey,
                                      const repl::OpTime& opTime,
                                      const repl::OpTime& preImageOpTime) {}
};

}  // namespace mongo
