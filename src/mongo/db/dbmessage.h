// dbmessage.h

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

#pragma once

#include "mongo/bson/bson_validate.h"
#include "mongo/client/constants.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/message_port.h"

namespace mongo {

    /* db response format

       Query or GetMore: // see struct QueryResult
          int resultFlags;
          int64 cursorID;
          int startingFrom;
          int nReturned;
          list of marshalled JSObjects;
    */

/* db request message format

   unsigned opid;         // arbitary; will be echoed back
   byte operation;
   int options;

   then for:

   dbInsert:
      string collection;
      a series of JSObjects
   dbDelete:
      string collection;
      int flags=0; // 1=DeleteSingle
      JSObject query;
   dbUpdate:
      string collection;
      int flags; // 1=upsert
      JSObject query;
      JSObject objectToUpdate;
        objectToUpdate may include { $inc: <field> } or { $set: ... }, see struct Mod.
   dbQuery:
      string collection;
      int nToSkip;
      int nToReturn; // how many you want back as the beginning of the cursor data (0=no limit)
                     // greater than zero is simply a hint on how many objects to send back per "cursor batch".
                     // a negative number indicates a hard limit.
      JSObject query;
      [JSObject fieldsToReturn]
   dbGetMore:
      string collection; // redundant, might use for security.
      int nToReturn;
      int64 cursorID;
   dbKillCursors=2007:
      int n;
      int64 cursorIDs[n];

   Note that on Update, there is only one object, which is different
   from insert where you can pass a list of objects to insert in the db.
   Note that the update field layout is very similar layout to Query.
*/


#pragma pack(1)
    struct QueryResult : public MsgData {
        long long cursorId;
        int startingFrom;
        int nReturned;
        const char *data() {
            return (char *) (((int *)&nReturned)+1);
        }
        int resultFlags() {
            return dataAsInt();
        }
        int& _resultFlags() {
            return dataAsInt();
        }
        void setResultFlagsToOk() {
            _resultFlags() = ResultFlag_AwaitCapable;
        }
        void initializeResultFlags() {
            _resultFlags() = 0;   
        }
    };

#pragma pack()

    /* For the database/server protocol, these objects and functions encapsulate
       the various messages transmitted over the connection.

       See http://dochub.mongodb.org/core/mongowireprotocol
    */
    class DbMessage {
    // Assume sizeof(int) == 4 bytes
    BOOST_STATIC_ASSERT(sizeof(int) == 4);

    public:
        // Note: DbMessage constructor reads the first 4 bytes and stores it in reserved
        DbMessage(const Message& msg);

        // Indicates whether this message is expected to have a ns
        // or in the case of dbMsg, a string in the same place as ns
        bool messageShouldHaveNs() const {
            return (_msg.operation() >= dbMsg) & (_msg.operation() <= dbDelete);
        }

        /** the 32 bit field before the ns
         * track all bit usage here as its cross op
         * 0: InsertOption_ContinueOnError
         * 1: fromWriteback
         */
        int reservedField() const { return _reserved; }

        const char * getns() const;
        int getQueryNToReturn() const;

        int pullInt();
        long long pullInt64();
        const long long* getArray(size_t count) const;

        /* for insert and update msgs */
        bool moreJSObjs() const {
            return _nextjsobj != 0;
        }

        BSONObj nextJsObj();

        const Message& msg() const { return _msg; }

        const char * markGet() const {
            return _nextjsobj;
        }

        void markSet() {
            _mark = _nextjsobj;
        }

        void markReset(const char * toMark);

    private:
        // Check if we have enough data to read
        template<typename T>
        void checkRead(const char* start, size_t count = 0) const;

        // Read some type without advancing our position
        template<typename T>
        T read() const;

        // Read some type, and advance our position
        template<typename T> T readAndAdvance();

        const Message& _msg;
        int _reserved; // flags or zero depending on packet, starts the packet

        const char* _nsStart; // start of namespace string, +4 from message start
        const char* _nextjsobj; // current position reading packet
        const char* _theEnd; // end of packet

        const char* _mark;

        unsigned int _nsLen;
    };


    /* a request to run a query, received from the database */
    class QueryMessage {
    public:
        const char *ns;
        int ntoskip;
        int ntoreturn;
        int queryOptions;
        BSONObj query;
        BSONObj fields;

        /**
         * parses the message into the above fields
         * Warning: constructor mutates DbMessage.
         */
        QueryMessage(DbMessage& d) {
            ns = d.getns();
            ntoskip = d.pullInt();
            ntoreturn = d.pullInt();
            query = d.nextJsObj();
            if ( d.moreJSObjs() ) {
                fields = d.nextJsObj();
            }
            queryOptions = d.msg().header()->dataAsInt();
        }
    };

    /**
     * A response to a DbMessage.
     */
    struct DbResponse {
        Message *response;
        MSGID responseTo;
        string exhaustNS; /* points to ns if exhaust mode. 0=normal mode*/
        DbResponse(Message *r, MSGID rt) : response(r), responseTo(rt){ }
        DbResponse() {
            response = 0;
        }
        ~DbResponse() { delete response; }
    };

    void replyToQuery(int queryResultFlags,
                      AbstractMessagingPort* p, Message& requestMsg,
                      void *data, int size,
                      int nReturned, int startingFrom = 0,
                      long long cursorId = 0
                      );


    /* object reply helper. */
    void replyToQuery(int queryResultFlags,
                      AbstractMessagingPort* p, Message& requestMsg,
                      const BSONObj& responseObj);

    /* helper to do a reply using a DbResponse object */
    void replyToQuery( int queryResultFlags, Message& m, DbResponse& dbresponse, BSONObj obj );

    /**
     * Helper method for setting up a response object.
     *
     * @param queryResultFlags The flags to set to the response object.
     * @param response The object to be used for building the response. The internal buffer of
     *     this object will contain the raw data from resultObj after a successful call.
     * @param resultObj The bson object that contains the reply data.
     */
    void replyToQuery( int queryResultFlags, Message& response, const BSONObj& resultObj );
} // namespace mongo
