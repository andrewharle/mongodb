/** @file oplogreader.h */

#pragma once

#include "../client/dbclient.h"
#include "../client/constants.h"
#include "dbhelpers.h"

namespace mongo {

    /* started abstracting out the querying of the primary/master's oplog
       still fairly awkward but a start.
    */
    class OplogReader {
        shared_ptr<DBClientConnection> _conn;
        shared_ptr<DBClientCursor> cursor;
    public:

        OplogReader() {
        }
        ~OplogReader() {
        }

        void resetCursor() {
            cursor.reset();
        }
        void resetConnection() {
            cursor.reset();
            _conn.reset();
        }
        DBClientConnection* conn() { return _conn.get(); }
        BSONObj findOne(const char *ns, const Query& q) {
            return conn()->findOne(ns, q, 0, QueryOption_SlaveOk);
        }

        BSONObj getLastOp(const char *ns) {
            return findOne(ns, Query().sort(reverseNaturalObj));
        }

        /* ok to call if already connected */
        bool connect(string hostname);

        bool connect(const BSONObj& rid, const int from, const string& to);


        void tailCheck() {
            if( cursor.get() && cursor->isDead() ) {
                log() << "repl: old cursor isDead, will initiate a new one" << endl;
                resetCursor();
            }
        }

        bool haveCursor() { return cursor.get() != 0; }

        void query(const char *ns, const BSONObj& query) {
            assert( !haveCursor() );
            cursor.reset( _conn->query(ns, query, 0, 0, 0, QueryOption_SlaveOk).release() );
        }

        void queryGTE(const char *ns, OpTime t) {
            BSONObjBuilder q;
            q.appendDate("$gte", t.asDate());
            BSONObjBuilder q2;
            q2.append("ts", q.done());
            query(ns, q2.done());
        }

        void tailingQuery(const char *ns, const BSONObj& query, const BSONObj* fields=0) {
            assert( !haveCursor() );
            log(2) << "repl: " << ns << ".find(" << query.toString() << ')' << endl;
            cursor.reset( _conn->query( ns, query, 0, 0, fields,
                                        QueryOption_CursorTailable | QueryOption_SlaveOk | QueryOption_OplogReplay |
                                        /* TODO: slaveok maybe shouldn't use? */
                                        QueryOption_AwaitData
                                        ).release() );
        }

        void tailingQueryGTE(const char *ns, OpTime t, const BSONObj* fields=0) {
            BSONObjBuilder q;
            q.appendDate("$gte", t.asDate());
            BSONObjBuilder query;
            query.append("ts", q.done());
            tailingQuery(ns, query.done(), fields);
        }

        /* Do a tailing query, but only send the ts field back. */
        void ghostQueryGTE(const char *ns, OpTime t) {
            const BSONObj fields = BSON("ts" << 1 << "_id" << 0);
            return tailingQueryGTE(ns, t, &fields);
        }

        bool more() {
            assert( cursor.get() );
            return cursor->more();
        }
        bool moreInCurrentBatch() {
            assert( cursor.get() );
            return cursor->moreInCurrentBatch();
        }

        /* old mongod's can't do the await flag... */
        bool awaitCapable() {
            return cursor->hasResultFlag(ResultFlag_AwaitCapable);
        }

        void peek(vector<BSONObj>& v, int n) {
            if( cursor.get() )
                cursor->peek(v,n);
        }

        BSONObj nextSafe() { return cursor->nextSafe(); }

        BSONObj next() { return cursor->next(); }

        void putBack(BSONObj op) { cursor->putBack(op); }

    private:
        bool commonConnect(const string& hostName);
        bool passthroughHandshake(const BSONObj& rid, const int f);
    };

}
