/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <cstddef>
#include <iosfwd>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/repl/collection_bulk_loader.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"

namespace mongo {

class Collection;
struct CollectionOptions;
class OperationContext;

namespace repl {

/**
 * Storage interface used by the replication system to interact with storage.
 * This interface provides seperation of concerns and a place for mocking out test
 * interactions.
 *
 * The grouping of functionality includes general collection helpers, and more specific replication
 * concepts:
 *      * Create Collection and Oplog
 *      * Drop database and all user databases
 *      * Drop a collection
 *      * Insert documents into a collection
 *      * Manage minvalid boundaries and initial sync state
 *
 * ***** MINVALID *****
 * This interface provides helper functions for maintaining a single document in the
 * local.replset.minvalid collection.
 *
 * When a member reaches its minValid optime it is in a consistent state.  Thus, minValid is
 * set as the last step in initial sync.  At the beginning of initial sync, doingInitialSync
 * is appended onto minValid to indicate that initial sync was started but has not yet
 * completed.
 *
 * The document is also updated during "normal" sync. The optime of the last op in each batch is
 * used to set minValid, along with a "begin" field to demark the start and the fact that a batch
 * is active. When the batch is done the "begin" field is removed to indicate that we are in a
 * consistent state when the batch has been fully applied.
 *
 * Example of all fields:
 * { _id:...,
 *      doingInitialSync: true // initial sync is active
 *      ts:..., t:...   // end-OpTime
 *      begin: {ts:..., t:...} // a batch is currently being applied, and not consistent
 * }
 */
class StorageInterface {
    MONGO_DISALLOW_COPYING(StorageInterface);

public:
    // Operation Context binding.
    static StorageInterface* get(ServiceContext* service);
    static StorageInterface* get(ServiceContext& service);
    static StorageInterface* get(OperationContext* txn);
    static void set(ServiceContext* service, std::unique_ptr<StorageInterface> storageInterface);

    // Constructor and Destructor.
    StorageInterface() = default;
    virtual ~StorageInterface() = default;

    virtual void startup() = 0;
    virtual void shutdown() = 0;

    // MinValid and Initial Sync Flag.
    /**
     * Returns true if initial sync was started but has not not completed.
     */
    virtual bool getInitialSyncFlag(OperationContext* txn) const = 0;

    /**
     * Sets the the initial sync flag to record that initial sync has not completed.
     *
     * This operation is durable and waits for durable writes (which will block on
     *journaling/checkpointing).
     */
    virtual void setInitialSyncFlag(OperationContext* txn) = 0;

    /**
     * Clears the the initial sync flag to record that initial sync has completed.
     *
     * This operation is durable and waits for durable writes (which will block on
     *journaling/checkpointing).
     */
    virtual void clearInitialSyncFlag(OperationContext* txn) = 0;

    /**
     * The minValid value is the earliest (minimum) Timestamp that must be applied in order to
     * consider the dataset consistent.
     */
    virtual void setMinValid(OperationContext* txn, const OpTime& minValid) = 0;
    virtual OpTime getMinValid(OperationContext* txn) const = 0;

    /**
     * Sets minValid only if it is not already higher than endOpTime.
     * Warning, this compares the term and timestamp independently. Do not use if the current
     * minValid could be from the other fork of a rollback.
     */
    virtual void setMinValidToAtLeast(OperationContext* txn, const OpTime& endOpTime) = 0;

    /**
     * On startup all oplog entries with a value >= the oplog delete from point should be deleted.
     * If null, no documents should be deleted.
     */
    virtual void setOplogDeleteFromPoint(OperationContext* txn, const Timestamp& timestamp) = 0;
    virtual Timestamp getOplogDeleteFromPoint(OperationContext* txn) = 0;

    /**
     * The applied through point is a persistent record of where we've applied through. If null, the
     * applied through point is the top of the oplog.
     */
    virtual void setAppliedThrough(OperationContext* txn, const OpTime& optime) = 0;

    /**
     * You should probably be calling ReplicationCoordinator::getLastAppliedOpTime() instead.
     *
     * This reads the value from storage which isn't always updated when the ReplicationCoordinator
     * is.
     */
    virtual OpTime getAppliedThrough(OperationContext* txn) = 0;


    // Collection creation and population for initial sync.
    /**
     * Creates a collection with the provided indexes.
     *
     * Assumes that no database locks have been acquired prior to calling this function.
     */
    virtual StatusWith<std::unique_ptr<CollectionBulkLoader>> createCollectionForBulkLoading(
        const NamespaceString& nss,
        const CollectionOptions& options,
        const BSONObj idIndexSpec,
        const std::vector<BSONObj>& secondaryIndexSpecs) = 0;

    /**
     * Inserts a document into a collection.
     *
     * NOTE: If the collection doesn't exist, it will not be created, and instead
     * an error is returned.
     */
    virtual Status insertDocument(OperationContext* txn,
                                  const NamespaceString& nss,
                                  const BSONObj& doc) = 0;

    /**
     * Inserts the given documents into the collection.
     * It is an error to call this function with an empty set of documents.
     */
    virtual Status insertDocuments(OperationContext* txn,
                                   const NamespaceString& nss,
                                   const std::vector<BSONObj>& docs) = 0;

    /**
     * Creates the initial oplog, errors if it exists.
     */
    virtual Status createOplog(OperationContext* txn, const NamespaceString& nss) = 0;

    /**
     * Returns the configured maximum size of the oplog.
     *
     * Implementations are allowed to be "fuzzy" and delete documents when the actual size is
     * slightly above or below this, so callers should not rely on its exact value.
     */
    virtual StatusWith<size_t> getOplogMaxSize(OperationContext* txn,
                                               const NamespaceString& nss) = 0;

    /**
     * Creates a collection.
     */
    virtual Status createCollection(OperationContext* txn,
                                    const NamespaceString& nss,
                                    const CollectionOptions& options) = 0;

    /**
     * Drops a collection, like the oplog.
     */
    virtual Status dropCollection(OperationContext* txn, const NamespaceString& nss) = 0;

    /**
     * Drops all databases except "local".
     */
    virtual Status dropReplicatedDatabases(OperationContext* txn) = 0;

    /**
     * Validates that the admin database is valid during initial sync.
     */
    virtual Status isAdminDbValid(OperationContext* txn) = 0;

    /**
     * Finds at most "limit" documents returned by a collection or index scan on the collection in
     * the requested direction.
     * The documents returned will be copied and buffered. No cursors on the underlying collection
     * will be kept open once this function returns.
     * If "indexName" is boost::none, a collection scan is used to locate the document.
     * Index scan options:
     *     If "startKey" is not empty, the index scan will start from the given key (instead of
     *     MinKey/MaxKey).
     *     Set "boundInclusion" to BoundInclusion::kIncludeStartKeyOnly to include "startKey" in
     *     the index scan results. Set to BoundInclusion::kIncludeEndKeyOnly to return the key
     *     immediately following "startKey" from the index.
     */
    enum class ScanDirection {
        kForward = 1,
        kBackward = -1,
    };
    virtual StatusWith<std::vector<BSONObj>> findDocuments(OperationContext* txn,
                                                           const NamespaceString& nss,
                                                           boost::optional<StringData> indexName,
                                                           ScanDirection scanDirection,
                                                           const BSONObj& startKey,
                                                           BoundInclusion boundInclusion,
                                                           std::size_t limit) = 0;

    /**
     * Deletes at most "limit" documents returned by a collection or index scan on the collection in
     * the requested direction. Returns deleted documents on success.
     * The documents returned will be copied and buffered. No cursors on the underlying collection
     * will be kept open once this function returns.
     * If "indexName" is null, a collection scan is used to locate the document.
     */
    virtual StatusWith<std::vector<BSONObj>> deleteDocuments(OperationContext* txn,
                                                             const NamespaceString& nss,
                                                             boost::optional<StringData> indexName,
                                                             ScanDirection scanDirection,
                                                             const BSONObj& startKey,
                                                             BoundInclusion boundInclusion,
                                                             std::size_t limit) = 0;
};

}  // namespace repl
}  // namespace mongo
