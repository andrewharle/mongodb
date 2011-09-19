// @file logfile.cpp simple file log writing / journaling

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
#include "logfile.h"
#include "text.h"
#include "mongoutils/str.h"
#include "unittest.h"

using namespace mongoutils;

namespace mongo {
    struct LogfileTest : public UnitTest {
        LogfileTest() { }
        void run() {
            if( 0 && debug ) {
                try {
                    LogFile f("logfile_test");
                    void *p = malloc(16384);
                    char *buf = (char*) p;
                    buf += 4095;
                    buf = (char*) (((size_t)buf)&(~0xfff));
                    memset(buf, 'z', 8192);
                    buf[8190] = '\n';
                    buf[8191] = 'B';
                    buf[0] = 'A';
                    f.synchronousAppend(buf, 8192);
                    f.synchronousAppend(buf, 8192);
                    free(p);
                }
                catch(DBException& e ) {
                    log() << "logfile.cpp test failed : " << e.what() << endl;
                    throw;
                }
            }
        }
    } __test;
}

#if defined(_WIN32)

namespace mongo {

    LogFile::LogFile(string name) : _name(name) {
        _fd = CreateFile(
                  toNativeString(name.c_str()).c_str(),
                  GENERIC_WRITE,
                  FILE_SHARE_READ,
                  NULL,
                  OPEN_ALWAYS,
                  FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                  NULL);
        if( _fd == INVALID_HANDLE_VALUE ) {
            DWORD e = GetLastError();
            uasserted(13518, str::stream() << "couldn't open file " << name << " for writing " << errnoWithDescription(e));
        }
        SetFilePointer(_fd, 0, 0, FILE_BEGIN);
    }

    LogFile::~LogFile() {
        if( _fd != INVALID_HANDLE_VALUE )
            CloseHandle(_fd);
    }

    void LogFile::truncate() {
        verify(15870, _fd != INVALID_HANDLE_VALUE);

        if (!SetEndOfFile(_fd)){
            msgasserted(15871, "Couldn't truncate file: " + errnoWithDescription());
        }
    }

    void LogFile::synchronousAppend(const void *_buf, size_t _len) {
        const size_t BlockSize = 8 * 1024 * 1024;
        assert(_fd);
        assert(_len % 4096 == 0);
        const char *buf = (const char *) _buf;
        size_t left = _len;
        while( left ) {
            size_t toWrite = min(left, BlockSize);
            DWORD written;
            if( !WriteFile(_fd, buf, toWrite, &written, NULL) ) {
                DWORD e = GetLastError();
                if( e == 87 )
                    msgasserted(13519, "error 87 appending to file - invalid parameter");
                else
                    uasserted(13517, str::stream() << "error appending to file " << _name << ' ' << _len << ' ' << toWrite << ' ' << errnoWithDescription(e));
            }
            else {
                dassert( written == toWrite );
            }
            left -= written;
            buf += written;
        }
    }

}

#else

// posix

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "paths.h"

namespace mongo {

    LogFile::LogFile(string name) : _name(name) {
        int options = O_CREAT
                    | O_WRONLY
#if defined(O_DIRECT)
                    | O_DIRECT
#endif
#if defined(O_NOATIME)
                    | O_NOATIME
#endif
                    ;

        _fd = open(name.c_str(), options, S_IRUSR | S_IWUSR);

#if defined(O_DIRECT)
        _direct = true;
        if( _fd < 0 ) {
            _direct = false;
            options &= ~O_DIRECT;
            _fd = open(name.c_str(), options, S_IRUSR | S_IWUSR);
        }
#else
        _direct = false;
#endif

        if( _fd < 0 ) {
            uasserted(13516, str::stream() << "couldn't open file " << name << " for writing " << errnoWithDescription());
        }

        flushMyDirectory(name);
    }

    LogFile::~LogFile() {
        if( _fd >= 0 )
            close(_fd);
        _fd = -1;
    }

    void LogFile::truncate() {
        verify(15872, _fd >= 0);

        BOOST_STATIC_ASSERT(sizeof(off_t) == 8); // we don't want overflow here
        const off_t pos = lseek(_fd, 0, SEEK_CUR); // doesn't actually seek
        if (ftruncate(_fd, pos) != 0){
            msgasserted(15873, "Couldn't truncate file: " + errnoWithDescription());
        }
    }

    void LogFile::synchronousAppend(const void *b, size_t len) {
#ifdef POSIX_FADV_DONTNEED
        const off_t pos = lseek(_fd, 0, SEEK_CUR); // doesn't actually seek
#endif

        const char *buf = (char *) b;
        assert(_fd);
        assert(((size_t)buf)%4096==0); // aligned
        if( len % 4096 != 0 ) {
            log() << len << ' ' << len % 4096 << endl;
            assert(false);
        }
        ssize_t written = write(_fd, buf, len);
        if( written != (ssize_t) len ) {
            log() << "write fails written:" << written << " len:" << len << " buf:" << buf << ' ' << errnoWithDescription() << endl;
            uasserted(13515, str::stream() << "error appending to file " << _fd  << ' ' << errnoWithDescription());
        }

        if( 
#if defined(__linux__)
           fdatasync(_fd) < 0 
#else
           fsync(_fd)
#endif
            ) {
            uasserted(13514, str::stream() << "error appending to file on fsync " << ' ' << errnoWithDescription());
        }

#ifdef POSIX_FADV_DONTNEED
        if (!_direct)
            posix_fadvise(_fd, pos, len, POSIX_FADV_DONTNEED);
#endif
    }

}

#endif
