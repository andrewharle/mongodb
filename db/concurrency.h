/* concurrency.h

   mongod concurrency rules & notes will be placed here.

   Mutex heirarchy (1 = "leaf")
     name                   level
     Logstream::mutex       1
     ClientCursor::ccmutex  2
     dblock                 3

     End func name with _inlock to indicate "caller must lock before calling".
*/

#pragma once

#if BOOST_VERSION >= 103500
#include <boost/thread/shared_mutex.hpp>
#undef assert
#define assert xassert
#else
#warning built with boost version 1.34 or older limited concurrency
#endif

namespace mongo {

    /* mutex time stats */
    class MutexInfo {
        unsigned long long start, enter, timeLocked; // all in microseconds
        int locked;

    public:
        MutexInfo() : locked(0) {
            start = curTimeMicros64();
        }
        void entered() {
            if ( locked == 0 )
                enter = curTimeMicros64();
            locked++;
            assert( locked >= 1 );
        }
        void leaving() {
            locked--;
            assert( locked >= 0 );
            if ( locked == 0 )
                timeLocked += curTimeMicros64() - enter;
        }
        int isLocked() const {
            return locked;
        }
        void getTimingInfo(unsigned long long &s, unsigned long long &tl) const {
            s = start;
            tl = timeLocked;
        }
    };

#if BOOST_VERSION >= 103500
//#if 0
    class MongoMutex {
        MutexInfo _minfo;
        boost::shared_mutex _m;
        ThreadLocalValue<int> _state;

        /* we use a separate TLS value for releasedEarly - that is ok as 
           our normal/common code path, we never even touch it.
        */
        ThreadLocalValue<bool> _releasedEarly;
    public:
        /**
         * @return
         *    > 0  write lock
         *    = 0  no lock
         *    < 0  read lock
         */
        int getState(){ return _state.get(); }
        void assertWriteLocked() { 
            assert( getState() > 0 ); 
            DEV assert( !_releasedEarly.get() );
        }
        bool atLeastReadLocked() { return _state.get() != 0; }
        void assertAtLeastReadLocked() { assert(atLeastReadLocked()); }

        void lock() { 
            DEV cout << "LOCK" << endl;
            int s = _state.get();
            if( s > 0 ) {
                _state.set(s+1);
                return;
            }
            massert( 10293 , "internal error: locks are not upgradeable", s == 0 );
            _state.set(1);
            _m.lock(); 
            _minfo.entered();
        }
        void unlock() { 
            DEV cout << "UNLOCK" << endl;
            int s = _state.get();
            if( s > 1 ) { 
                _state.set(s-1);
                return;
            }
            if( s != 1 ) { 
                if( _releasedEarly.get() ) { 
                    _releasedEarly.set(false);
                    return;
                }
                assert(false); // attempt to unlock when wasn't in a write lock
            }
            _state.set(0);
            _minfo.leaving();
            _m.unlock(); 
        }

        /* unlock (write lock), and when unlock() is called later, 
           be smart then and don't unlock it again.
           */
        void releaseEarly() {
            assert( getState() == 1 ); // must not be recursive
            assert( !_releasedEarly.get() );
            _releasedEarly.set(true);
            unlock();
        }

        void lock_shared() { 
            DEV cout << " LOCKSHARED" << endl;
            int s = _state.get();
            if( s ) {
                if( s > 0 ) { 
                    // already in write lock - just be recursive and stay write locked
                    _state.set(s+1);
                    return;
                }
                else { 
                    // already in read lock - recurse
                    _state.set(s-1);
                    return;
                }
            }
            _state.set(-1);
            _m.lock_shared(); 
        }
        void unlock_shared() { 
            DEV cout << " UNLOCKSHARED" << endl;
            int s = _state.get();
            if( s > 0 ) { 
                assert( s > 1 ); /* we must have done a lock write first to have s > 1 */
                _state.set(s-1);
                return;
            }
            if( s < -1 ) { 
                _state.set(s+1);
                return;
            }
            assert( s == -1 );
            _state.set(0);
            _m.unlock_shared(); 
        }
        MutexInfo& info() { return _minfo; }
    };
#else
    /* this will be for old versions of boost */
    class MongoMutex { 
        MutexInfo _minfo;
        boost::recursive_mutex m;
        ThreadLocalValue<bool> _releasedEarly;
    public:
        MongoMutex() { }
        void lock() { 
#if BOOST_VERSION >= 103500
            m.lock();
#else
            boost::detail::thread::lock_ops<boost::recursive_mutex>::lock(m);
#endif
            _minfo.entered();
        }

        void releaseEarly() {
            assertWriteLocked(); // aso must not be recursive, although we don't verify that in the old boost version
            assert( !_releasedEarly.get() );
            _releasedEarly.set(true);
            _unlock();
        }

        void _unlock() {
            _minfo.leaving();
#if BOOST_VERSION >= 103500
            m.unlock();
#else
            boost::detail::thread::lock_ops<boost::recursive_mutex>::unlock(m);
#endif
        }
        void unlock() { 
            if( _releasedEarly.get() ) { 
                _releasedEarly.set(false);
                return;
            }
            _unlock();
        }

        void lock_shared() { lock(); }
        void unlock_shared() { unlock(); }
        MutexInfo& info() { return _minfo; }
        void assertWriteLocked() { 
            assert( info().isLocked() );
        }
        void assertAtLeastReadLocked() { 
            assert( info().isLocked() );
        }
        bool atLeastReadLocked() { return info().isLocked(); }
        int getState(){ return info().isLocked() ? 1 : 0; }
    };
#endif

    extern MongoMutex &dbMutex;

	void dbunlocking_write();
	void dbunlocking_read();

    struct writelock {
        writelock(const string& ns) {
            dbMutex.lock();
        }
        ~writelock() { 
            dbunlocking_write();
            dbMutex.unlock();
        }
    };
    
    struct readlock {
        readlock(const string& ns) {
            dbMutex.lock_shared();
        }
        ~readlock() { 
            dbunlocking_read();
            dbMutex.unlock_shared();
        }
    };
    
    class mongolock {
        bool _writelock;
    public:
        mongolock(bool write) : _writelock(write) {
            if( _writelock ) {
                dbMutex.lock();
            }
            else
                dbMutex.lock_shared();
        }
        ~mongolock() { 
            if( _writelock ) { 
                dbunlocking_write();
                dbMutex.unlock();
            }
            else {
                dbunlocking_read();
                dbMutex.unlock_shared();
            }
        }
        /* this unlocks, does NOT upgrade. that works for our current usage */
        void releaseAndWriteLock();
    };
    
	/* use writelock and readlock instead */
    struct dblock : public writelock {
        dblock() : writelock("") { }
        ~dblock() { 
        }
    };

    // eliminate
    inline void assertInWriteLock() { dbMutex.assertWriteLocked(); }

}
