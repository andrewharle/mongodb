// assert_util.h

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

#include "../db/lasterror.h"

// MONGO_NORETURN undefed at end of file
#ifdef __GNUC__
# define MONGO_NORETURN __attribute__((__noreturn__))
#else
# define MONGO_NORETURN 
#endif

namespace mongo {

    enum CommonErrorCodes {
        DatabaseDifferCaseCode = 13297 ,
        StaleConfigInContextCode = 13388
    };

    class AssertionCount {
    public:
        AssertionCount();
        void rollover();
        void condrollover( int newValue );

        int regular;
        int warning;
        int msg;
        int user;
        int rollovers;
    };

    extern AssertionCount assertionCount;

    struct ExceptionInfo {
        ExceptionInfo() : msg(""),code(-1) {}
        ExceptionInfo( const char * m , int c )
            : msg( m ) , code( c ) {
        }
        ExceptionInfo( const string& m , int c )
            : msg( m ) , code( c ) {
        }
        void append( BSONObjBuilder& b , const char * m = "$err" , const char * c = "code" ) const ;
        string toString() const { stringstream ss; ss << "exception: " << code << " " << msg; return ss.str(); }
        bool empty() const { return msg.empty(); }
        
        void reset(){ msg = ""; code=-1; }

        string msg;
        int code;
    };

    /** helper class that builds error strings.  lighter weight than a StringBuilder, albeit less flexible.
        NOINLINE_DECL used in the constructor implementations as we are assuming this is a cold code path when used.

        example: 
          throw UserException(123, ErrorMsg("blah", num_val));
    */
    class ErrorMsg { 
    public:
        ErrorMsg(const char *msg, char ch);
        ErrorMsg(const char *msg, unsigned val);
        operator string() const { return buf; }
    private:
        char buf[256];
    };

    class DBException : public std::exception {
    public:
        DBException( const ExceptionInfo& ei ) : _ei(ei) {}
        DBException( const char * msg , int code ) : _ei(msg,code) {}
        DBException( const string& msg , int code ) : _ei(msg,code) {}
        virtual ~DBException() throw() { }

        virtual const char* what() const throw() { return _ei.msg.c_str(); }
        virtual int getCode() const { return _ei.code; }

        virtual void appendPrefix( stringstream& ss ) const { }

        virtual string toString() const {
            stringstream ss; ss << getCode() << " " << what(); return ss.str();
            return ss.str();
        }

        const ExceptionInfo& getInfo() const { return _ei; }

    protected:
        ExceptionInfo _ei;
    };

    class AssertionException : public DBException {
    public:

        AssertionException( const ExceptionInfo& ei ) : DBException(ei) {}
        AssertionException( const char * msg , int code ) : DBException(msg,code) {}
        AssertionException( const string& msg , int code ) : DBException(msg,code) {}

        virtual ~AssertionException() throw() { }

        virtual bool severe() { return true; }
        virtual bool isUserAssertion() { return false; }

        /* true if an interrupted exception - see KillCurrentOp */
        bool interrupted() {
            return _ei.code == 11600 || _ei.code == 11601;
        }
    };

    /* UserExceptions are valid errors that a user can cause, like out of disk space or duplicate key */
    class UserException : public AssertionException {
    public:
        UserException(int c , const string& m) : AssertionException( m , c ) {}

        virtual bool severe() { return false; }
        virtual bool isUserAssertion() { return true; }
        virtual void appendPrefix( stringstream& ss ) const { ss << "userassert:"; }
    };

    class MsgAssertionException : public AssertionException {
    public:
        MsgAssertionException( const ExceptionInfo& ei ) : AssertionException( ei ) {}
        MsgAssertionException(int c, const string& m) : AssertionException( m , c ) {}
        virtual bool severe() { return false; }
        virtual void appendPrefix( stringstream& ss ) const { ss << "massert:"; }
    };

    void asserted(const char *msg, const char *file, unsigned line) MONGO_NORETURN;
    void wasserted(const char *msg, const char *file, unsigned line);
    void verifyFailed( int msgid );
    
    /** a "user assertion".  throws UserAssertion.  logs.  typically used for errors that a user
        could cause, such as duplicate key, disk full, etc.
    */
    void uasserted(int msgid, const char *msg) MONGO_NORETURN;
    inline void uasserted(int msgid , string msg) { uasserted(msgid, msg.c_str()); }

    /** reported via lasterror, but don't throw exception */
    void uassert_nothrow(const char *msg);

    /** msgassert and massert are for errors that are internal but have a well defined error text string.
        a stack trace is logged.
    */
    void msgassertedNoTrace(int msgid, const char *msg) MONGO_NORETURN;
    inline void msgassertedNoTrace(int msgid, const string& msg) { msgassertedNoTrace( msgid , msg.c_str() ); }
    void msgasserted(int msgid, const char *msg) MONGO_NORETURN;
    inline void msgasserted(int msgid, string msg) { msgasserted(msgid, msg.c_str()); }

    /* convert various types of exceptions to strings */
    inline string causedBy( const char* e ){ return (string)" :: caused by :: " + e; }
    inline string causedBy( const DBException& e ){ return causedBy( e.toString().c_str() ); }
    inline string causedBy( const std::exception& e ){ return causedBy( e.what() ); }
    inline string causedBy( const string& e ){ return causedBy( e.c_str() ); }

    /** in the mongodb source, use verify() instead of assert().  verify is always evaluated even in release builds. */
    inline void verify( int msgid , bool testOK ) { if ( ! testOK ) verifyFailed( msgid ); }

#define MONGO_assert(_Expression) (void)( MONGO_likely(!!(_Expression)) || (mongo::asserted(#_Expression, __FILE__, __LINE__), 0) )

    /* "user assert".  if asserts, user did something wrong, not our code */
#define MONGO_uassert(msgid, msg, expr) (void)( MONGO_likely(!!(expr)) || (mongo::uasserted(msgid, msg), 0) )

    /* warning only - keeps going */
#define MONGO_wassert(_Expression) (void)( MONGO_likely(!!(_Expression)) || (mongo::wasserted(#_Expression, __FILE__, __LINE__), 0) )

    /* display a message, no context, and throw assertionexception

       easy way to throw an exception and log something without our stack trace
       display happening.
    */
#define MONGO_massert(msgid, msg, expr) (void)( MONGO_likely(!!(expr)) || (mongo::msgasserted(msgid, msg), 0) )

    /* dassert is 'debug assert' -- might want to turn off for production as these
       could be slow.
    */
#if defined(_DEBUG)
# define MONGO_dassert assert
#else
# define MONGO_dassert(x)
#endif

    // some special ids that we want to duplicate

    // > 10000 asserts
    // < 10000 UserException

    enum { ASSERT_ID_DUPKEY = 11000 };

    /* throws a uassertion with an appropriate msg */
    void streamNotGood( int code , string msg , std::ios& myios ) MONGO_NORETURN;

    inline void assertStreamGood(unsigned msgid, string msg, std::ios& myios) {
        if( !myios.good() ) streamNotGood(msgid, msg, myios);
    }

    string demangleName( const type_info& typeinfo );

} // namespace mongo

#define BOOST_CHECK_EXCEPTION MONGO_BOOST_CHECK_EXCEPTION
#define MONGO_BOOST_CHECK_EXCEPTION( expression ) \
    try { \
        expression; \
    } catch ( const std::exception &e ) { \
        stringstream ss; \
        ss << "caught boost exception: " << e.what() << ' ' << __FILE__ << ' ' << __LINE__; \
        msgasserted( 13294 , ss.str() ); \
    } catch ( ... ) { \
        massert( 10437 ,  "unknown boost failed" , false ); \
    }

#define MONGO_BOOST_CHECK_EXCEPTION_WITH_MSG( expression, msg ) \
    try { \
        expression; \
    } catch ( const std::exception &e ) { \
        stringstream ss; \
        ss << msg << " caught boost exception: " << e.what();   \
        msgasserted( 14043 , ss.str() );        \
    } catch ( ... ) { \
        msgasserted( 14044 , string("unknown boost failed ") + msg );   \
    }

#define DESTRUCTOR_GUARD MONGO_DESTRUCTOR_GUARD
#define MONGO_DESTRUCTOR_GUARD( expression ) \
    try { \
        expression; \
    } catch ( const std::exception &e ) { \
        problem() << "caught exception (" << e.what() << ") in destructor (" << __FUNCTION__ << ")" << endl; \
    } catch ( ... ) { \
        problem() << "caught unknown exception in destructor (" << __FUNCTION__ << ")" << endl; \
    }

#undef MONGO_NORETURN
