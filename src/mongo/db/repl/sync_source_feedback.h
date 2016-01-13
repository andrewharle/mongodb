/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/repl/oplogreader.h"
#include "mongo/util/background.h"


namespace mongo {

    class Member;

    class SyncSourceFeedback : public BackgroundJob {
    public:
        SyncSourceFeedback() : BackgroundJob(false /*don't selfdelete*/),
                              _syncTarget(NULL),
                              _oplogReader(new OplogReader()),
                              _supportsUpdater(true),
                              _positionChanged(false),
                              _handshakeNeeded(false) {}

        ~SyncSourceFeedback() {
            delete _oplogReader;
        }

        /// Adds an entry to _members for a secondary that has connected to us.
        void associateMember(const BSONObj& id, Member* member);

        /// Ensures local.me is populated and populates it if not.
        void ensureMe();

        /// Passes handshake up the replication chain, upon receiving a handshake.
        void forwardSlaveHandshake();

        void updateSelfInMap(const OpTime& ot) {
            updateMap(_me["_id"].OID(), ot, true);
        }

        /// Connect to sync target and create OplogReader if needed.
        bool connect(const Member* target);

        void resetConnection() {
            LOG(1) << "resetting connection in sync source feedback";
            _connection.reset();
        }

        void resetOplogReaderConnection() {
            _oplogReader->resetConnection();
        }

        /// Used extensively in bgsync, to see if we need to use the OplogReader syncing method.
        bool supportsUpdater() const {
            // oplogReader will be NULL if new updater is supported
            //boost::unique_lock<boost::mutex> lock(_mtx);
            return _supportsUpdater;
        }

        /// Transfers information about a chained node's oplog position from downstream to upstream
        void percolate(const mongo::OID& rid, const OpTime& ot);

        /// Updates the _slaveMap to be forwarded to the sync target.
        void updateMap(const mongo::OID& rid, const OpTime& ot, bool self=false);

        std::string name() const { return "SyncSourceFeedbackThread"; }

        /// Loops forever, passing updates when they are present.
        void run();

        /* The below methods just fall through to OplogReader and are only used when our sync target
         * does not support the update command.
         */
        bool connectOplogReader(const std::string& hostName) {
            return _oplogReader->connect(hostName, _me);
        }

        bool connect(const mongo::OID& rid, const int from, const string& to) {
            return _oplogReader->connect(rid, from, to);
        }

        void ghostQueryGTE(const char *ns, OpTime t) {
            _oplogReader->ghostQueryGTE(ns, t);
        }

        bool haveCursor() {
            return _oplogReader->haveCursor();
        }

        bool more() {
            return _oplogReader->more();
        }

        bool moreInCurrentBatch() {
            return _oplogReader->moreInCurrentBatch();
        }

        BSONObj nextSafe() {
            return _oplogReader->nextSafe();
        }

        void tailCheck() {
            _oplogReader->tailCheck();
        }

        void tailingQueryGTE(const char *ns, OpTime t, const BSONObj* fields=0) {
            _oplogReader->tailingQueryGTE(ns, t, fields);
        }

        /** 
        * this mutex protects the _conn field of _oplogReader in that we cannot mix the functions
        * which check _conn for null (commonConnect() and connect() do this) with the function that
        * sets the pointer to null (resetConnection()). All other uses of the _oplogReader's _conn
        * do not need the mutex locked, due to the threading logic that prevents _connect()
        * from being called concurrently.
        */
        boost::mutex oplock;

    private:
        /**
         * Authenticates _connection using the server's cluster-membership credentials.
         *
         * Returns true on successful authentication.
         */
        bool replAuthenticate();

        /* Sends initialization information to our sync target, also determines whether or not they
         * support the updater command.
         */
        bool replHandshake();

        /* Inform the sync target of our current position in the oplog, as well as the positions
         * of all secondaries chained through us.
         */
        bool updateUpstream();

        bool hasConnection() {
            return _connection.get();
        }

        /// Connect to sync target and create OplogReader if needed.
        bool _connect(const std::string& hostName);

        // stores our OID to be passed along in commands
        BSONObj _me;
        // the member we are currently syncing from
        const Member* _syncTarget;
        // holds the oplogReader for use when we fall back to old style updates
        OplogReader* _oplogReader;
        // our connection to our sync target
        boost::scoped_ptr<DBClientConnection> _connection;
        // tracks whether we are in fallback mode or not
        bool _supportsUpdater;
        // protects connection
        boost::mutex _connmtx;
        // protects cond and maps and the indicator bools
        boost::mutex _mtx;
        // contains the most recent optime of each member syncing to us
        map<mongo::OID, OpTime> _slaveMap;
        typedef map<mongo::OID, Member*> OIDMemberMap;
        // contains a pointer to each member, which we can look up by oid
        OIDMemberMap _members;
        // used to alert our thread of changes which need to be passed up the chain
        boost::condition _cond;
        // used to indicate a position change which has not yet been pushed along
        bool _positionChanged;
        // used to indicate a connection change which has not yet been shook on
        bool _handshakeNeeded;
    };
}
