// util.cpp

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

#include "stdafx.h"
#include "goodies.h"
#include "unittest.h"
#include "top.h"
#include "file_allocator.h"
#include "optime.h"

namespace mongo {

    vector<UnitTest*> *UnitTest::tests = 0;
    bool UnitTest::running = false;

    Nullstream nullstream;

    thread_specific_ptr<Logstream> Logstream::tsp;

    const char *default_getcurns() { return ""; }
    const char * (*getcurns)() = default_getcurns;

    int logLevel = 0;
    boost::mutex &Logstream::mutex = *( new boost::mutex );
    int Logstream::doneSetup = Logstream::magicNumber();
    
    bool goingAway = false;

    bool isPrime(int n) {
        int z = 2;
        while ( 1 ) {
            if ( z*z > n )
                break;
            if ( n % z == 0 )
                return false;
            z++;
        }
        return true;
    }

    int nextPrime(int n) {
        n |= 1; // 2 goes to 3...don't care...
        while ( !isPrime(n) )
            n += 2;
        return n;
    }

    struct UtilTest : public UnitTest {
        void run() {
            assert( WrappingInt(0) <= WrappingInt(0) );
            assert( WrappingInt(0) <= WrappingInt(1) );
            assert( !(WrappingInt(1) <= WrappingInt(0)) );
            assert( (WrappingInt(0xf0000000) <= WrappingInt(0)) );
            assert( (WrappingInt(0xf0000000) <= WrappingInt(9000)) );
            assert( !(WrappingInt(300) <= WrappingInt(0xe0000000)) );

            assert( tdiff(3, 4) == 1 );
            assert( tdiff(4, 3) == -1 );
            assert( tdiff(0xffffffff, 0) == 1 );

            assert( isPrime(3) );
            assert( isPrime(2) );
            assert( isPrime(13) );
            assert( isPrime(17) );
            assert( !isPrime(9) );
            assert( !isPrime(6) );
            assert( nextPrime(4) == 5 );
            assert( nextPrime(8) == 11 );

            assert( endsWith("abcde", "de") );
            assert( !endsWith("abcde", "dasdfasdfashkfde") );

            assert( swapEndian(0x01020304) == 0x04030201 );

        }
    } utilTest;
    
    Top::T Top::snapshotStart_ = Top::currentTime();
    Top::D Top::snapshotDuration_;
    Top::UsageMap Top::totalUsage_;
    Top::UsageMap Top::snapshotA_;
    Top::UsageMap Top::snapshotB_;
    Top::UsageMap &Top::snapshot_ = Top::snapshotA_;
    Top::UsageMap &Top::nextSnapshot_ = Top::snapshotB_;
    boost::mutex Top::topMutex;
    
    // The mutex contained in this object may be held on shutdown.
    FileAllocator &theFileAllocator_ = *(new FileAllocator());
    FileAllocator &theFileAllocator() { return theFileAllocator_; }
    
    OpTime OpTime::last(0, 0);
    
    /* this is a good place to set a breakpoint when debugging, as lots of warning things
       (assert, wassert) call it.
    */
    void sayDbContext(const char *errmsg) {
        if ( errmsg ) {
            problem() << errmsg << endl;
        }
        printStackTrace();
    }
    
    void rawOut( const string &s ) {
        if( s.empty() ) return;
        char now[64];
        time_t_to_String(time(0), now);
        now[20] = 0;        
#if defined(_WIN32)
        (std::cout << now << " " << s).flush();
#else
        write( STDOUT_FILENO, now, 20 );
		write( STDOUT_FILENO, " ", 1 );
        write( STDOUT_FILENO, s.c_str(), s.length() );
        fsync( STDOUT_FILENO );        
#endif
    }

#ifndef _SCONS
    // only works in scons
    const char * gitVersion(){ return ""; }
    const char * sysInfo(){ return ""; }
#endif

    void printGitVersion() { log() << "git version: " << gitVersion() << endl; }
    void printSysInfo() { log() << "sys info: " << sysInfo() << endl; }
    string mongodVersion() {
        stringstream ss;
        ss << "db version v" << versionString << ", pdfile version " << VERSION << "." << VERSION_MINOR;
        return ss.str();
    }
        
} // namespace mongo
