// @file common.cpp

/*
 *    Copyright (C) 2010 10gen Inc.
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
 */

#include "pch.h"
#include "concurrency.h"
#include "jsobjmanipulator.h"

/**
 * this just has globals
 */
namespace mongo {

    /** called by mongos, mongod, test. do not call from clients and such. 
        invoked before about everything except global var construction.
     */
    void doPreServerStatupInits() { 
    }

    /* we use new here so we don't have to worry about destructor orders at program shutdown */
    MongoMutex &dbMutex( *(new MongoMutex("dbMutex")) );

    MongoMutex::MongoMutex(const char *name) : _m(name) {
        static int n = 0;
        assert( ++n == 1 ); // below releasingWriteLock we assume MongoMutex is a singleton, and uses dbMutex ref above
        _remapPrivateViewRequested = false;
    }

    // OpTime::now() uses dbMutex, thus it is in this file not in the cpp files used by drivers and such
    void BSONElementManipulator::initTimestamp() {
        massert( 10332 ,  "Expected CurrentTime type", _element.type() == Timestamp );
        unsigned long long &timestamp = *( reinterpret_cast< unsigned long long* >( value() ) );
        if ( timestamp == 0 )
            timestamp = OpTime::now().asDate();
    }

    NOINLINE_DECL OpTime OpTime::skewed() {
        bool toLog = false;
        ONCE toLog = true;
        RARELY toLog = true;
        last.i++;
        if ( last.i & 0x80000000 )
            toLog = true;
        if ( toLog ) {
            log() << "clock skew detected  prev: " << last.secs << " now: " << (unsigned) time(0) << endl;
        }
        if ( last.i & 0x80000000 ) {
            log() << "error large clock skew detected, shutting down" << endl;
            throw ClockSkewException();
        }
        return last;
    }

}
