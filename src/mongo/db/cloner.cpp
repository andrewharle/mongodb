// cloner.cpp - copy a database (export/import basically)

/**
*    Copyright (C) 2008 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/cloner.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/initial_sync_common.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using repl::initialSyncHangDuringCollectionClone;
using std::endl;
using std::list;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

MONGO_EXPORT_SERVER_PARAMETER(skipCorruptDocumentsWhenCloning, bool, false);

BSONElement getErrField(const BSONObj& o);

namespace {

/* for index info object:
     { "name" : "name_1" , "ns" : "foo.index3" , "key" :  { "name" : 1.0 } }
   we need to fix up the value in the "ns" parameter so that the name prefix is correct on a
   copy to a new name.
*/
BSONObj fixIndexSpec(const string& newDbName, BSONObj indexSpec) {
    BSONObjBuilder bob;

    for (auto&& indexSpecElem : indexSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (IndexDescriptor::kIndexVersionFieldName == indexSpecElemFieldName) {
            IndexVersion indexVersion = static_cast<IndexVersion>(indexSpecElem.numberInt());
            if (IndexVersion::kV0 == indexVersion) {
                // We automatically upgrade v=0 indexes to v=1 indexes.
                bob.append(IndexDescriptor::kIndexVersionFieldName,
                           static_cast<int>(IndexVersion::kV1));
            } else {
                bob.append(IndexDescriptor::kIndexVersionFieldName, static_cast<int>(indexVersion));
            }
        } else if (IndexDescriptor::kNamespaceFieldName == indexSpecElemFieldName) {
            uassert(10024, "bad ns field for index during dbcopy", indexSpecElem.type() == String);
            const char* p = strchr(indexSpecElem.valuestr(), '.');
            uassert(10025, "bad ns field for index during dbcopy [2]", p);
            string newname = newDbName + p;
            bob.append(IndexDescriptor::kNamespaceFieldName, newname);
        } else {
            bob.append(indexSpecElem);
        }
    }

    return bob.obj();
}
}  // namespace

BSONObj Cloner::getIdIndexSpec(const std::list<BSONObj>& indexSpecs) {
    for (auto&& indexSpec : indexSpecs) {
        BSONElement indexName;
        uassertStatusOK(bsonExtractTypedField(
            indexSpec, IndexDescriptor::kIndexNameFieldName, String, &indexName));
        if (indexName.valueStringData() == "_id_"_sd) {
            return indexSpec;
        }
    }
    return BSONObj();
}

Cloner::Cloner() {}

struct Cloner::Fun {
    Fun(OperationContext* txn, const string& dbName) : lastLog(0), txn(txn), _dbName(dbName) {}

    void operator()(DBClientCursorBatchIterator& i) {
        invariant(from_collection.coll() != "system.indexes");

        // XXX: can probably take dblock instead
        unique_ptr<ScopedTransaction> scopedXact(new ScopedTransaction(txn, MODE_X));
        unique_ptr<Lock::GlobalWrite> globalWriteLock(new Lock::GlobalWrite(txn->lockState()));
        uassert(ErrorCodes::NotMaster,
                str::stream() << "Not primary while cloning collection " << from_collection.ns()
                              << " to "
                              << to_collection.ns(),
                !txn->writesAreReplicated() ||
                    repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(to_collection));

        // Make sure database still exists after we resume from the temp release
        Database* db = dbHolder().openDb(txn, _dbName);

        bool createdCollection = false;
        Collection* collection = NULL;

        collection = db->getCollection(to_collection);
        if (!collection) {
            massert(17321,
                    str::stream() << "collection dropped during clone [" << to_collection.ns()
                                  << "]",
                    !createdCollection);
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                txn->checkForInterrupt();

                WriteUnitOfWork wunit(txn);
                Status s = userCreateNS(txn,
                                        db,
                                        to_collection.toString(),
                                        from_options,
                                        true,
                                        fixIndexSpec(to_collection.db().toString(), from_id_index));
                verify(s.isOK());
                wunit.commit();
                collection = db->getCollection(to_collection);
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", to_collection.ns());
        }

        const bool isSystemViewsClone = to_collection.isSystemDotViews();

        while (i.moreInCurrentBatch()) {
            if (numSeen % 128 == 127) {
                time_t now = time(0);
                if (now - lastLog >= 60) {
                    // report progress
                    if (lastLog)
                        log() << "clone " << to_collection << ' ' << numSeen;
                    lastLog = now;
                }
                txn->checkForInterrupt();

                scopedXact.reset();
                globalWriteLock.reset();

                CurOp::get(txn)->yielded();

                scopedXact.reset(new ScopedTransaction(txn, MODE_X));
                globalWriteLock.reset(new Lock::GlobalWrite(txn->lockState()));

                // Check if everything is still all right.
                if (txn->writesAreReplicated()) {
                    uassert(
                        28592,
                        str::stream() << "Cannot write to ns: " << to_collection.ns()
                                      << " after yielding",
                        repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(to_collection));
                }

                // TODO: SERVER-16598 abort if original db or collection is gone.
                db = dbHolder().get(txn, _dbName);
                uassert(28593,
                        str::stream() << "Database " << _dbName << " dropped while cloning",
                        db != NULL);

                collection = db->getCollection(to_collection);
                uassert(28594,
                        str::stream() << "Collection " << to_collection.ns()
                                      << " dropped while cloning",
                        collection != NULL);
            }

            BSONObj tmp = i.nextSafe();

            // If copying the system.views collection to a database with a different name, then any
            // view definitions must be modified to refer to the 'to' database.
            if (isSystemViewsClone && from_collection.db() != to_collection.db()) {
                BSONObjBuilder bob;
                for (auto&& item : tmp) {
                    if (item.fieldNameStringData() == "_id") {
                        auto viewNss = NamespaceString(item.checkAndGetStringData());

                        bob.append("_id",
                                   NamespaceString(to_collection.db(), viewNss.coll()).toString());
                    } else {
                        bob.append(item);
                    }
                }
                tmp = bob.obj();
            }

            /* assure object is valid.  note this will slow us down a little. */
            // Use the latest BSON validation version. We allow cloning of collections containing
            // decimal data even if decimal is disabled.
            const Status status = validateBSON(tmp.objdata(), tmp.objsize(), BSONVersion::kLatest);
            if (!status.isOK()) {
                str::stream ss;
                ss << "Cloner: found corrupt document in " << from_collection.toString() << ": "
                   << redact(status);
                if (skipCorruptDocumentsWhenCloning) {
                    warning() << ss.ss.str() << "; skipping";
                    continue;
                }
                msgasserted(28531, ss);
            }

            verify(collection);
            ++numSeen;
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                txn->checkForInterrupt();

                WriteUnitOfWork wunit(txn);

                BSONObj doc = tmp;
                OpDebug* const nullOpDebug = nullptr;
                Status status = collection->insertDocument(txn, doc, nullOpDebug, true);
                if (!status.isOK() && status.code() != ErrorCodes::DuplicateKey) {
                    error() << "error: exception cloning object in " << from_collection << ' '
                            << redact(status) << " obj:" << redact(doc);
                    uassertStatusOK(status);
                }
                if (status.isOK()) {
                    wunit.commit();
                }
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "cloner insert", to_collection.ns());
            RARELY if (time(0) - saveLast > 60) {
                log() << numSeen << " objects cloned so far from collection " << from_collection;
                saveLast = time(0);
            }

            MONGO_FAIL_POINT_BLOCK(initialSyncHangDuringCollectionClone, options) {
                const BSONObj& data = options.getData();
                if (data["namespace"].String() == to_collection.ns() &&
                    numSeen >= data["numDocsToClone"].numberInt()) {
                    log() << "initial sync - initialSyncHangDuringCollectionClone fail point "
                             "enabled. Blocking until fail point is disabled.";
                    while (MONGO_FAIL_POINT(initialSyncHangDuringCollectionClone)) {
                        mongo::sleepsecs(1);
                    }
                }
            }
        }
    }

    time_t lastLog;
    OperationContext* txn;
    const string _dbName;

    int64_t numSeen;
    NamespaceString from_collection;
    BSONObj from_options;
    BSONObj from_id_index;
    NamespaceString to_collection;
    time_t saveLast;
    CloneOptions _opts;
};

/* copy the specified collection
*/
void Cloner::copy(OperationContext* txn,
                  const string& toDBName,
                  const NamespaceString& from_collection,
                  const BSONObj& from_opts,
                  const BSONObj& from_id_index,
                  const NamespaceString& to_collection,
                  const CloneOptions& opts,
                  Query query) {
    LOG(2) << "\t\tcloning collection " << from_collection << " to " << to_collection << " on "
           << _conn->getServerAddress() << " with filter " << redact(query.toString());

    Fun f(txn, toDBName);
    f.numSeen = 0;
    f.from_collection = from_collection;
    f.from_options = from_opts;
    f.from_id_index = from_id_index;
    f.to_collection = to_collection;
    f.saveLast = time(0);
    f._opts = opts;

    int options = QueryOption_NoCursorTimeout | (opts.slaveOk ? QueryOption_SlaveOk : 0);
    {
        Lock::TempRelease tempRelease(txn->lockState());
        _conn->query(stdx::function<void(DBClientCursorBatchIterator&)>(f),
                     from_collection.ns(),
                     query,
                     0,
                     options);
    }

    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Not primary while cloning collection " << from_collection.ns()
                          << " to "
                          << to_collection.ns()
                          << " with filter "
                          << query.toString(),
            !txn->writesAreReplicated() ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(to_collection));
}

void Cloner::copyIndexes(OperationContext* txn,
                         const string& toDBName,
                         const NamespaceString& from_collection,
                         const BSONObj& from_opts,
                         const std::list<BSONObj>& from_indexes,
                         const NamespaceString& to_collection) {
    LOG(2) << "\t\t copyIndexes " << from_collection << " to " << to_collection << " on "
           << _conn->getServerAddress();

    vector<BSONObj> indexesToBuild;
    for (auto&& indexSpec : from_indexes) {
        indexesToBuild.push_back(fixIndexSpec(to_collection.db().toString(), indexSpec));
    }

    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Not primary while copying indexes from " << from_collection.ns()
                          << " to "
                          << to_collection.ns()
                          << " (Cloner)",
            !txn->writesAreReplicated() ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(to_collection));


    if (indexesToBuild.empty())
        return;

    // We are under lock here again, so reload the database in case it may have disappeared
    // during the temp release
    Database* db = dbHolder().openDb(txn, toDBName);

    Collection* collection = db->getCollection(to_collection);
    if (!collection) {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            txn->checkForInterrupt();

            WriteUnitOfWork wunit(txn);
            Status s = userCreateNS(
                txn,
                db,
                to_collection.toString(),
                from_opts,
                true,
                fixIndexSpec(to_collection.db().toString(), getIdIndexSpec(from_indexes)));
            invariant(s.isOK());
            collection = db->getCollection(to_collection);
            invariant(collection);
            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", to_collection.ns());
    }

    // TODO pass the MultiIndexBlock when inserting into the collection rather than building the
    // indexes after the fact. This depends on holding a lock on the collection the whole time
    // from creation to completion without yielding to ensure the index and the collection
    // matches. It also wouldn't work on non-empty collections so we would need both
    // implementations anyway as long as that is supported.
    MultiIndexBlock indexer(txn, collection);
    indexer.allowInterruption();

    indexer.removeExistingIndexes(&indexesToBuild);
    if (indexesToBuild.empty())
        return;

    auto indexInfoObjs = uassertStatusOK(indexer.init(indexesToBuild));
    uassertStatusOK(indexer.insertAllDocumentsInCollection());

    WriteUnitOfWork wunit(txn);
    indexer.commit();
    if (txn->writesAreReplicated()) {
        const string targetSystemIndexesCollectionName = to_collection.getSystemIndexesCollection();
        const char* createIndexNs = targetSystemIndexesCollectionName.c_str();
        for (auto&& infoObj : indexInfoObjs) {
            getGlobalServiceContext()->getOpObserver()->onCreateIndex(
                txn, createIndexNs, infoObj, false);
        }
    }
    wunit.commit();
}

bool Cloner::copyCollection(OperationContext* txn,
                            const string& ns,
                            const BSONObj& query,
                            string& errmsg,
                            bool shouldCopyIndexes) {
    const NamespaceString nss(ns);
    const string dbname = nss.db().toString();

    // config
    BSONObj filter = BSON("name" << nss.coll().toString());
    list<BSONObj> collList = _conn->getCollectionInfos(dbname, filter);
    BSONObj options;
    bool shouldCreateCollection = false;

    if (!collList.empty()) {
        invariant(collList.size() <= 1);
        shouldCreateCollection = true;
        BSONObj col = collList.front();

        // Confirm that 'col' is not a view.
        {
            std::string namespaceType;
            auto status = bsonExtractStringField(col, "type", &namespaceType);

            uassert(ErrorCodes::InternalError,
                    str::stream() << "Collection 'type' expected to be a string: " << col,
                    ErrorCodes::TypeMismatch != status.code());

            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "copyCollection not supported for views. ns: "
                                  << col["name"].valuestrsafe(),
                    !(status.isOK() && namespaceType == "view"));
        }

        if (col["options"].isABSONObj()) {
            options = col["options"].Obj();
        }
    }

    auto sourceIndexes = _conn->getIndexSpecs(nss.ns(), QueryOption_SlaveOk);
    auto idIndexSpec = getIdIndexSpec(sourceIndexes);

    ScopedTransaction transaction(txn, MODE_IX);
    Lock::DBLock dbWrite(txn->lockState(), dbname, MODE_X);

    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Not primary while copying collection " << ns << " (Cloner)",
            !txn->writesAreReplicated() ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nss));

    Database* db = dbHolder().openDb(txn, dbname);

    if (shouldCreateCollection) {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            txn->checkForInterrupt();

            WriteUnitOfWork wunit(txn);
            Status status = userCreateNS(txn, db, ns, options, true, idIndexSpec);
            if (!status.isOK()) {
                errmsg = status.toString();
                // abort write unit of work
                return false;
            }
            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", ns);
    } else {
        LOG(1) << "No collection info found for ns:" << nss.toString()
               << ", host:" << _conn->getServerAddress();
    }

    // main data
    CloneOptions opts;
    opts.slaveOk = true;
    copy(txn, dbname, nss, options, idIndexSpec, nss, opts, Query(query).snapshot());

    /* TODO : copyIndexes bool does not seem to be implemented! */
    if (!shouldCopyIndexes) {
        log() << "ERROR copy collection shouldCopyIndexes not implemented? " << ns;
    }

    // indexes
    copyIndexes(txn, dbname, NamespaceString(ns), options, sourceIndexes, NamespaceString(ns));

    return true;
}

StatusWith<std::vector<BSONObj>> Cloner::filterCollectionsForClone(
    const CloneOptions& opts, const std::list<BSONObj>& initialCollections) {
    std::vector<BSONObj> finalCollections;
    for (auto&& collection : initialCollections) {
        LOG(2) << "\t cloner got " << collection;

        BSONElement collectionOptions = collection["options"];
        if (collectionOptions.isABSONObj()) {
            auto parseOptionsStatus = CollectionOptions().parse(collectionOptions.Obj());
            if (!parseOptionsStatus.isOK()) {
                return parseOptionsStatus;
            }
        }

        std::string collectionName;
        auto status = bsonExtractStringField(collection, "name", &collectionName);
        if (!status.isOK()) {
            return status;
        }

        const NamespaceString ns(opts.fromDB, collectionName.c_str());

        if (ns.isSystem()) {
            if (legalClientSystemNS(ns.ns()) == 0) {
                LOG(2) << "\t\t not cloning because system collection";
                continue;
            }
        }
        if (!ns.isNormal()) {
            LOG(2) << "\t\t not cloning because has $ ";
            continue;
        }
        if (opts.collsToIgnore.find(ns.ns()) != opts.collsToIgnore.end()) {
            LOG(2) << "\t\t ignoring collection " << ns;
            continue;
        } else {
            LOG(2) << "\t\t not ignoring collection " << ns;
        }

        finalCollections.push_back(collection.getOwned());
    }
    return finalCollections;
}

Status Cloner::createCollectionsForDb(
    OperationContext* txn,
    const std::vector<CreateCollectionParams>& createCollectionParams,
    const std::string& dbName) {
    Database* db = dbHolder().openDb(txn, dbName);
    for (auto&& params : createCollectionParams) {
        auto options = params.collectionInfo["options"].Obj();
        const NamespaceString nss(dbName, params.collectionName);

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            txn->checkForInterrupt();
            WriteUnitOfWork wunit(txn);

            Status createStatus =
                userCreateNS(txn,
                             db,
                             nss.ns(),
                             options,
                             true,
                             fixIndexSpec(nss.db().toString(), params.idIndexSpec));
            if (!createStatus.isOK()) {
                return createStatus;
            }

            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", nss.ns());
    }
    return Status::OK();
}

Status Cloner::copyDb(OperationContext* txn,
                      const std::string& toDBName,
                      const string& masterHost,
                      const CloneOptions& opts,
                      set<string>* clonedColls,
                      std::vector<BSONObj> collectionsToClone) {
    massert(10289,
            "useReplAuth is not written to replication log",
            !opts.useReplAuth || !txn->writesAreReplicated());

    auto statusWithMasterHost = ConnectionString::parse(masterHost);
    if (!statusWithMasterHost.isOK()) {
        return statusWithMasterHost.getStatus();
    }

    const ConnectionString cs(statusWithMasterHost.getValue());

    bool masterSameProcess = false;
    std::vector<HostAndPort> csServers = cs.getServers();
    for (std::vector<HostAndPort>::const_iterator iter = csServers.begin(); iter != csServers.end();
         ++iter) {
        if (!repl::isSelf(*iter, txn->getServiceContext()))
            continue;

        masterSameProcess = true;
        break;
    }

    if (masterSameProcess) {
        if (opts.fromDB == toDBName) {
            // Guard against re-entrance
            return Status(ErrorCodes::IllegalOperation, "can't clone from self (localhost)");
        }
    }

    {
        // setup connection
        if (_conn.get()) {
            // nothing to do
        } else if (!masterSameProcess) {
            std::string errmsg;
            unique_ptr<DBClientBase> con(cs.connect(StringData(), errmsg));
            if (!con.get()) {
                return Status(ErrorCodes::HostUnreachable, errmsg);
            }

            if (isInternalAuthSet() && !con->authenticateInternalUser()) {
                return Status(ErrorCodes::AuthenticationFailed,
                              "Unable to authenticate as internal user");
            }

            _conn = std::move(con);
        } else {
            _conn.reset(new DBDirectClient(txn));
        }
    }

    // Gather the list of collections to clone
    std::vector<BSONObj> toClone;
    if (clonedColls) {
        clonedColls->clear();
    }

    if (opts.createCollections) {
        // getCollectionInfos may make a remote call, which may block indefinitely, so release
        // the global lock that we are entering with.
        Lock::TempRelease tempRelease(txn->lockState());
        std::list<BSONObj> initialCollections = _conn->getCollectionInfos(
            opts.fromDB, ListCollectionsFilter::makeTypeCollectionFilter());
        auto status = filterCollectionsForClone(opts, initialCollections);
        if (!status.isOK()) {
            return status.getStatus();
        }
        toClone = status.getValue();
    } else {
        toClone = collectionsToClone;
    }

    std::vector<CreateCollectionParams> createCollectionParams;
    for (auto&& collection : toClone) {
        CreateCollectionParams params;
        params.collectionName = collection["name"].String();
        params.collectionInfo = collection;
        if (auto idIndex = collection["idIndex"]) {
            params.idIndexSpec = idIndex.Obj();
        }
        createCollectionParams.push_back(params);
    }

    // Get index specs for each collection.
    std::map<StringData, std::list<BSONObj>> collectionIndexSpecs;
    {
        Lock::TempRelease tempRelease(txn->lockState());
        for (auto&& params : createCollectionParams) {
            const NamespaceString nss(opts.fromDB, params.collectionName);
            auto indexSpecs =
                _conn->getIndexSpecs(nss.ns(), opts.slaveOk ? QueryOption_SlaveOk : 0);

            collectionIndexSpecs[params.collectionName] = indexSpecs;

            if (params.idIndexSpec.isEmpty()) {
                params.idIndexSpec = getIdIndexSpec(indexSpecs);
            }
        }
    }

    uassert(ErrorCodes::NotMaster,
            str::stream() << "Not primary while cloning database " << opts.fromDB
                          << " (after getting list of collections to clone)",
            !txn->writesAreReplicated() ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(toDBName));

    if (opts.syncData) {
        if (opts.createCollections) {
            Status status = createCollectionsForDb(txn, createCollectionParams, toDBName);
            if (!status.isOK()) {
                return status;
            }
        }
        for (auto&& params : createCollectionParams) {
            LOG(2) << "  really will clone: " << params.collectionInfo;

            const NamespaceString from_name(opts.fromDB, params.collectionName);
            const NamespaceString to_name(toDBName, params.collectionName);

            if (clonedColls) {
                clonedColls->insert(from_name.ns());
            }

            LOG(1) << "\t\t cloning " << from_name << " -> " << to_name;
            Query q;
            if (opts.snapshot)
                q.snapshot();

            copy(txn,
                 toDBName,
                 from_name,
                 params.collectionInfo["options"].Obj(),
                 params.idIndexSpec,
                 to_name,
                 opts,
                 q);
        }
    }

    // now build the secondary indexes
    if (opts.syncIndexes) {
        for (auto&& params : createCollectionParams) {
            log() << "copying indexes for: " << params.collectionInfo;

            const NamespaceString from_name(opts.fromDB, params.collectionName);
            const NamespaceString to_name(toDBName, params.collectionName);


            copyIndexes(txn,
                        toDBName,
                        from_name,
                        params.collectionInfo["options"].Obj(),
                        collectionIndexSpecs[params.collectionName],
                        to_name);
        }
    }

    return Status::OK();
}

}  // namespace mongo
