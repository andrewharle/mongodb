// restore.cpp

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

#include "../stdafx.h"
#include "../client/dbclient.h"
#include "../util/mmap.h"
#include "tool.h"

#include <boost/program_options.hpp>

#include <fcntl.h>

using namespace mongo;

namespace po = boost::program_options;

class Restore : public Tool {
public:
    
    bool _drop;
    bool _objcheck;
    
    Restore() : Tool( "restore" , true , "" , "" ) , _drop(false),_objcheck(false){
        add_options()
            ("drop" , "drop each collection before import" )
            ("objcheck" , "validate object before inserting" )
            ;
        add_hidden_options()
            ("dir", po::value<string>()->default_value("dump"), "directory to restore from")
            ;
        addPositionArg("dir", 1);
    }

    virtual void printExtraHelp(ostream& out) {
        out << "usage: " << _name << " [options] [directory or filename to restore from]" << endl;
    }

    int run(){
        auth();
        path root = getParam("dir");
        _drop = hasParam( "drop" );
        _objcheck = hasParam( "objcheck" );

        /* If _db is not "" then the user specified a db name to restore as.
         *
         * In that case we better be given either a root directory that
         * contains only .bson files or a single .bson file  (a db).
         *
         * In the case where a collection name is specified we better be
         * given either a root directory that contains only a single
         * .bson file, or a single .bson file itself (a collection).
         */
        drillDown(root, _db != "", _coll != "");
        conn().getLastError();
        return EXIT_CLEAN;
    }

    void drillDown( path root, bool use_db = false, bool use_coll = false ) {
        log(2) << "drillDown: " << root.string() << endl;

        if ( is_directory( root ) ) {
            directory_iterator end;
            directory_iterator i(root);
            while ( i != end ) {
                path p = *i;
                i++;

                if (use_db) {
                    if (is_directory(p)) {
                        cerr << "ERROR: root directory must be a dump of a single database" << endl;
                        cerr << "       when specifying a db name with --db" << endl;
                        printHelp(cout);
                        return;
                    }
                }

                if (use_coll) {
                    if (is_directory(p) || i != end) {
                        cerr << "ERROR: root directory must be a dump of a single collection" << endl;
                        cerr << "       when specifying a collection name with --collection" << endl;
                        printHelp(cout);
                        return;
                    }
                }

                drillDown(p, use_db, use_coll);
            }
            return;
        }

        if ( ! ( endsWith( root.string().c_str() , ".bson" ) ||
                 endsWith( root.string().c_str() , ".bin" ) ) ) {
            cerr << "don't know what to do with [" << root.string() << "]" << endl;
            return;
        }

        out() << root.string() << endl;

        string ns;
        if (use_db) {
            ns += _db;
        } else {
            string dir = root.branch_path().string();
            if ( dir.find( "/" ) == string::npos )
                ns += dir;
            else
                ns += dir.substr( dir.find_last_of( "/" ) + 1 );
        }

        if (use_coll) {
            ns += "." + _coll;
        } else {
            string l = root.leaf();
            l = l.substr( 0 , l.find_last_of( "." ) );
            ns += "." + l;
        }

        long long fileLength = file_size( root );

        if ( fileLength == 0 ) {
            out() << "file " + root.native_file_string() + " empty, skipping" << endl;
            return;
        }

        out() << "\t going into namespace [" << ns << "]" << endl;

        if ( _drop ){
            out() << "\t dropping" << endl;
            conn().dropCollection( ns );
        }

        string fileString = root.string();
        ifstream file( fileString.c_str() , ios_base::in | ios_base::binary);
        if ( ! file.is_open() ){
            log() << "error opening file: " << fileString << endl;
            return;
        }

        log(1) << "\t file size: " << fileLength << endl;

        long long read = 0;
        long long num = 0;

        const int BUF_SIZE = 1024 * 1024 * 5;
        boost::scoped_array<char> buf_holder(new char[BUF_SIZE]);
        char * buf = buf_holder.get();

        ProgressMeter m( fileLength );

        while ( read < fileLength ) {
            file.read( buf , 4 );
            int size = ((int*)buf)[0];
            if ( size >= BUF_SIZE ){
                cerr << "got an object of size: " << size << "  terminating..." << endl;
            }
            uassert( 10264 ,  "invalid object size" , size < BUF_SIZE );

            file.read( buf + 4 , size - 4 );

            BSONObj o( buf );
            if ( _objcheck && ! o.valid() ){
                cerr << "INVALID OBJECT - going try and pring out " << endl;
                cerr << "size: " << size << endl;
                BSONObjIterator i(o);
                while ( i.more() ){
                    BSONElement e = i.next();
                    try {
                        e.validate();
                    }
                    catch ( ... ){
                        cerr << "\t\t NEXT ONE IS INVALID" << endl;
                    }
                    cerr << "\t name : " << e.fieldName() << " " << e.type() << endl;
                    cerr << "\t " << e << endl;
                }
            }
            conn().insert( ns.c_str() , o );

            read += o.objsize();
            num++;

            m.hit( o.objsize() );
        }

        uassert( 10265 ,  "counts don't match" , m.done() == fileLength );
        out() << "\t "  << m.hits() << " objects" << endl;
    }
};

int main( int argc , char ** argv ) {
    Restore restore;
    return restore.main( argc , argv );
}
