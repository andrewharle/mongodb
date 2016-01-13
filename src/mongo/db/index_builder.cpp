/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/index_builder.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/db.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AtomicUInt IndexBuilder::_indexBuildCount = 0;

namespace {
    // Synchronization tools when replication spawns a background index in a new thread.
    // The bool is 'true' when a new background index has started in a new thread but the
    // parent thread has not yet synchronized with it.
    bool _bgIndexStarting(false);
    boost::mutex _bgIndexStartingMutex;
    boost::condition_variable _bgIndexStartingCondVar;

    void _setBgIndexStarting() {
        boost::mutex::scoped_lock lk(_bgIndexStartingMutex);
        invariant(_bgIndexStarting == false);
        _bgIndexStarting = true;
        _bgIndexStartingCondVar.notify_one();
    }
} // namespace

    IndexBuilder::IndexBuilder(const BSONObj& index) :
        BackgroundJob(true /* self-delete */), _index(index.getOwned()),
        _name(str::stream() << "repl index builder " << (_indexBuildCount++).get()) {
    }

    IndexBuilder::~IndexBuilder() {}

    std::string IndexBuilder::name() const {
        return _name;
    }

    void IndexBuilder::run() {
        LOG(2) << "IndexBuilder building index " << _index;

        Client::initThread(name().c_str());
        Lock::ParallelBatchWriterMode::iAmABatchParticipant();

        replLocalAuth();

        cc().curop()->reset(HostAndPort(), dbInsert);
        NamespaceString ns(_index["ns"].String());
        Client::WriteContext ctx(ns.getSystemIndexesCollection());

        // Show which index we're background building in the curop display, and unblock waiters.
        cc().curop()->setQuery(_index);
        _setBgIndexStarting();

        Status status = build( ctx.ctx() );
        if ( !status.isOK() ) {
            log() << "IndexBuilder could not build index: " << status.toString();
        }

        cc().shutdown();
    }

    Status IndexBuilder::build( Client::Context& context ) const {
        string ns = _index["ns"].String();
        Database* db = context.db();
        Collection* c = db->getCollection( ns );
        if ( !c ) {
            c = db->getOrCreateCollection( ns );
            verify(c);
        }

        // Show which index we're building in the curop display.
        context.getClient()->curop()->setQuery(_index);

        Status status = c->getIndexCatalog()->createIndex( _index, 
                                                           true, 
                                                           IndexCatalog::SHUTDOWN_LEAVE_DIRTY );
        if ( status.code() == ErrorCodes::IndexAlreadyExists )
            return Status::OK();
        return status;
    }

    void IndexBuilder::waitForBgIndexStarting() {
        boost::unique_lock<boost::mutex> lk(_bgIndexStartingMutex);
        while (_bgIndexStarting == false) {
            _bgIndexStartingCondVar.wait(lk);
        }
        // Reset for next time.
        _bgIndexStarting = false;
    }

    std::vector<BSONObj> 
    IndexBuilder::killMatchingIndexBuilds(Collection* collection,
                                          const IndexCatalog::IndexKillCriteria& criteria) {
        invariant(collection);
        return collection->getIndexCatalog()->killMatchingIndexBuilds(criteria);
    }

    void IndexBuilder::restoreIndexes(const std::vector<BSONObj>& indexes) {
        log() << "restarting " << indexes.size() << " index build(s)" << endl;
        for (int i = 0; i < static_cast<int>(indexes.size()); i++) {
            IndexBuilder* indexBuilder = new IndexBuilder(indexes[i]);
            // This looks like a memory leak, but indexBuilder deletes itself when it finishes
            indexBuilder->go();
            dbtemprelease release;
            IndexBuilder::waitForBgIndexStarting();
        }
    }
}

