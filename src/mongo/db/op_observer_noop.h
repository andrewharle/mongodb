/**
 *    Copyright 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

class OpObserverNoop : public OpObserver {
    MONGO_DISALLOW_COPYING(OpObserverNoop);

public:
    OpObserverNoop() = default;
    virtual ~OpObserverNoop() = default;

    void onCreateIndex(OperationContext* txn,
                       const std::string& ns,
                       BSONObj indexDoc,
                       bool fromMigrate) override;
    void onInserts(OperationContext* txn,
                   const NamespaceString& ns,
                   std::vector<BSONObj>::const_iterator begin,
                   std::vector<BSONObj>::const_iterator end,
                   bool fromMigrate) override;
    void onUpdate(OperationContext* txn, const OplogUpdateEntryArgs& args) override;
    CollectionShardingState::DeleteState aboutToDelete(OperationContext* txn,
                                                       const NamespaceString& ns,
                                                       const BSONObj& doc) override;
    void onDelete(OperationContext* txn,
                  const NamespaceString& ns,
                  CollectionShardingState::DeleteState deleteState,
                  bool fromMigrate) override;
    void onOpMessage(OperationContext* txn, const BSONObj& msgObj) override;
    void onCreateCollection(OperationContext* txn,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex) override;
    void onCollMod(OperationContext* txn,
                   const std::string& dbName,
                   const BSONObj& collModCmd) override;
    void onDropDatabase(OperationContext* txn, const std::string& dbName) override;
    void onDropCollection(OperationContext* txn, const NamespaceString& collectionName) override;
    void onDropIndex(OperationContext* txn,
                     const std::string& dbName,
                     const BSONObj& idxDescriptor) override;
    void onRenameCollection(OperationContext* txn,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            bool dropTarget,
                            bool stayTemp) override;
    void onApplyOps(OperationContext* txn,
                    const std::string& dbName,
                    const BSONObj& applyOpCmd) override;
    void onEmptyCapped(OperationContext* txn, const NamespaceString& collectionName);
    void onConvertToCapped(OperationContext* txn,
                           const NamespaceString& collectionName,
                           double size) override;
};

}  // namespace mongo
