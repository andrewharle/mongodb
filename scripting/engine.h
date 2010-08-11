// engine.h

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

#include "../pch.h"
#include "../db/jsobj.h"

extern const char * jsconcatcode; // TODO: change name to mongoJSCode

namespace mongo {

    typedef unsigned long long ScriptingFunction;
    typedef BSONObj (*NativeFunction) ( const BSONObj &args );

    class Scope : boost::noncopyable {
    public:
        Scope();
        virtual ~Scope();
        
        virtual void reset() = 0;
        virtual void init( BSONObj * data ) = 0;
        void init( const char * data ){
            BSONObj o( data , 0 );
            init( &o );
        }
        
        virtual void localConnect( const char * dbName ) = 0;
        virtual void externalSetup() = 0;
        
        virtual double getNumber( const char *field ) = 0;
        virtual int getNumberInt( const char *field ){ return (int)getNumber( field ); }
        virtual long long getNumberLongLong( const char *field ){ return (long long)getNumber( field ); }
        virtual string getString( const char *field ) = 0;
        virtual bool getBoolean( const char *field ) = 0;
        virtual BSONObj getObject( const char *field ) = 0;

        virtual int type( const char *field ) = 0;

        void append( BSONObjBuilder & builder , const char * fieldName , const char * scopeName );

        virtual void setElement( const char *field , const BSONElement& e ) = 0;
        virtual void setNumber( const char *field , double val ) = 0;
        virtual void setString( const char *field , const char * val ) = 0;
        virtual void setObject( const char *field , const BSONObj& obj , bool readOnly=true ) = 0;
        virtual void setBoolean( const char *field , bool val ) = 0;
        virtual void setThis( const BSONObj * obj ) = 0;
                    
        virtual ScriptingFunction createFunction( const char * code );
        
        /**
         * @return 0 on success
         */
        virtual int invoke( ScriptingFunction func , const BSONObj& args, int timeoutMs = 0 , bool ignoreReturn = false ) = 0;
        void invokeSafe( ScriptingFunction func , const BSONObj& args, int timeoutMs = 0 ){
            int res = invoke( func , args , timeoutMs );
            if ( res == 0 )
                return;
            throw UserException( 9004 , (string)"invoke failed: " + getError() );
        }
        virtual string getError() = 0;
        
        int invoke( const char* code , const BSONObj& args, int timeoutMs = 0 );
        void invokeSafe( const char* code , const BSONObj& args, int timeoutMs = 0 ){
            if ( invoke( code , args , timeoutMs ) == 0 )
                return;
            throw UserException( 9005 , (string)"invoke failed: " + getError() );
        }

        virtual bool exec( const string& code , const string& name , bool printResult , bool reportError , bool assertOnError, int timeoutMs = 0 ) = 0;
        virtual void execSetup( const string& code , const string& name = "setup" ){
            exec( code , name , false , true , true , 0 );
        }
        virtual bool execFile( const string& filename , bool printResult , bool reportError , bool assertOnError, int timeoutMs = 0 );
        
        virtual void injectNative( const char *field, NativeFunction func ) = 0;

        virtual void gc() = 0;

        void loadStored( bool ignoreNotConnected = false );
        
        /**
         if any changes are made to .system.js, call this
         right now its just global - slightly inefficient, but a lot simpler
        */
        static void storedFuncMod();
        
        static int getNumScopes(){
            return _numScopes;
        }
        
        static void validateObjectIdString( const string &str );
        
    protected:

        virtual ScriptingFunction _createFunction( const char * code ) = 0;

        string _localDBName;
        long long _loadedVersion;
        set<string> _storedNames;
        static long long _lastVersion;
        map<string,ScriptingFunction> _cachedFunctions;

        static int _numScopes;
    };
    
    void installGlobalUtils( Scope& scope );

    class DBClientWithCommands;
    
    class ScriptEngine : boost::noncopyable {
    public:
        ScriptEngine();
        virtual ~ScriptEngine();
        
        virtual Scope * newScope() {
            Scope *s = createScope();
            if ( s && _scopeInitCallback )
                _scopeInitCallback( *s );
            installGlobalUtils( *s );
            return s;
        }
        
        virtual void runTest() = 0;
        
        virtual bool utf8Ok() const = 0;

        static void setup();

        auto_ptr<Scope> getPooledScope( const string& pool );
        void threadDone();
        
        struct Unlocker { virtual ~Unlocker() {} };
        virtual auto_ptr<Unlocker> newThreadUnlocker() { return auto_ptr< Unlocker >( new Unlocker ); }
        
        void setScopeInitCallback( void ( *func )( Scope & ) ) { _scopeInitCallback = func; }
        static void setConnectCallback( void ( *func )( DBClientWithCommands& ) ) { _connectCallback = func; }
        static void runConnectCallback( DBClientWithCommands &c ) {
            if ( _connectCallback )
                _connectCallback( c );
        }
        
    protected:
        virtual Scope * createScope() = 0;
        
    private:
        void ( *_scopeInitCallback )( Scope & );
        static void ( *_connectCallback )( DBClientWithCommands & );
    };

    bool hasJSReturn( const string& s );

    extern ScriptEngine * globalScriptEngine;
}
