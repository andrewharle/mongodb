// assert_util.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "assert_util.h"
#include "assert.h"
//#include "file.h"
#include <cmath>
using namespace std;

#ifndef _WIN32
#include <cxxabi.h>
#include <sys/file.h>
#endif

//#include "../bson/bson.h"
#include "../db/jsobj.h"

namespace mongo {

    AssertionCount assertionCount;

    AssertionCount::AssertionCount()
        : regular(0),warning(0),msg(0),user(0),rollovers(0) {
    }

    void AssertionCount::rollover() {
        rollovers++;
        regular = 0;
        warning = 0;
        msg = 0;
        user = 0;
    }

    void AssertionCount::condrollover( int newvalue ) {
        static int max = (int)pow( 2.0 , 30 );
        if ( newvalue >= max )
            rollover();
    }

    void ExceptionInfo::append( BSONObjBuilder& b , const char * m , const char * c ) const {
        if ( msg.empty() )
            b.append( m , "unknown assertion" );
        else
            b.append( m , msg );

        if ( code )
            b.append( c , code );
    }

    string getDbContext();

    /* "warning" assert -- safe to continue, so we don't throw exception. */
    NOINLINE_DECL void wasserted(const char *msg, const char *file, unsigned line) {
        static bool rateLimited;
        static time_t lastWhen;
        static unsigned lastLine;
        if( lastLine == line && time(0)-lastWhen < 5 ) { 
            if( rateLimited++ == 0 ) { 
                log() << "rate limiting wassert" << endl;
            }
            return;
        }
        lastWhen = time(0);
        lastLine = line;

        problem() << "warning assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        sayDbContext();
        raiseError(0,msg && *msg ? msg : "wassertion failure");
        assertionCount.condrollover( ++assertionCount.warning );
#if defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF)
        // this is so we notice in buildbot
        log() << "\n\n***aborting after wassert() failure in a debug/test build\n\n" << endl;
        abort();
#endif
    }

    NOINLINE_DECL void asserted(const char *msg, const char *file, unsigned line) {
        assertionCount.condrollover( ++assertionCount.regular );
        problem() << "Assertion failure " << msg << ' ' << file << ' ' << dec << line << endl;
        sayDbContext();
        raiseError(0,msg && *msg ? msg : "assertion failure");
        stringstream temp;
        temp << "assertion " << file << ":" << line;
        AssertionException e(temp.str(),0);
        breakpoint();
#if defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF)
        // this is so we notice in buildbot
        log() << "\n\n***aborting after assert() failure in a debug/test build\n\n" << endl;
        abort();
#endif
        throw e;
    }

    NOINLINE_DECL void verifyFailed( int msgid ) {
        assertionCount.condrollover( ++assertionCount.regular );
        problem() << "Assertion failure " << msgid << endl;
        sayDbContext();
        raiseError(0,"assertion failure");
        stringstream temp;
        temp << msgid;
        AssertionException e(temp.str(),0);
        breakpoint();
#if defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF)
        // this is so we notice in buildbot
        log() << "\n\n***aborting after verify() failure in a debug/test build\n\n" << endl;
        abort();
#endif
        throw e;
    }

    void uassert_nothrow(const char *msg) {
        raiseError(0,msg);
    }

    NOINLINE_DECL void uasserted(int msgid, const char *msg) {
        assertionCount.condrollover( ++assertionCount.user );
        LOG(1) << "User Assertion: " << msgid << ":" << msg << endl;
        raiseError(msgid,msg);
        throw UserException(msgid, msg);
    }

    NOINLINE_DECL void msgasserted(int msgid, const char *msg) {
        assertionCount.condrollover( ++assertionCount.warning );
        tlog() << "Assertion: " << msgid << ":" << msg << endl;
        raiseError(msgid,msg && *msg ? msg : "massert failure");
        breakpoint();
        printStackTrace();
        throw MsgAssertionException(msgid, msg);
    }

    NOINLINE_DECL void msgassertedNoTrace(int msgid, const char *msg) {
        assertionCount.condrollover( ++assertionCount.warning );
        log() << "Assertion: " << msgid << ":" << msg << endl;
        raiseError(msgid,msg && *msg ? msg : "massert failure");
        throw MsgAssertionException(msgid, msg);
    }

    NOINLINE_DECL void streamNotGood( int code , string msg , std::ios& myios ) {
        stringstream ss;
        // errno might not work on all systems for streams
        // if it doesn't for a system should deal with here
        ss << msg << " stream invalid: " << errnoWithDescription();
        throw UserException( code , ss.str() );
    }

    string errnoWithPrefix( const char * prefix ) {
        stringstream ss;
        if ( prefix )
            ss << prefix << ": ";
        ss << errnoWithDescription();
        return ss.str();
    }

    string demangleName( const type_info& typeinfo ) {
#ifdef _WIN32
        return typeinfo.name();
#else
        int status;

        char * niceName = abi::__cxa_demangle(typeinfo.name(), 0, 0, &status);
        if ( ! niceName )
            return typeinfo.name();

        string s = niceName;
        free(niceName);
        return s;
#endif
    }

    NOINLINE_DECL ErrorMsg::ErrorMsg(const char *msg, char ch) {
        int l = strlen(msg);
        assert( l < 128);
        memcpy(buf, msg, l);
        char *p = buf + l;
        p[0] = ch;
        p[1] = 0;
    }

    NOINLINE_DECL ErrorMsg::ErrorMsg(const char *msg, unsigned val) {
        int l = strlen(msg);
        assert( l < 128);
        memcpy(buf, msg, l);
        char *p = buf + l;
        sprintf(p, "%u", val);
    }

}

