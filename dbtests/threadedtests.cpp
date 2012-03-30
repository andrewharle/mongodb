// threadedtests.cpp - Tests for threaded code
//

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
 */

#include "pch.h"
#include "../bson/util/atomic_int.h"
#include "../util/concurrency/mvar.h"
#include "../util/concurrency/thread_pool.h"
#include "../util/concurrency/list.h"
#include "../util/timer.h"
#include <boost/thread.hpp>
#include <boost/bind.hpp>

#include "dbtests.h"

namespace ThreadedTests {

    template <int nthreads_param=10>
    class ThreadedTest {
    public:
        virtual void setup() {} //optional
        virtual void subthread(int remaining) = 0; // each thread whatever test work you want done
        virtual void validate() = 0; // after work is done

        static const int nthreads = nthreads_param;

        void run() {
            setup();
            launch_subthreads(nthreads);
            validate();
        }

        virtual ~ThreadedTest() {}; // not necessary, but makes compilers happy

    private:
        void launch_subthreads(int remaining) {
            if (!remaining) 
                return;

            boost::thread athread(boost::bind(&ThreadedTest::subthread, this, remaining));
            launch_subthreads(remaining - 1);
            athread.join();
        }
    };

    class MongoMutexTest : public ThreadedTest<135> {
#if defined(_DEBUG)
        enum { N = 5000 };
#else
        enum { N = 40000 };
#endif
        MongoMutex *mm;
        ProgressMeter pm;
    public:
        MongoMutexTest() : pm(N * nthreads) {}
        void run() {
            DEV {
                // in _DEBUG builds on linux we mprotect each time a writelock
                // is taken. That can greatly slow down this test if there are
                // many open files
                DBDirectClient db;
                db.simpleCommand("admin", NULL, "closeAllDatabases");
            }

            Timer t;
            cout << "MongoMutexTest N:" << N << endl;
            ThreadedTest<135>::run();
            cout << "MongoMutexTest " << t.millis() << "ms" << endl;
        }
    private:
        virtual void setup() {
            mm = &dbMutex;
        }
        virtual void subthread(int) {
            Client::initThread("mongomutextest");
            sleepmillis(0);
            for( int i = 0; i < N; i++ ) {
                if( i % 7 == 0 ) {
                    mm->lock_shared();
                    mm->lock_shared();
                    mm->unlock_shared();
                    mm->unlock_shared();
                }
                else if( i % 7 == 1 ) {
                    mm->lock_shared();
                    ASSERT( mm->atLeastReadLocked() );
                    mm->unlock_shared();
                }
                else if( i % 7 == 2 ) {
                    mm->lock();
                    ASSERT( mm->isWriteLocked() );
                    mm->unlock();
                }
                else if( i % 7 == 3 ) {
                    mm->lock();
                    mm->lock_shared();
                    ASSERT( mm->isWriteLocked() );
                    mm->unlock_shared();
                    mm->unlock();
                }
                else if( i % 7 == 4 ) {
                    mm->lock();
                    mm->releaseEarly();
                    mm->unlock();
                }
                else if( i % 7 == 5 ) {
                    if( mm->lock_try(1) ) {
                        mm->unlock();
                    }
                }
                else if( i % 7 == 6 ) {
                    if( mm->lock_shared_try(0) ) {
                        mm->unlock_shared();
                    }
                }
                else {
                    mm->lock_shared();
                    mm->unlock_shared();
                }
                pm.hit();
            }
            cc().shutdown();
        }
        virtual void validate() {
            ASSERT( !mm->atLeastReadLocked() );
            mm->lock();
            mm->unlock();
            mm->lock_shared();
            mm->unlock_shared();
        }
    };

    // Tested with up to 30k threads
    class IsAtomicUIntAtomic : public ThreadedTest<> {
        static const int iterations = 1000000;
        AtomicUInt target;

        void subthread(int) {
            for(int i=0; i < iterations; i++) {
                //target.x++; // verified to fail with this version
                target++;
            }
        }
        void validate() {
            ASSERT_EQUALS(target.x , unsigned(nthreads * iterations));

            AtomicUInt u;
            ASSERT_EQUALS(0u, u);
            ASSERT_EQUALS(0u, u++);
            ASSERT_EQUALS(2u, ++u);
            ASSERT_EQUALS(2u, u--);
            ASSERT_EQUALS(0u, --u);
            ASSERT_EQUALS(0u, u);
            
            u++;
            ASSERT( u > 0 );

            u--;
            ASSERT( ! ( u > 0 ) );
        }
    };

    class MVarTest : public ThreadedTest<> {
        static const int iterations = 10000;
        MVar<int> target;

    public:
        MVarTest() : target(0) {}
        void subthread(int) {
            for(int i=0; i < iterations; i++) {
                int val = target.take();
#if BOOST_VERSION >= 103500
                //increase chances of catching failure
                boost::this_thread::yield();
#endif
                target.put(val+1);
            }
        }
        void validate() {
            ASSERT_EQUALS(target.take() , nthreads * iterations);
        }
    };

    class ThreadPoolTest {
        static const int iterations = 10000;
        static const int nThreads = 8;

        AtomicUInt counter;
        void increment(int n) {
            for (int i=0; i<n; i++) {
                counter++;
            }
        }

    public:
        void run() {
            ThreadPool tp(nThreads);

            for (int i=0; i < iterations; i++) {
                tp.schedule(&ThreadPoolTest::increment, this, 2);
            }

            tp.join();

            ASSERT(counter == (unsigned)(iterations * 2));
        }
    };

    class LockTest {
    public:
        void run() {
            // quick atomicint wrap test
            // MSGID likely assumes this semantic
            AtomicUInt counter = 0xffffffff;
            counter++;
            ASSERT( counter == 0 );

            writelocktry lk( "" , 0 );
            ASSERT( lk.got() );
        }
    };

    class RWLockTest1 { 
    public:
        void run() { 
            RWLock lk( "eliot" );
            {
                rwlock r( lk , true , 1000 );
            }
        }
    };

    class RWLockTest2 { 
    public:
        
        static void worker1( const RWLock * lk , AtomicUInt * x ) {
            (*x)++; // 1
            //cout << "lock b try" << endl;
            rwlock b( *lk , true ); 
            //cout << "lock b got" << endl;
            (*x)++; // 2
        }

        static void worker2( const RWLock * lk , AtomicUInt * x ) {
            //cout << "lock c try" << endl;
            rwlock c( *lk , false );
            (*x)++;
            //cout << "lock c got" << endl;
        }

        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */
            
            RWLock lk( "eliot2" , 120 * 1000 );
            cout << "RWLock impl: " << lk.implType() << endl;

            auto_ptr<rwlock> a( new rwlock( lk , false ) );
            
            AtomicUInt x1 = 0;
            cout << "A : " << &x1 << endl;
            boost::thread t1( boost::bind( worker1 , &lk , &x1 ) );
            while ( ! x1 );
            assert( x1 == 1 );
            sleepmillis( 500 );
            assert( x1 == 1 );
            
            AtomicUInt x2 = 0;

            boost::thread t2( boost::bind( worker2, &lk , &x2 ) );
            t2.join();
            assert( x2 == 1 );

            a.reset();

            for ( int i=0; i<2000; i++ ) {
                if ( x1 == 2 )
                    break;
                sleepmillis(1);
            }

            assert( x1 == 2 );
            t1.join();
            
        }
    };



    /** test of shared lock */
    class RWLockTest3 { 
    public:
        
        static void worker2( RWLock * lk , AtomicUInt * x ) {
    	    assert( ! lk->lock_try(0) );
            //cout << "lock c try" << endl;
            rwlock c( *lk , false );
            (*x)++;
            //cout << "lock c got" << endl;
        }

        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */
            
            RWLock lk( "eliot2" , 120 * 1000 );
            
            auto_ptr<rwlock> a( new rwlock( lk , false ) );
            
            AtomicUInt x2 = 0;

            boost::thread t2( boost::bind( worker2, &lk , &x2 ) );
            t2.join();
            assert( x2 == 1 );

            a.reset();
            
        }
    };

    class RWLockTest4 { 
    public:
        
#if defined(__linux__) || defined(__APPLE__)
        static void worker1( pthread_rwlock_t * lk , AtomicUInt * x ) {
            (*x)++; // 1
            cout << "lock b try" << endl;
            while ( 1 ) {
                if ( pthread_rwlock_trywrlock( lk ) == 0 )
                    break;
                sleepmillis(10);
            }
            cout << "lock b got" << endl;
            (*x)++; // 2
            pthread_rwlock_unlock( lk );
        }

        static void worker2( pthread_rwlock_t * lk , AtomicUInt * x ) {
            cout << "lock c try" << endl;
            pthread_rwlock_rdlock( lk );
            (*x)++;
            cout << "lock c got" << endl;
            pthread_rwlock_unlock( lk );
        }
#endif
        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */
      
#if defined(__linux__) || defined(__APPLE__)      
            
            // create
            pthread_rwlock_t lk;
            assert( pthread_rwlock_init( &lk , 0 ) == 0 );
            
            // read lock
            assert( pthread_rwlock_rdlock( &lk ) == 0 );
            
            AtomicUInt x1 = 0;
            boost::thread t1( boost::bind( worker1 , &lk , &x1 ) );
            while ( ! x1 );
            assert( x1 == 1 );
            sleepmillis( 500 );
            assert( x1 == 1 );
            
            AtomicUInt x2 = 0;

            boost::thread t2( boost::bind( worker2, &lk , &x2 ) );
            t2.join();
            assert( x2 == 1 );

            pthread_rwlock_unlock( &lk );

            for ( int i=0; i<2000; i++ ) {
                if ( x1 == 2 )
                    break;
                sleepmillis(1);
            }

            assert( x1 == 2 );
            t1.join();
#endif            
        }
    };

    class List1Test2 : public ThreadedTest<> {
        static const int iterations = 1000; // note: a lot of iterations will use a lot of memory as List1 leaks on purpose
        class M : public List1<M>::Base {
        public:
            M(int x) : _x(x) { }
            const int _x;
        };
        List1<M> l;
    public:
        void validate() { }
        void subthread(int) {
            for(int i=0; i < iterations; i++) {
                int r = std::rand() % 256;
                if( r == 0 ) {
                    l.orphanAll();
                }
                else if( r < 4 ) { 
                    l.push(new M(r));
                }
                else {
                    M *orph = 0;
                    for( M *m = l.head(); m; m=m->next() ) { 
                        ASSERT( m->_x > 0 && m->_x < 4 );
                        if( r > 192 && std::rand() % 8 == 0 )
                            orph = m;
                    }
                    if( orph ) {
                        try { 
                            l.orphan(orph);
                        }
                        catch(...) { }
                    }
                }
            }
        }
    };

    class List1Test {
    public:
        class M : public List1<M>::Base {
            ~M();
        public:
            M( int x ) {
                num = x;
            }
            int num;
        };

        void run(){
            List1<M> l;
            
            vector<M*> ms;
            for ( int i=0; i<5; i++ ) {
                M * m = new M(i);
                ms.push_back( m );
                l.push( m );
            }
            
            // must assert as the item is missing
            ASSERT_EXCEPTION( l.orphan( new M( -3 ) ) , UserException );
        }
    };

#if 0
    class UpgradableTest : public ThreadedTest<7> {
        RWLock m;
    public:
        UpgradableTest() : m("utest") {}
    private:
        virtual void validate() { }
        virtual void subthread(int x) {
            Client::initThread("utest");

            /* r = read lock 
               R = get a read lock and we expect it to be fast
               w = write lock
            */
            //                    /-- verify upgrade can be done instantly while in a read lock already
            //                    |  /-- verify upgrade acquisition isn't greedy
            //                    |  | /-- verify writes aren't greedy while in upgradable
            //                    v  v v
            const char *what = " RURuRwR";

            sleepmillis(100*x);

            log() << x << what[x] << " request" << endl;
            switch( what[x] ) { 
            case 'w':
                {
                    m.lock();
                    log() << x << " W got" << endl;
                    sleepmillis(100);
                    log() << x << " W unlock" << endl;
                    m.unlock();
                }
                break;
            case 'u':
            case 'U':
                {
                    Timer t;
                    m.lockAsUpgradable();
                    log() << x << " U got" << endl;
                    if( what[x] == 'U' ) {
                        if( t.millis() > 20 ) {
                            DEV {
                                // a _DEBUG buildbot might be slow, try to avoid false positives
                                log() << "warning lock upgrade was slow " << t.millis() << endl;
                            }
                            else {
                                ASSERT( false );
                            }
                        }
                    }
                    sleepsecs(1);
                    log() << x << " U unlock" << endl;
                    m.unlockFromUpgradable();
                }
                break;
            case 'r':
            case 'R':
                {
                    Timer t;
                    m.lock_shared();
                    log() << x << " R got " << endl;
                    if( what[x] == 'R' ) {
                        if( t.millis() > 15 ) { 
                            log() << "warning: when in upgradable write locks are still greedy on this platform" << endl;
                        }
                    }
                    sleepmillis(200);
                    log() << x << " R unlock" << endl;
                    m.unlock_shared();
                }
                break;
            default:
                ASSERT(false);
            }

            cc().shutdown();
        }
    };
#endif

    void sleepalittle() {
        Timer t;
        while( 1 ) {
            boost::this_thread::yield();
            if( t.micros() > 8 )
                break;
        }
    }

    class WriteLocksAreGreedy : public ThreadedTest<3> {
    public:
        WriteLocksAreGreedy() : m("gtest") {}
    private:
        RWLock m;
        virtual void validate() { }
        virtual void subthread(int x) {
            Client::initThread("utest");
            if( x == 1 ) { 
                cout << mongo::curTimeMillis64() % 10000 << " 1" << endl;
                rwlock_shared lk(m);
                sleepmillis(300);
                cout << mongo::curTimeMillis64() % 10000 << " 1x" << endl;
            }
            if( x == 2 ) {
                sleepmillis(100);
                cout << mongo::curTimeMillis64() % 10000 << " 2" << endl;
                rwlock lk(m, true);
                //m._lock();
                cout << mongo::curTimeMillis64() % 10000 << " 2x" << endl;
                //m.unlock();
            }
            if( x == 3 ) {
                sleepmillis(200);
                Timer t;
                cout << mongo::curTimeMillis64() % 10000 << " 3" << endl;
                rwlock_shared lk(m);
                cout << mongo::curTimeMillis64() % 10000 << " 3x" << endl;
                cout << t.millis() << endl;
                ASSERT( t.millis() > 50 );
            }
            cc().shutdown();
        }
    };

    // Tests waiting on the TicketHolder by running many more threads than can fit into the "hotel", but only
    // max _nRooms threads should ever get in at once
    class TicketHolderWaits : public ThreadedTest<10> {

        static const int checkIns = 1000;
        static const int rooms = 3;

    public:
        TicketHolderWaits() : _hotel( rooms ), _tickets( _hotel._nRooms ) {}

    private:

        class Hotel {
        public:
            Hotel( int nRooms ) : _frontDesk( "frontDesk" ), _nRooms( nRooms ), _checkedIn( 0 ), _maxRooms( 0 ) {}

            void checkIn(){
                scoped_lock lk( _frontDesk );
                _checkedIn++;
                assert( _checkedIn <= _nRooms );
                if( _checkedIn > _maxRooms ) _maxRooms = _checkedIn;
            }

            void checkOut(){
                scoped_lock lk( _frontDesk );
                _checkedIn--;
                assert( _checkedIn >= 0 );
            }

            mongo::mutex _frontDesk;
            int _nRooms;
            int _checkedIn;
            int _maxRooms;
        };

        Hotel _hotel;
        TicketHolder _tickets;

        virtual void subthread(int x) {

            string threadName = ( str::stream() << "ticketHolder" << x );
            Client::initThread( threadName.c_str() );

            for( int i = 0; i < checkIns; i++ ){

                _tickets.waitForTicket();
                TicketHolderReleaser whenDone( &_tickets );

                _hotel.checkIn();

                sleepalittle();
                if( i == checkIns - 1 ) sleepsecs( 2 );

                _hotel.checkOut();

                if( ( i % ( checkIns / 10 ) ) == 0 )
                    log() << "checked in " << i << " times..." << endl;

            }

            cc().shutdown();

        }

        virtual void validate() {

            // This should always be true, assuming that it takes < 1 sec for the hardware to process a check-out/check-in
            // Time for test is then ~ #threads / _nRooms * 2 seconds
            assert( _hotel._maxRooms == _hotel._nRooms );

        }

    };


    class All : public Suite {
    public:
        All() : Suite( "threading" ) { }

        void setupTests() {
            add< WriteLocksAreGreedy >();
            //add< UpgradableTest >();
            add< List1Test >();
            add< List1Test2 >();

            add< IsAtomicUIntAtomic >();
            add< MVarTest >();
            add< ThreadPoolTest >();
            add< LockTest >();

            add< RWLockTest1 >();
            //add< RWLockTest2 >(); // SERVER-2996
            add< RWLockTest3 >();
            add< RWLockTest4 >();

            add< MongoMutexTest >();
            add< TicketHolderWaits >();
        }
    } myall;
}
