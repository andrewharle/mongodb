// goodies.h
// miscellaneous junk

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

#pragma once

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace mongo {

#if !defined(_WIN32) && !defined(NOEXECINFO) && !defined(__freebsd__) && !defined(__sun__)

} // namespace mongo

#include <pthread.h>
#include <execinfo.h>

namespace mongo {

    inline pthread_t GetCurrentThreadId() {
        return pthread_self();
    }

    /* use "addr2line -CFe <exe>" to parse. */
    inline void printStackTrace( ostream &o = cout ) {
        void *b[20];
        size_t size;
        char **strings;
        size_t i;

        size = backtrace(b, 20);
        strings = backtrace_symbols(b, size);

        for (i = 0; i < size; i++)
            o << hex << b[i] << dec << ' ';
        o << '\n';
        for (i = 0; i < size; i++)
            o << ' ' << strings[i] << '\n';

        free (strings);
    }
#else
    inline void printStackTrace( ostream &o = cout ) { }
#endif

    /* set to TRUE if we are exiting */
    extern bool goingAway;

    /* find the multimap member which matches a particular key and value.

       note this can be slow if there are a lot with the same key.
    */
    template<class C,class K,class V> inline typename C::iterator kv_find(C& c, const K& k,const V& v) {
        pair<typename C::iterator,typename C::iterator> p = c.equal_range(k);

        for ( typename C::iterator it=p.first; it!=p.second; ++it)
            if ( it->second == v )
                return it;

        return c.end();
    }

    bool isPrime(int n);
    int nextPrime(int n);

    inline void dumpmemory(const char *data, int len) {
        if ( len > 1024 )
            len = 1024;
        try {
            const char *q = data;
            const char *p = q;
            while ( len > 0 ) {
                for ( int i = 0; i < 16; i++ ) {
                    if ( *p >= 32 && *p <= 126 )
                        cout << *p;
                    else
                        cout << '.';
                    p++;
                }
                cout << "  ";
                p -= 16;
                for ( int i = 0; i < 16; i++ )
                    cout << (unsigned) ((unsigned char)*p++) << ' ';
                cout << endl;
                len -= 16;
            }
        } catch (...) {
        }
    }

// PRINT(2+2);  prints "2+2: 4"
#define PRINT(x) cout << #x ": " << (x) << endl
// PRINTFL; prints file:line
#define PRINTFL cout << __FILE__ ":" << __LINE__ << endl

#undef yassert

#undef assert
#define assert xassert
#define yassert 1

    struct WrappingInt {
        WrappingInt() {
            x = 0;
        }
        WrappingInt(unsigned z) : x(z) { }
        unsigned x;
        operator unsigned() const {
            return x;
        }


        static int diff(unsigned a, unsigned b) {
            return a-b;
        }
        bool operator<=(WrappingInt r) {
            // platform dependent
            int df = (r.x - x);
            return df >= 0;
        }
        bool operator>(WrappingInt r) {
            return !(r<=*this);
        }
    };

} // namespace mongo

#include <ctime>

namespace mongo {

    inline void time_t_to_String(time_t t, char *buf) {
#if defined(_WIN32)
        ctime_s(buf, 64, &t);
#else
        ctime_r(&t, buf);
#endif
        buf[24] = 0; // don't want the \n
    }


    inline void time_t_to_Struct(time_t t, struct tm * buf , bool local = false ) {
#if defined(_WIN32)
        if ( local )
            localtime_s( buf , &t );
        else
            gmtime_s(buf, &t);
#else
        if ( local )
            localtime_r(&t, buf);
        else
            gmtime_r(&t, buf);
#endif
    }



#define asctime _asctime_not_threadsafe_
#define gmtime _gmtime_not_threadsafe_
#define localtime _localtime_not_threadsafe_
#define ctime _ctime_is_not_threadsafe_

#if defined(_WIN32) || defined(__sunos__)
    inline void sleepsecs(int s) {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += s;
        boost::thread::sleep(xt);
    }
    inline void sleepmillis(int s) {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += ( s / 1000 );
        xt.nsec += ( s % 1000 ) * 1000000;
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }        
        boost::thread::sleep(xt);
    }
    inline void sleepmicros(int s) {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += ( s / 1000000 );
        xt.nsec += ( s % 1000000 ) * 1000;
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }        
        boost::thread::sleep(xt);
    }
#else
    inline void sleepsecs(int s) {
        struct timespec t;
        t.tv_sec = s;
        t.tv_nsec = 0;
        if ( nanosleep( &t , 0 ) ){
            cout << "nanosleep failed" << endl;
        }
    }
    inline void sleepmicros(int s) {
        struct timespec t;
        t.tv_sec = (int)(s / 1000000);
        t.tv_nsec = s % 1000000;
        if ( nanosleep( &t , 0 ) ){
            cout << "nanosleep failed" << endl;
        }
    }
    inline void sleepmillis(int s) {
        sleepmicros( s * 1000 );
    }
#endif

// note this wraps
    inline int tdiff(unsigned told, unsigned tnew) {
        return WrappingInt::diff(tnew, told);
    }
    inline unsigned curTimeMillis() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned t = xt.nsec / 1000000;
        return (xt.sec & 0xfffff) * 1000 + t;
    }

    struct Date_t {
        // TODO: make signed (and look for related TODO's)
        unsigned long long millis;
        Date_t(): millis(0) {}
        Date_t(unsigned long long m): millis(m) {}
        operator unsigned long long&() { return millis; }
        operator const unsigned long long&() const { return millis; }
    };

    inline Date_t jsTime() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned long long t = xt.nsec / 1000000;
        return ((unsigned long long) xt.sec * 1000) + t;
    }

    inline unsigned long long curTimeMicros64() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned long long t = xt.nsec / 1000;
        return (((unsigned long long) xt.sec) * 1000000) + t;
    }

// measures up to 1024 seconds.  or, 512 seconds with tdiff that is...
    inline unsigned curTimeMicros() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned t = xt.nsec / 1000;
        unsigned secs = xt.sec % 1024;
        return secs*1000000 + t;
    }
    using namespace boost;
    
    extern bool __destroyingStatics;
    
    // If you create a local static instance of this class, that instance will be destroyed
    // before all global static objects are destroyed, so __destroyingStatics will be set
    // to true before the global static variables are destroyed.
    class StaticObserver : boost::noncopyable {
    public:
        ~StaticObserver() { __destroyingStatics = true; }
    };
    
    // On pthread systems, it is an error to destroy a mutex while held.  Static global
    // mutexes may be held upon shutdown in our implementation, and this way we avoid
    // destroying them.
    class mutex : boost::noncopyable {
    public:
        mutex() { new (_buf) boost::mutex(); }
        ~mutex() {
            if( !__destroyingStatics ) {
                boost().boost::mutex::~mutex();
            }
        }
        class scoped_lock : boost::noncopyable {
        public:
            scoped_lock( mongo::mutex &m ) : _l( m.boost() ) {}
            boost::mutex::scoped_lock &boost() { return _l; }
        private:
            boost::mutex::scoped_lock _l;
        };
    private:
        boost::mutex &boost() { return *( boost::mutex * )( _buf ); }
        char _buf[ sizeof( boost::mutex ) ];
    };
    
    typedef mongo::mutex::scoped_lock scoped_lock;
    typedef boost::recursive_mutex::scoped_lock recursive_scoped_lock;

// simple scoped timer
    class Timer {
    public:
        Timer() {
            reset();
        }
        Timer( unsigned long long start ) {
            old = start;
        }
        int seconds(){
            return (int)(micros() / 1000000);
        }
        int millis() {
            return (long)(micros() / 1000);
        }
        unsigned long long micros() {
            unsigned long long n = curTimeMicros64();
            return n - old;
        }
        unsigned long long micros(unsigned long long & n) { // returns cur time in addition to timer result
            n = curTimeMicros64();
            return n - old;
        }
        unsigned long long startTime(){
            return old;
        }
        void reset() {
            old = curTimeMicros64();
        }
    private:
        unsigned long long old;
    };

    /*

    class DebugMutex : boost::noncopyable {
    	friend class lock;
    	mongo::mutex m;
    	int locked;
    public:
    	DebugMutex() : locked(0); { }
    	bool isLocked() { return locked; }
    };

    */

//typedef scoped_lock lock;

    inline bool startsWith(const char *str, const char *prefix) {
        size_t l = strlen(prefix);
        if ( strlen(str) < l ) return false;
        return strncmp(str, prefix, l) == 0;
    }

    inline bool endsWith(const char *p, const char *suffix) {
        size_t a = strlen(p);
        size_t b = strlen(suffix);
        if ( b > a ) return false;
        return strcmp(p + a - b, suffix) == 0;
    }

} // namespace mongo

#include "boost/detail/endian.hpp"

namespace mongo {

    inline unsigned long swapEndian(unsigned long x) {
        return
            ((x & 0xff) << 24) |
            ((x & 0xff00) << 8) |
            ((x & 0xff0000) >> 8) |
            ((x & 0xff000000) >> 24);
    }

#if defined(BOOST_LITTLE_ENDIAN)
    inline unsigned long fixEndian(unsigned long x) {
        return x;
    }
#else
    inline unsigned long fixEndian(unsigned long x) {
        return swapEndian(x);
    }
#endif

    // Like strlen, but only scans up to n bytes.
    // Returns -1 if no '0' found.
    inline int strnlen( const char *s, int n ) {
        for( int i = 0; i < n; ++i )
            if ( !s[ i ] )
                return i;
        return -1;
    }
    
#if !defined(_WIN32)
    typedef int HANDLE;
    inline void strcpy_s(char *dst, unsigned len, const char *src) {
        strcpy(dst, src);
    }
#else
    typedef void *HANDLE;
#endif
    
    /* thread local "value" rather than a pointer
       good for things which have copy constructors (and the copy constructor is fast enough)
       e.g. 
         ThreadLocalValue<int> myint;
    */
    template<class T>
    class ThreadLocalValue {
    public:
        ThreadLocalValue( T def = 0 ) : _default( def ) { }

        T get() {
            T * val = _val.get();
            if ( val )
                return *val;
            return _default;
        }

        void set( const T& i ) {
            T *v = _val.get();
            if( v ) { 
                *v = i;
                return;
            }
            v = new T(i);
            _val.reset( v );
        }

    private:
        T _default;
        boost::thread_specific_ptr<T> _val;
    };

    class ProgressMeter {
    public:
        ProgressMeter( long long total , int secondsBetween = 3 , int checkInterval = 100 ){
            reset( total , secondsBetween , checkInterval );
        }

        ProgressMeter(){
            _active = 0;
        }
        
        void reset( long long total , int secondsBetween = 3 , int checkInterval = 100 ){
            _total = total;
            _secondsBetween = secondsBetween;
            _checkInterval = checkInterval;

            _done = 0;
            _hits = 0;
            _lastTime = (int)time(0);

            _active = 1;
        }

        void finished(){
            _active = 0;
        }

        bool isActive(){
            return _active;
        }
        
        bool hit( int n = 1 ){
            if ( ! _active ){
                cout << "warning: hit on in-active ProgressMeter" << endl;
            }

            _done += n;
            _hits++;
            if ( _hits % _checkInterval )
                return false;
            
            int t = (int) time(0);
            if ( t - _lastTime < _secondsBetween )
                return false;
            
            if ( _total > 0 ){
                int per = (int)( ( (double)_done * 100.0 ) / (double)_total );
                cout << "\t\t" << _done << "/" << _total << "\t" << per << "%" << endl;
            }
            _lastTime = t;
            return true;
        }

        long long done(){
            return _done;
        }
        
        long long hits(){
            return _hits;
        }

        string toString() const {
            if ( ! _active )
                return "";
            stringstream buf;
            buf << _done << "/" << _total << " " << (_done*100)/_total << "%";
            return buf.str();
        }
    private:

        bool _active;
        
        long long _total;
        int _secondsBetween;
        int _checkInterval;

        long long _done;
        long long _hits;
        int _lastTime;
    };

    class TicketHolder {
    public:
        TicketHolder( int num ){
            _outof = num;
            _num = num;
        }
        
        bool tryAcquire(){
            scoped_lock lk( _mutex );
            if ( _num <= 0 ){
                if ( _num < 0 ){
                    cerr << "DISASTER! in TicketHolder" << endl;
                }
                return false;
            }
            _num--;
            return true;
        }
        
        void release(){
            scoped_lock lk( _mutex );
            _num++;
        }

        void resize( int newSize ){
            scoped_lock lk( _mutex );            
            int used = _outof - _num;
            if ( used > newSize ){
                cout << "ERROR: can't resize since we're using (" << used << ") more than newSize(" << newSize << ")" << endl;
                return;
            }
            
            _outof = newSize;
            _num = _outof - used;
        }

        int available(){
            return _num;
        }

        int used(){
            return _outof - _num;
        }

    private:
        int _outof;
        int _num;
        mongo::mutex _mutex;
    };

    class TicketHolderReleaser {
    public:
        TicketHolderReleaser( TicketHolder * holder ){
            _holder = holder;
        }
        
        ~TicketHolderReleaser(){
            _holder->release();
        }
    private:
        TicketHolder * _holder;
    };


    /**
     * this is a thread safe string
     * you will never get a bad pointer, though data may be mungedd
     */
    class ThreadSafeString {
    public:
        ThreadSafeString( size_t size=256 )
            : _size( 256 ) , _buf( new char[256] ){
            memset( _buf , 0 , _size );
        }

        ThreadSafeString( const ThreadSafeString& other )
            : _size( other._size ) , _buf( new char[_size] ){
            strncpy( _buf , other._buf , _size );
        }

        ~ThreadSafeString(){
            delete[] _buf;
            _buf = 0;
        }
        
        operator string() const {
            string s = _buf;
            return s;
        }

        ThreadSafeString& operator=( const char * str ){
            size_t s = strlen(str);
            if ( s >= _size - 2 )
                s = _size - 2;
            strncpy( _buf , str , s );
            _buf[s] = 0;
            return *this;
        }
        
        bool operator==( const ThreadSafeString& other ) const {
            return strcmp( _buf , other._buf ) == 0;
        }

        bool operator==( const char * str ) const {
            return strcmp( _buf , str ) == 0;
        }

        bool operator!=( const char * str ) const {
            return strcmp( _buf , str );
        }

        bool empty() const {
            return _buf[0] == 0;
        }

    private:
        size_t _size;
        char * _buf;  
    };

    ostream& operator<<( ostream &s, const ThreadSafeString &o );

    inline bool isNumber( char c ) {
        return c >= '0' && c <= '9';
    }
    
    // for convenience, '{' is greater than anything and stops number parsing
    inline int lexNumCmp( const char *s1, const char *s2 ) {
        int nret = 0;
        while( *s1 && *s2 ) {
            bool p1 = ( *s1 == '{' );
            bool p2 = ( *s2 == '{' );
            if ( p1 && !p2 )
                return 1;
            if ( p2 && !p1 )
                return -1;
            bool n1 = isNumber( *s1 );
            bool n2 = isNumber( *s2 );
            if ( n1 && n2 ) {
                if ( nret == 0 ) {
                    nret = *s1 > *s2 ? 1 : ( *s1 == *s2 ? 0 : -1 );
                }
            } else if ( n1 ) {
                return 1;
            } else if ( n2 ) {
                return -1;
            } else {
                if ( nret ) {
                    return nret;
                }
                if ( *s1 > *s2 ) {
                    return 1;
                } else if ( *s2 > *s1 ) {
                    return -1;
                }
                nret = 0;
            }
            ++s1; ++s2;
        }
        if ( *s1 ) {
            return 1;
        } else if ( *s2 ) {
            return -1;
        }
        return nret;
    }
    
} // namespace mongo
