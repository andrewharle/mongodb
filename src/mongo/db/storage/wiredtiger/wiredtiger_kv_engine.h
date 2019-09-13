// wiredtiger_kv_engine.h


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

#include <list>
#include <memory>
#include <string>

#include <boost/filesystem/path.hpp>
#include <wiredtiger.h>

#include "mongo/bson/ordering.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

class ClockSource;
class JournalListener;
class WiredTigerRecordStore;
class WiredTigerSessionCache;
class WiredTigerSizeStorer;

struct WiredTigerFileVersion {
    enum class StartupVersion { IS_34, IS_36, IS_40 };

    StartupVersion _startupVersion;
    bool shouldDowngrade(bool readOnly, bool repairMode, bool hasRecoveryTimestamp);
    std::string getDowngradeString();
};

class WiredTigerKVEngine final : public KVEngine {
public:
    static const int kDefaultJournalDelayMillis;
    WiredTigerKVEngine(const std::string& canonicalName,
                       const std::string& path,
                       ClockSource* cs,
                       const std::string& extraOpenOptions,
                       size_t cacheSizeMB,
                       size_t maxCacheOverflowFileSizeMB,
                       bool durable,
                       bool ephemeral,
                       bool repair,
                       bool readOnly);

    virtual ~WiredTigerKVEngine();

    void setRecordStoreExtraOptions(const std::string& options);
    void setSortedDataInterfaceExtraOptions(const std::string& options);

    virtual bool supportsDocLocking() const override;

    virtual bool supportsDirectoryPerDB() const override;

    virtual bool isDurable() const override {
        return _durable;
    }

    virtual bool isEphemeral() const override {
        return _ephemeral;
    }

    virtual RecoveryUnit* newRecoveryUnit() override;

    virtual Status createRecordStore(OperationContext* opCtx,
                                     StringData ns,
                                     StringData ident,
                                     const CollectionOptions& options) override {
        return createGroupedRecordStore(opCtx, ns, ident, options, KVPrefix::kNotPrefixed);
    }

    virtual std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                        StringData ns,
                                                        StringData ident,
                                                        const CollectionOptions& options) override {
        return getGroupedRecordStore(opCtx, ns, ident, options, KVPrefix::kNotPrefixed);
    }

    virtual Status createSortedDataInterface(OperationContext* opCtx,
                                             StringData ident,
                                             const IndexDescriptor* desc) override {
        return createGroupedSortedDataInterface(opCtx, ident, desc, KVPrefix::kNotPrefixed);
    }

    virtual SortedDataInterface* getSortedDataInterface(OperationContext* opCtx,
                                                        StringData ident,
                                                        const IndexDescriptor* desc) override {
        return getGroupedSortedDataInterface(opCtx, ident, desc, KVPrefix::kNotPrefixed);
    }

    virtual Status createGroupedRecordStore(OperationContext* opCtx,
                                            StringData ns,
                                            StringData ident,
                                            const CollectionOptions& options,
                                            KVPrefix prefix) override;

    virtual std::unique_ptr<RecordStore> getGroupedRecordStore(OperationContext* opCtx,
                                                               StringData ns,
                                                               StringData ident,
                                                               const CollectionOptions& options,
                                                               KVPrefix prefix) override;

    virtual Status createGroupedSortedDataInterface(OperationContext* opCtx,
                                                    StringData ident,
                                                    const IndexDescriptor* desc,
                                                    KVPrefix prefix) override;

    virtual SortedDataInterface* getGroupedSortedDataInterface(OperationContext* opCtx,
                                                               StringData ident,
                                                               const IndexDescriptor* desc,
                                                               KVPrefix prefix) override;

    virtual Status dropIdent(OperationContext* opCtx, StringData ident) override;

    virtual void alterIdentMetadata(OperationContext* opCtx,
                                    StringData ident,
                                    const IndexDescriptor* desc) override;

    virtual Status okToRename(OperationContext* opCtx,
                              StringData fromNS,
                              StringData toNS,
                              StringData ident,
                              const RecordStore* originalRecordStore) const override;

    virtual int flushAllFiles(OperationContext* opCtx, bool sync) override;

    virtual Status beginBackup(OperationContext* opCtx);

    virtual void endBackup(OperationContext* opCtx);

    virtual int64_t getIdentSize(OperationContext* opCtx, StringData ident) override;

    virtual Status repairIdent(OperationContext* opCtx, StringData ident) override;

    virtual Status recoverOrphanedIdent(OperationContext* opCtx,
                                        StringData ns,
                                        StringData ident,
                                        const CollectionOptions& options) override;

    virtual bool hasIdent(OperationContext* opCtx, StringData ident) const override;

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const override;

    virtual void cleanShutdown() override;

    SnapshotManager* getSnapshotManager() const final {
        return &_sessionCache->snapshotManager();
    }

    void setJournalListener(JournalListener* jl) final;

    virtual void setStableTimestamp(Timestamp stableTimestamp) override;

    virtual void setInitialDataTimestamp(Timestamp initialDataTimestamp) override;

    /**
     * This method will set the oldest timestamp and commit timestamp to the input value. Callers
     * must be serialized along with `setStableTimestamp`. If force=false, this function does not
     * set the commit timestamp and may choose to lag the oldest timestamp.
     */
    void setOldestTimestamp(Timestamp oldestTimestamp, bool force) override;

    virtual bool supportsRecoverToStableTimestamp() const override;

    virtual bool supportsRecoveryTimestamp() const override;

    virtual StatusWith<Timestamp> recoverToStableTimestamp(OperationContext* opCtx) override;

    virtual boost::optional<Timestamp> getRecoveryTimestamp() const override;

    /**
     * Returns a timestamp value that is at or before the last checkpoint. Everything before this
     * value is guaranteed to be persisted on disk and replication recovery will not need to
     * replay documents with an earlier time.
     */
    virtual boost::optional<Timestamp> getLastStableCheckpointTimestamp() const override;

    virtual Timestamp getAllCommittedTimestamp() const override;

    bool supportsReadConcernSnapshot() const final;

    virtual Timestamp getOldestOpenReadTimestamp() const override;

    bool supportsReadConcernMajority() const final;

    // wiredtiger specific
    // Calls WT_CONNECTION::reconfigure on the underlying WT_CONNECTION
    // held by this class
    int reconfigure(const char* str);

    WT_CONNECTION* getConnection() {
        return _conn;
    }
    void dropSomeQueuedIdents();
    std::list<WiredTigerCachedCursor> filterCursorsWithQueuedDrops(
        std::list<WiredTigerCachedCursor>* cache);
    bool haveDropsQueued() const;

    void syncSizeInfo(bool sync) const;

    /*
     * An oplog manager is always accessible, but this method will start the background thread to
     * control oplog entry visibility for reads.
     *
     * On mongod, the background thread will be started when the first oplog record store is
     * created, and stopped when the last oplog record store is destroyed, at shutdown time. For
     * unit tests, the background thread may be started and stopped multiple times as tests create
     * and destroy oplog record stores.
     */
    void startOplogManager(OperationContext* opCtx,
                           const std::string& uri,
                           WiredTigerRecordStore* oplogRecordStore);
    void haltOplogManager();

    /*
     * Always returns a non-nil pointer. However, the WiredTigerOplogManager may not have been
     * initialized and its background refreshing thread may not be running.
     *
     * A caller that wants to get the oplog read timestamp, or call
     * `waitForAllEarlierOplogWritesToBeVisible`, is advised to first see if the oplog manager is
     * running with a call to `isRunning`.
     *
     * A caller that simply wants to call `triggerJournalFlush` may do so without concern.
     */
    WiredTigerOplogManager* getOplogManager() const {
        return _oplogManager.get();
    }

    /*
     * This function is called when replication has completed a batch.  In this function, we
     * refresh our oplog visiblity read-at-timestamp value.
     */
    void replicationBatchIsComplete() const override;

    /**
     * Sets the implementation for `initRsOplogBackgroundThread` (allowing tests to skip the
     * background job, for example). Intended to be called from a MONGO_INITIALIZER and therefroe in
     * a single threaded context.
     */
    static void setInitRsOplogBackgroundThreadCallback(stdx::function<bool(StringData)> cb);

    /**
     * Initializes a background job to remove excess documents in the oplog collections.
     * This applies to the capped collections in the local.oplog.* namespaces (specifically
     * local.oplog.rs for replica sets).
     * Returns true if a background job is running for the namespace.
     */
    static bool initRsOplogBackgroundThread(StringData ns);

    static void appendGlobalStats(BSONObjBuilder& b);

    Timestamp getOldestTimestamp() const;

    /**
     * Returns the data file path associated with an ident on disk. Returns boost::none if the data
     * file can not be found. This will attempt to locate a file even if the storage engine's own
     * metadata is not aware of the ident. This is intented for database repair purposes only.
     */
    boost::optional<boost::filesystem::path> getDataFilePathForIdent(StringData ident) const;

    ClockSource* getClockSource() const {
        return _clockSource;
    }

private:
    class WiredTigerSessionSweeper;
    class WiredTigerJournalFlusher;
    class WiredTigerCheckpointThread;

    /**
     * Opens a connection on the WiredTiger database 'path' with the configuration 'wtOpenConfig'.
     * Only returns when successful. Intializes both '_conn' and '_fileVersion'.
     *
     * If corruption is detected and _inRepairMode is 'true', attempts to salvage the WiredTiger
     * metadata.
     */
    void _openWiredTiger(const std::string& path, const std::string& wtOpenConfig);

    Status _salvageIfNeeded(const char* uri);
    void _ensureIdentPath(StringData ident);

    /**
     * Recreates a WiredTiger ident from the provided URI by dropping and recreating the ident.
     * This moves aside the existing data file, if one exists, with an added ".corrupt" suffix.
     *
     * Returns DataModifiedByRepair if the rebuild was successful, and any other error on failure.
     * This will never return Status::OK().
     */
    Status _rebuildIdent(WT_SESSION* session, const char* uri);

    bool _hasUri(WT_SESSION* session, const std::string& uri) const;

    std::string _uri(StringData ident) const;

    WT_CONNECTION* _conn;
    WiredTigerFileVersion _fileVersion;
    WiredTigerEventHandler _eventHandler;
    std::unique_ptr<WiredTigerSessionCache> _sessionCache;
    ClockSource* const _clockSource;

    // Mutex to protect use of _oplogManagerCount by this instance of KV engine.
    mutable stdx::mutex _oplogManagerMutex;
    std::size_t _oplogManagerCount = 0;
    std::unique_ptr<WiredTigerOplogManager> _oplogManager;

    std::string _canonicalName;
    std::string _path;
    std::string _wtOpenConfig;

    std::unique_ptr<WiredTigerSizeStorer> _sizeStorer;
    std::string _sizeStorerUri;
    mutable ElapsedTracker _sizeStorerSyncTracker;

    bool _durable;
    bool _ephemeral;
    const bool _inRepairMode;
    bool _readOnly;

    // If _keepDataHistory is true, then the storage engine keeps all history after the stable
    // timestamp, and WiredTigerKVEngine is responsible for advancing the oldest timestamp. If
    // _keepDataHistory is false (i.e. majority reads are disabled), then we only keep history after
    // the "no holes point", and WiredTigerOplogManager is responsible for advancing the oldest
    // timestamp.
    const bool _keepDataHistory = true;

    std::unique_ptr<WiredTigerSessionSweeper> _sessionSweeper;
    std::unique_ptr<WiredTigerJournalFlusher> _journalFlusher;  // Depends on _sizeStorer
    std::unique_ptr<WiredTigerCheckpointThread> _checkpointThread;

    std::string _rsOptions;
    std::string _indexOptions;

    mutable stdx::mutex _dropAllQueuesMutex;
    mutable stdx::mutex _identToDropMutex;
    std::list<std::string> _identToDrop;

    mutable Date_t _previousCheckedDropsQueued;

    std::unique_ptr<WiredTigerSession> _backupSession;
    Timestamp _recoveryTimestamp;

    // Tracks the stable and oldest timestamps we've set on the storage engine.
    AtomicWord<std::uint64_t> _oldestTimestamp;
    AtomicWord<std::uint64_t> _stableTimestamp;
    AtomicWord<std::uint64_t> _oplogNeededForRollback{Timestamp::min().asULL()};

    // Timestamp of data at startup. Used internally to advise checkpointing and recovery to a
    // timestamp. Provided by replication layer because WT does not persist timestamps.
    AtomicWord<std::uint64_t> _initialDataTimestamp;
};
}
