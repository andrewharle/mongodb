// mmap_posix.cpp

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
#include "mmap.h"
#include "file_allocator.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace mongo {

    MemoryMappedFile::MemoryMappedFile() {
        fd = 0;
        maphandle = 0;
        view = 0;
        len = 0;
        created();
    }

    void MemoryMappedFile::close() {
        if ( view )
            munmap(view, len);
        view = 0;

        if ( fd )
            ::close(fd);
        fd = 0;
    }

#ifndef O_NOATIME
#define O_NOATIME 0
#endif

    void* MemoryMappedFile::map(const char *filename, long &length, int options) {
        // length may be updated by callee.
        theFileAllocator().allocateAsap( filename, length );
        len = length;
        
        fd = open(filename, O_RDWR | O_NOATIME);
        if ( fd <= 0 ) {
            out() << "couldn't open " << filename << ' ' << errno << endl;
            return 0;
        }

        off_t filelen = lseek(fd, 0, SEEK_END);
        if ( filelen != length ){
            cout << "wanted length: " << length << " filelen: " << filelen << endl;
            cout << sizeof(size_t) << endl;
            massert( "file size allocation failed", filelen == length );
        }
        lseek( fd, 0, SEEK_SET );
        
        view = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if ( view == MAP_FAILED ) {
            out() << "  mmap() failed for " << filename << " len:" << length << " errno:" << errno << endl;
            if ( errno == ENOMEM ){
                out() << "     mmap failed with out of memory, if you're using 32-bits, then you probably need to upgrade to 64" << endl;
            }
            return 0;
        }
        
#if defined(__sunos__)
#warning madvise not supported on solaris yet
#else

        if ( options & SEQUENTIAL ){
            if ( madvise( view , length , MADV_SEQUENTIAL ) ){
                out() << " madvise failed for " << filename << " " << errno << endl;
            }
        }
#endif        
        return view;
    }
    
    void MemoryMappedFile::flush(bool sync) {
        if ( view == 0 || fd == 0 )
            return;
        if ( msync(view, len, sync ? MS_SYNC : MS_ASYNC) )
            problem() << "msync error " << errno << endl;
    }
    

} // namespace mongo

