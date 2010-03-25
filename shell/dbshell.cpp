// dbshell.cpp
/*
 *    Copyright 2010 10gen Inc.
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


#include <stdio.h>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#include <setjmp.h>
jmp_buf jbuf;
#endif

#include "../scripting/engine.h"
#include "../client/dbclient.h"
#include "../util/unittest.h"
#include "../db/cmdline.h"
#include "utils.h"

using namespace std;
using namespace boost::filesystem;

string historyFile;
bool gotInterrupted = 0;
bool inMultiLine = 0;

#if defined(USE_READLINE) && !defined(__freebsd__) && !defined(_WIN32)
#define CTRLC_HANDLE
#endif

void shellHistoryInit(){
#ifdef USE_READLINE
    stringstream ss;
    char * h = getenv( "HOME" );
    if ( h )
        ss << h << "/";
    ss << ".dbshell";
    historyFile = ss.str();

    using_history();
    read_history( historyFile.c_str() );

#else
    cout << "type \"exit\" to exit" << endl;
#endif
}
void shellHistoryDone(){
#ifdef USE_READLINE
    write_history( historyFile.c_str() );
#endif
}
void shellHistoryAdd( const char * line ){
    if ( strlen(line) == 0 )
        return;
#ifdef USE_READLINE
    if ((strstr(line, ".auth")) == NULL)
        add_history( line );
#endif
}

void intr( int sig ){
#ifdef CTRLC_HANDLE
    longjmp( jbuf , 1 );
#endif
}

#if !defined(_WIN32)
void killOps() {
    if ( mongo::shellUtils::_nokillop || mongo::shellUtils::_allMyUris.size() == 0 )
        return;
    vector< string > uris;
    for( map< const void*, string >::iterator i = mongo::shellUtils::_allMyUris.begin(); i != mongo::shellUtils::_allMyUris.end(); ++i )
        uris.push_back( i->second );
    mongo::BSONObj spec = BSON( "" << uris );
    try {
        auto_ptr< mongo::Scope > scope( mongo::globalScriptEngine->newScope() );        
        scope->invoke( "function( x ) { killWithUris( x ); }", spec );
    } catch ( ... ) {
        mongo::rawOut( "exception while cleaning up any db ops started by this shell\n" );
    }
}

void quitNicely( int sig ){
    if ( sig == SIGINT && inMultiLine ){
        gotInterrupted = 1;
        return;
    }
    if ( sig == SIGPIPE )
        mongo::rawOut( "mongo got signal SIGPIPE\n" );
    killOps();
    shellHistoryDone();
    exit(0);
}
#endif

char * shellReadline( const char * prompt , int handlesigint = 0 ){
#ifdef USE_READLINE

#ifdef CTRLC_HANDLE
    if ( ! handlesigint )
        return readline( prompt );
    if ( setjmp( jbuf ) ){
        gotInterrupted = 1;
        sigrelse(SIGINT);
        signal( SIGINT , quitNicely );
        return 0;
    }
    signal( SIGINT , intr );
#endif

    char * ret = readline( prompt );
    signal( SIGINT , quitNicely );
    return ret;
#else
    printf("%s", prompt);
    char * buf = new char[1024];
    char * l = fgets( buf , 1024 , stdin );
    int len = strlen( buf );
    buf[len-1] = 0;
    return l;
#endif
}

#if !defined(_WIN32)
#include <string.h>

void quitAbruptly( int sig ) {
    ostringstream ossSig;
    ossSig << "mongo got signal " << sig << " (" << strsignal( sig ) << "), stack trace: " << endl;
    mongo::rawOut( ossSig.str() );

    ostringstream ossBt;
    mongo::printStackTrace( ossBt );
    mongo::rawOut( ossBt.str() );

    mongo::shellUtils::KillMongoProgramInstances();
    exit(14);
}

void setupSignals() {
    signal( SIGINT , quitNicely );
    signal( SIGTERM , quitNicely );
    signal( SIGPIPE , quitNicely ); // Maybe just log and continue?
    signal( SIGABRT , quitAbruptly );
    signal( SIGSEGV , quitAbruptly );
    signal( SIGBUS , quitAbruptly );
    signal( SIGFPE , quitAbruptly );
}
#else
inline void setupSignals() {}
#endif

string fixHost( string url , string host , string port ){
    //cout << "fixHost url: " << url << " host: " << host << " port: " << port << endl;

    if ( host.size() == 0 && port.size() == 0 ){
        if ( url.find( "/" ) == string::npos ){
            // check for ips
            if ( url.find( "." ) != string::npos )
                return url + "/test";

            if ( url.find( ":" ) != string::npos &&
                 isdigit( url[url.find(":")+1] ) )
                return url + "/test";
        }
        return url;
    }

    if ( url.find( "/" ) != string::npos ){
        cerr << "url can't have host or port if you specify them individually" << endl;
        exit(-1);
    }

    if ( host.size() == 0 )
        host = "127.0.0.1";

    string newurl = host;
    if ( port.size() > 0 )
        newurl += ":" + port;

    newurl += "/" + url;

    return newurl;
}

bool isBalanced( string code ){
    int brackets = 0;
    int parens = 0;

    for ( size_t i=0; i<code.size(); i++ ){
        switch( code[i] ){
        case '/':
            if ( i+1 < code.size() && code[i+1] == '/' ){
                while ( i<code.size() && code[i] != '\n' )
                    i++;
            }
            continue;
        case '{': brackets++; break;
        case '}': if ( brackets <= 0 ) return true; brackets--; break;
        case '(': parens++; break;
        case ')': if ( parens <= 0 ) return true; parens--; break;
        case '"':
            i++;
            while ( i < code.size() && code[i] != '"' ) i++;
            break;
        case '\'':
            i++;
            while ( i < code.size() && code[i] != '\'' ) i++;
            break;
        }
    }

    return brackets == 0 && parens == 0;
}

using mongo::asserted;

struct BalancedTest : public mongo::UnitTest {
public:
    void run(){
        assert( isBalanced( "x = 5" ) );
        assert( isBalanced( "function(){}" ) );
        assert( isBalanced( "function(){\n}" ) );
        assert( ! isBalanced( "function(){" ) );
        assert( isBalanced( "x = \"{\";" ) );
        assert( isBalanced( "// {" ) );
        assert( ! isBalanced( "// \n {" ) );
        assert( ! isBalanced( "\"//\" {" ) );

    }
} balnaced_test;

string finishCode( string code ){
    while ( ! isBalanced( code ) ){
        inMultiLine = 1;
        code += "\n";
        char * line = shellReadline("... " , 1 );
        if ( gotInterrupted )
            return "";
        if ( ! line )
            return "";
        code += line;
    }
    return code;
}

#include <boost/program_options.hpp>
namespace po = boost::program_options;

void show_help_text(const char* name, po::options_description options) {
    cout << "MongoDB shell version: " << mongo::versionString << endl;
    cout << "usage: " << name << " [options] [db address] [file names (ending in .js)]" << endl
         << "db address can be:" << endl
         << "  foo                   foo database on local machine" << endl
         << "  192.169.0.5/foo       foo database on 192.168.0.5 machine" << endl
         << "  192.169.0.5:9999/foo  foo database on 192.168.0.5 machine on port 9999" << endl
         << options << endl
         << "file names: a list of files to run. files have to end in .js and will exit after "
         << "unless --shell is specified" << endl;
};

bool fileExists( string file ){
    try {
        path p(file);
        return boost::filesystem::exists( file );
    }
    catch (...){
        return false;
    }
}

int _main(int argc, char* argv[]) {
    setupSignals();

    mongo::shellUtils::RecordMyLocation( argv[ 0 ] );

    string url = "test";
    string dbhost;
    string port;
    vector<string> files;

    string username;
    string password;

    bool runShell = false;
    bool nodb = false;
    
    string script;

    po::options_description shell_options("options");
    po::options_description hidden_options("Hidden options");
    po::options_description cmdline_options("Command line options");
    po::positional_options_description positional_options;
    
    shell_options.add_options()
        ("shell", "run the shell after executing files")
        ("nodb", "don't connect to mongod on startup - no 'db address' arg expected")
        ("quiet", "be less chatty" )
        ("port", po::value<string>(&port), "port to connect to")
        ("host", po::value<string>(&dbhost), "server to connect to")
        ("eval", po::value<string>(&script), "evaluate javascript")
        ("username,u", po::value<string>(&username), "username for authentication")
        ("password,p", po::value<string>(&password), "password for authentication")
        ("help,h", "show this usage information")
        ("version", "show version information")
        ;

    hidden_options.add_options()
        ("dbaddress", po::value<string>(), "dbaddress")
        ("files", po::value< vector<string> >(), "files")
        ("nokillop", "nokillop") // for testing, kill op will also be disabled automatically if the tests starts a mongo program
        ;

    positional_options.add("dbaddress", 1);
    positional_options.add("files", -1);

    cmdline_options.add(shell_options).add(hidden_options);

    po::variables_map params;

    /* using the same style as db.cpp uses because eventually we're going
     * to merge some of this stuff. */
    int command_line_style = (((po::command_line_style::unix_style ^
                                po::command_line_style::allow_guessing) |
                               po::command_line_style::allow_long_disguise) ^
                              po::command_line_style::allow_sticky);

    try {
        po::store(po::command_line_parser(argc, argv).options(cmdline_options).
                  positional(positional_options).
                  style(command_line_style).run(), params);
        po::notify(params);
    } catch (po::error &e) {
        cout << "ERROR: " << e.what() << endl << endl;
        show_help_text(argv[0], shell_options);
        return mongo::EXIT_BADOPTIONS;
    }

    if (params.count("shell")) {
        runShell = true;
    }
    if (params.count("nodb")) {
        nodb = true;
    }
    if (params.count("help")) {
        show_help_text(argv[0], shell_options);
        return mongo::EXIT_CLEAN;
    }
    if (params.count("files")) {
        files = params["files"].as< vector<string> >();
    }
    if (params.count("version")) {
        cout << "MongoDB shell version: " << mongo::versionString << endl;
        return mongo::EXIT_CLEAN;
    }
    if (params.count("quiet")) {
        mongo::cmdLine.quiet = true;
    }
    if (params.count("nokillop")) {
        mongo::shellUtils::_nokillop = true;
    }
    
    /* This is a bit confusing, here are the rules:
     *
     * if nodb is set then all positional parameters are files
     * otherwise the first positional parameter might be a dbaddress, but
     * only if one of these conditions is met:
     *   - it contains no '.' after the last appearance of '\' or '/'
     *   - it doesn't end in '.js' and it doesn't specify a path to an existing file */
    if (params.count("dbaddress")) {
        string dbaddress = params["dbaddress"].as<string>();
        if (nodb) {
            files.insert(files.begin(), dbaddress);
        } else {
            string basename = dbaddress.substr(dbaddress.find_last_of("/\\") + 1);
            if (basename.find_first_of('.') == string::npos ||
                (basename.find(".js", basename.size() - 3) == string::npos && !fileExists(dbaddress))) {
                url = dbaddress;
            } else {
                files.insert(files.begin(), dbaddress);
            }
        }
    }
    
    if ( ! mongo::cmdLine.quiet ) 
        cout << "MongoDB shell version: " << mongo::versionString << endl;

    mongo::UnitTest::runTests();

    if ( !nodb ) { // connect to db
        if ( ! mongo::cmdLine.quiet ) cout << "url: " << url << endl;
        
        stringstream ss;
        if ( mongo::cmdLine.quiet )
            ss << "__quiet = true;";
        ss << "db = connect( \"" << fixHost( url , dbhost , port ) << "\")";
        
        mongo::shellUtils::_dbConnect = ss.str();

        if ( username.size() && password.size() ){
            stringstream ss;
            ss << "if ( ! db.auth( \"" << username << "\" , \"" << password << "\" ) ){ throw 'login failed'; }";
            mongo::shellUtils::_dbAuth = ss.str();
        }

    }

    mongo::ScriptEngine::setConnectCallback( mongo::shellUtils::onConnect );
    mongo::ScriptEngine::setup();
    mongo::globalScriptEngine->setScopeInitCallback( mongo::shellUtils::initScope );
    auto_ptr< mongo::Scope > scope( mongo::globalScriptEngine->newScope() );    
    
    if ( !script.empty() ) {
        mongo::shellUtils::MongoProgramScope s;
        if ( ! scope->exec( script , "(shell eval)" , true , true , false ) )
            return -4;
    }

    for (size_t i = 0; i < files.size(); i++) {
        mongo::shellUtils::MongoProgramScope s;

        if ( files.size() > 1 )
            cout << "loading file: " << files[i] << endl;

        if ( ! scope->execFile( files[i] , false , true , false ) ){
            cout << "failed to load: " << files[i] << endl;
            return -3;
        }
    }

    if ( files.size() == 0 && script.empty() ) {
        runShell = true;
    }

    if ( runShell ){

        mongo::shellUtils::MongoProgramScope s;

        shellHistoryInit();

        cout << "type \"help\" for help" << endl;

        //v8::Handle<v8::Object> shellHelper = baseContext_->Global()->Get( v8::String::New( "shellHelper" ) )->ToObject();

        while ( 1 ){
            inMultiLine = 0;
            gotInterrupted = 0;
            char * line = shellReadline( "> " );

            if ( line )
                while ( line[0] == ' ' )
                    line++;

            if ( ! line || ( strlen(line) == 4 && strstr( line , "exit" ) ) ){
                cout << "bye" << endl;
                break;
            }

            string code = line;
            if ( code == "exit" || code == "exit;" ){
                break;
            }
            if ( code.size() == 0 )
                continue;

            code = finishCode( code );
            if ( gotInterrupted ){
                cout << endl;
                continue;
            }

            if ( code.size() == 0 )
                break;

            bool wascmd = false;
            {
                string cmd = line;
                if ( cmd.find( " " ) > 0 )
                    cmd = cmd.substr( 0 , cmd.find( " " ) );

                if ( cmd.find( "\"" ) == string::npos ){
                    try {
                        scope->exec( (string)"__iscmd__ = shellHelper[\"" + cmd + "\"];" , "(shellhelp1)" , false , true , true );
                        if ( scope->getBoolean( "__iscmd__" )  ){
                            scope->exec( (string)"shellHelper( \"" + cmd + "\" , \"" + code.substr( cmd.size() ) + "\");" , "(shellhelp2)" , false , true , false );
                            wascmd = true;
                        }
                    }
                    catch ( std::exception& e ){
                        cout << "error2:" << e.what() << endl;    
                        wascmd = true;
                    }
                }

            }

            if ( ! wascmd ){
                try {
                    scope->exec( code.c_str() , "(shell)" , false , true , false );
                    scope->exec( "shellPrintHelper( __lastres__ );" , "(shell2)" , true , true , false );
                }
                catch ( std::exception& e ){
                    cout << "error:" << e.what() << endl;
                }
            }


            shellHistoryAdd( line );
        }

        shellHistoryDone();
    }

    return 0;
}

int main(int argc, char* argv[]) {
    static mongo::StaticObserver staticObserver;
    try {
        return _main( argc , argv );
    }
    catch ( mongo::DBException& e ){
        cerr << "exception: " << e.what() << endl;
        return -1;
    }
}


