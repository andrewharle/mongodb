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

#include "mongo/pch.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mongo/platform/atomic_word.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/mmap.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/startup_test.h"

using namespace mongoutils;

namespace {
    mongo::AtomicUInt64 mmfNextId(0);
}

namespace mongo {
    static size_t fetchMinOSPageSizeBytes() {
        size_t minOSPageSizeBytes = sysconf( _SC_PAGESIZE );
        minOSPageSizeBytesTest(minOSPageSizeBytes);
        return minOSPageSizeBytes;
    }
    const size_t g_minOSPageSizeBytes = fetchMinOSPageSizeBytes();
        
    

    MemoryMappedFile::MemoryMappedFile() : _uniqueId(mmfNextId.fetchAndAdd(1)) {
        fd = 0;
        maphandle = 0;
        len = 0;
        created();
    }

    void MemoryMappedFile::close() {
        LockMongoFilesShared::assertExclusivelyLocked();
        for( vector<void*>::iterator i = views.begin(); i != views.end(); i++ ) {
            munmap(*i,len);
        }
        views.clear();

        if ( fd )
            ::close(fd);
        fd = 0;
        destroyed(); // cleans up from the master list of mmaps
    }

#ifndef O_NOATIME
#define O_NOATIME (0)
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE (0)
#endif

    namespace {
        void* _pageAlign( void* p ) {
            return (void*)((int64_t)p & ~(g_minOSPageSizeBytes-1));
        }

        class PageAlignTest : public StartupTest {
        public:
            void run() {
                {
                    int64_t x = g_minOSPageSizeBytes + 123;
                    void* y = _pageAlign( reinterpret_cast<void*>( x ) );
                    invariant( g_minOSPageSizeBytes == reinterpret_cast<size_t>(y) );
                }
                {
                    int64_t a = static_cast<uint64_t>( numeric_limits<int>::max() );
                    a = a / g_minOSPageSizeBytes;
                    a = a * g_minOSPageSizeBytes;
                    // a should now be page aligned

                    // b is not page aligned
                    int64_t b = a + 123;

                    void* y = _pageAlign( reinterpret_cast<void*>( b ) );
                    invariant( a == reinterpret_cast<int64_t>(y) );
                }

            }
        } pageAlignTest;
    }

#if defined(__sunos__)
    MAdvise::MAdvise(void *,unsigned, Advice) { }
    MAdvise::~MAdvise() { }
#else
    MAdvise::MAdvise(void *p, unsigned len, Advice a) {

        _p = _pageAlign( p );

        _len = len + static_cast<unsigned>( reinterpret_cast<size_t>(p) -
                                            reinterpret_cast<size_t>(_p)  );

        int advice = 0;
        switch ( a ) {
        case Sequential:
            advice = MADV_SEQUENTIAL;
            break;
        case Random:
            advice = MADV_RANDOM;
            break;
        }

        if ( madvise(_p,_len,advice ) ) {
            error() << "madvise failed: " << errnoWithDescription();
        }

    }
    MAdvise::~MAdvise() {
        madvise(_p,_len,MADV_NORMAL);
    }
#endif

    void* MemoryMappedFile::map(const char *filename, unsigned long long &length, int options) {
        // length may be updated by callee.
        setFilename(filename);
        FileAllocator::get()->allocateAsap( filename, length );
        len = length;

        massert( 10446 , str::stream() << "mmap: can't map area of size 0 file: " << filename, length > 0 );

        fd = open(filename, O_RDWR | O_NOATIME);
        if ( fd <= 0 ) {
            log() << "couldn't open " << filename << ' ' << errnoWithDescription() << endl;
            fd = 0; // our sentinel for not opened
            return 0;
        }

        unsigned long long filelen = lseek(fd, 0, SEEK_END);
        uassert(10447,  str::stream() << "map file alloc failed, wanted: " << length << " filelen: " << filelen << ' ' << sizeof(size_t), filelen == length );
        lseek( fd, 0, SEEK_SET );

        void * view = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if ( view == MAP_FAILED ) {
            error() << "  mmap() failed for " << filename << " len:" << length << " " << errnoWithDescription() << endl;
            if ( errno == ENOMEM ) {
                if( sizeof(void*) == 4 )
                    error() << "mmap failed with out of memory. You are using a 32-bit build and probably need to upgrade to 64" << endl;
                else
                    error() << "mmap failed with out of memory. (64 bit build)" << endl;
            }
            return 0;
        }


#if defined(__sunos__)
#warning madvise not supported on solaris yet
#else
        if ( options & SEQUENTIAL ) {
            if ( madvise( view , length , MADV_SEQUENTIAL ) ) {
                warning() << "map: madvise failed for " << filename << ' ' << errnoWithDescription() << endl;
            }
        }
#endif

        views.push_back( view );

        return view;
    }

    void* MemoryMappedFile::createReadOnlyMap() {
        void * x = mmap( /*start*/0 , len , PROT_READ , MAP_SHARED , fd , 0 );
        if( x == MAP_FAILED ) {
            if ( errno == ENOMEM ) {
                if( sizeof(void*) == 4 )
                    error() << "mmap ro failed with out of memory. You are using a 32-bit build and probably need to upgrade to 64" << endl;
                else
                    error() << "mmap ro failed with out of memory. (64 bit build)" << endl;
            }
            return 0;
        }
        return x;
    }

    void* MemoryMappedFile::createPrivateMap() {
        void * x = mmap( /*start*/0 , len , PROT_READ|PROT_WRITE , MAP_PRIVATE|MAP_NORESERVE , fd , 0 );
        if( x == MAP_FAILED ) {
            if ( errno == ENOMEM ) {
                if( sizeof(void*) == 4 ) {
                    error() << "mmap private failed with out of memory. You are using a 32-bit build and probably need to upgrade to 64" << endl;
                }
                else {
                    error() << "mmap private failed with out of memory. (64 bit build)" << endl;
                }
            }
            else { 
                error() << "mmap private failed " << errnoWithDescription() << endl;
            }
            return 0;
        }

        views.push_back(x);
        return x;
    }

    void* MemoryMappedFile::remapPrivateView(void *oldPrivateAddr) {
#if defined(__sunos__) // SERVER-8795
        verify( Lock::isW() );
        LockMongoFilesExclusive lockMongoFiles;
#endif

        // don't unmap, just mmap over the old region
        void * x = mmap( oldPrivateAddr, len , PROT_READ|PROT_WRITE , MAP_PRIVATE|MAP_NORESERVE|MAP_FIXED , fd , 0 );
        if( x == MAP_FAILED ) {
            int err = errno;
            error()  << "13601 Couldn't remap private view: " << errnoWithDescription(err) << endl;
            log() << "aborting" << endl;
            printMemInfo();
            abort();
        }
        verify( x == oldPrivateAddr );
        return x;
    }

    void MemoryMappedFile::flush(bool sync) {
        if ( views.empty() || fd == 0 )
            return;
        if ( msync(viewForFlushing(), len, sync ? MS_SYNC : MS_ASYNC) ) {
            // msync failed, this is very bad
            problem() << "msync failed: " << errnoWithDescription();
            dataSyncFailedHandler();
        }
    }

    class PosixFlushable : public MemoryMappedFile::Flushable {
    public:
        PosixFlushable( MemoryMappedFile* theFile, void* view , HANDLE fd , long len)
            : _theFile( theFile ), _view( view ), _fd(fd), _len(len), _id(_theFile->getUniqueId()) {
        }

        void flush() {
            if ( _view == NULL || _fd == 0 )
                return;

            if ( msync(_view, _len, MS_SYNC ) == 0 )
                return;

            if ( errno == EBADF ) {
                // ok, we were unlocked, so this file was closed
                return;
            }

            // some error, lets see if we're supposed to exist
            LockMongoFilesShared mmfilesLock;
            std::set<MongoFile*> mmfs = MongoFile::getAllFiles();
            std::set<MongoFile*>::const_iterator it = mmfs.find(_theFile);
            if ( (it == mmfs.end()) || ((*it)->getUniqueId() != _id) ) {
                log() << "msync failed with: " << errnoWithDescription()
                      << " but file doesn't exist anymore, so ignoring";
                // this was deleted while we were unlocked
                return;
            }

            // we got an error, and we still exist, so this is bad, we fail
            problem() << "msync " << errnoWithDescription() << endl;
            dataSyncFailedHandler();
        }

        MemoryMappedFile* _theFile;
        void * _view;
        HANDLE _fd;
        long _len;
        const uint64_t _id;
    };

    MemoryMappedFile::Flushable * MemoryMappedFile::prepareFlush() {
        return new PosixFlushable( this, viewForFlushing(), fd, len);
    }


} // namespace mongo

