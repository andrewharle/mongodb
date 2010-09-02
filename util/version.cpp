#include "pch.h"

#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>

#include "version.h"

namespace mongo {

    //
    // mongo processes version support
    //

    const char versionString[] = "1.6.2";

    string mongodVersion() {
        stringstream ss;
        ss << "db version v" << versionString << ", pdfile version " << VERSION << "." << VERSION_MINOR;
        return ss.str();
    }

    //
    // git version support
    //

#ifndef _SCONS
    // only works in scons
    const char * gitVersion(){ return "not-scons"; }
#endif

    void printGitVersion() { log() << "git version: " << gitVersion() << endl; }

    //
    // sys info support
    //

#ifndef _SCONS
#if defined(_WIN32)
    string sysInfo(){ 
        stringstream ss;
        ss << "not-scons win";
        ss << " mscver:" << _MSC_FULL_VER << " built:" << __DATE__;
        ss << " boostver:" << BOOST_VERSION;
#if( !defined(_MT) )
#error _MT is not defined
#endif
        ss << (sizeof(char *) == 8) ? " 64bit" : " 32bit";
        return ss.str();
    }
#else
    string sysInfo(){ return ""; }
#endif
#endif

    void printSysInfo() { log() << "sys info: " << sysInfo() << endl; }

    //
    // 32 bit systems warning
    //

    void show_32_warning(){
        bool warned = false;
        {
            const char * foo = strchr( versionString , '.' ) + 1;
            int bar = atoi( foo );
            if ( ( 2 * ( bar / 2 ) ) != bar ) {
                cout << "\n** NOTE: This is a development version (" << versionString << ") of MongoDB.";
                cout << "\n**       Not recommended for production. \n" << endl;
                warned = true;
            }
        }

        if ( sizeof(int*) != 4 )
            return;

        if( !warned ) // prettier this way 
            cout << endl;
        cout << "** NOTE: when using MongoDB 32 bit, you are limited to about 2 gigabytes of data" << endl;
        cout << "**       see http://blog.mongodb.org/post/137788967/32-bit-limitations" << endl;
        cout << endl;
    }

}
