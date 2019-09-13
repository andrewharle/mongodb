
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/storage/mmap_v1/file_allocator.h"
#include "mongo/db/storage/mmap_v1/mmap.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/startup_test.h"

using std::endl;
using std::numeric_limits;
using std::vector;

using namespace mongoutils;

namespace mongo {

namespace {
void printMemInfo() {
    LogstreamBuilder out = log();
    out << "mem info: ";

    ProcessInfo pi;
    if (!pi.supported()) {
        out << " not supported";
        return;
    }

    out << "vsize: " << pi.getVirtualMemorySize() << " resident: " << pi.getResidentSize()
        << " mapped: " << MemoryMappedFile::totalMappedLengthInMB();
}
}  // namespace
}  // namespace mongo

std::size_t mongo::getMinOSPageSizeBytes() {
    static const std::size_t cachedSize = [] {
        std::size_t minOSPageSizeBytes = sysconf(_SC_PAGESIZE);
        minOSPageSizeBytesTest(minOSPageSizeBytes);
        return minOSPageSizeBytes;
    }();
    return cachedSize;
}

namespace mongo {

void MemoryMappedFile::close(OperationContext* opCtx) {
    for (vector<void*>::iterator i = views.begin(); i != views.end(); i++) {
        munmap(*i, len);
    }
    views.clear();
    totalMappedLength.fetchAndSubtract(len);
    len = 0;

    if (fd) {
        ::close(fd);
        fd = 0;
    }
    destroyed(opCtx);  // cleans up from the master list of mmaps
}

#ifndef O_NOATIME
#define O_NOATIME (0)
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE (0)
#endif

namespace {
void* _pageAlign(void* p) {
    return (void*)((int64_t)p & ~(getMinOSPageSizeBytes() - 1));
}

class PageAlignTest : public StartupTest {
public:
    void run() {
        {
            int64_t x = getMinOSPageSizeBytes() + 123;
            void* y = _pageAlign(reinterpret_cast<void*>(x));
            invariant(getMinOSPageSizeBytes() == reinterpret_cast<size_t>(y));
        }
        {
            int64_t a = static_cast<uint64_t>(numeric_limits<int>::max());
            a = a / getMinOSPageSizeBytes();
            a = a * getMinOSPageSizeBytes();
            // a should now be page aligned

            // b is not page aligned
            int64_t b = a + 123;

            void* y = _pageAlign(reinterpret_cast<void*>(b));
            invariant(a == reinterpret_cast<int64_t>(y));
        }
    }
} pageAlignTest;
}

#if defined(__sun)
MAdvise::MAdvise(void*, unsigned, Advice) {}
MAdvise::~MAdvise() {}
#else
MAdvise::MAdvise(void* p, unsigned len, Advice a) {
    _p = _pageAlign(p);

    _len = len + static_cast<unsigned>(reinterpret_cast<size_t>(p) - reinterpret_cast<size_t>(_p));

    int advice = 0;
    switch (a) {
        case Sequential:
            advice = MADV_SEQUENTIAL;
            break;
        case Random:
            advice = MADV_RANDOM;
            break;
    }

    if (madvise(_p, _len, advice)) {
        error() << "madvise failed: " << errnoWithDescription();
    }
}
MAdvise::~MAdvise() {
    madvise(_p, _len, MADV_NORMAL);
}
#endif

void* MemoryMappedFile::map(OperationContext* opCtx,
                            const char* filename,
                            unsigned long long& length) {
    // length may be updated by callee.
    setFilename(opCtx, filename);
    FileAllocator::get()->allocateAsap(filename, length);

    const bool readOnly = isOptionSet(READONLY);

    massert(
        10446, str::stream() << "mmap: can't map area of size 0 file: " << filename, length > 0);

    const int posixOpenOpts = O_NOATIME | (readOnly ? O_RDONLY : O_RDWR);
    fd = ::open(filename, posixOpenOpts);
    if (fd <= 0) {
        severe() << "couldn't open " << filename << ' ' << errnoWithDescription() << endl;
        fd = 0;  // our sentinel for not opened
        return 0;
    }

    unsigned long long filelen = lseek(fd, 0, SEEK_END);
    if (filelen != length) {
        severe() << "map file alloc failed, wanted: " << length << " filelen: " << filelen << ' '
                 << sizeof(size_t);
        fassertFailed(16330);
    }
    lseek(fd, 0, SEEK_SET);

    const int mmapProtectionOpts = readOnly ? PROT_READ : (PROT_READ | PROT_WRITE);
    void* view = mmap(NULL, length, mmapProtectionOpts, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        severe() << "  mmap() failed for " << filename << " len:" << length << " "
                 << errnoWithDescription() << endl;
        if (errno == ENOMEM) {
            if (sizeof(void*) == 4)
                severe() << "mmap failed with out of memory. You are using a 32-bit build and "
                            "probably need to upgrade to 64"
                         << endl;
            else
                severe() << "mmap failed with out of memory. (64 bit build)" << endl;
        }
        return 0;
    }


#if !defined(__sun)
    if (isOptionSet(SEQUENTIAL)) {
        if (madvise(view, length, MADV_SEQUENTIAL)) {
            warning() << "map: madvise failed for " << filename << ' ' << errnoWithDescription()
                      << endl;
        }
    }
#endif

    // MemoryMappedFile successfully created, now update state.
    len = length;
    MemoryMappedFile::totalMappedLength.fetchAndAdd(len);

    views.push_back(view);

    return view;
}

void* MemoryMappedFile::createPrivateMap() {
    void* x = mmap(/*start*/ 0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
    if (x == MAP_FAILED) {
        if (errno == ENOMEM) {
            if (sizeof(void*) == 4) {
                severe() << "mmap private failed with out of memory. You are using a 32-bit build "
                            "and probably need to upgrade to 64"
                         << endl;
            } else {
                severe() << "mmap private failed with out of memory. (64 bit build)" << endl;
            }
        } else {
            severe() << "mmap private failed " << errnoWithDescription() << endl;
        }
        return 0;
    }

    views.push_back(x);
    return x;
}

void* MemoryMappedFile::remapPrivateView(OperationContext* opCtx, void* oldPrivateAddr) {
#if defined(__sun)  // SERVER-8795
    LockMongoFilesExclusive lockMongoFiles(opCtx);
#endif

    // don't unmap, just mmap over the old region
    void* x = mmap(oldPrivateAddr,
                   len,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_NORESERVE | MAP_FIXED,
                   fd,
                   0);
    if (x == MAP_FAILED) {
        int err = errno;
        severe() << "13601 Couldn't remap private view: " << errnoWithDescription(err) << endl;
        printMemInfo();
        abort();
    }
    verify(x == oldPrivateAddr);
    return x;
}

void MemoryMappedFile::flush(bool sync) {
    if (views.empty() || fd == 0 || !sync)
        return;

    bool useFsync = !ProcessInfo::preferMsyncOverFSync();

    if (useFsync ? fsync(fd) != 0 : msync(viewForFlushing(), len, MS_SYNC) != 0) {
        // msync failed, this is very bad
        log() << (useFsync ? "fsync failed: " : "msync failed: ") << errnoWithDescription()
              << " file: " << filename() << endl;
        dataSyncFailedHandler();
    }
}

bool MemoryMappedFile::isClosed() {
    return !len && !fd && !views.size();
}

class PosixFlushable : public MemoryMappedFile::Flushable {
public:
    PosixFlushable(MemoryMappedFile* theFile, void* view, HANDLE fd, long len)
        : _theFile(theFile), _view(view), _fd(fd), _len(len), _id(_theFile->getUniqueId()) {}

    void flush(OperationContext* opCtx) {
        if (_view == NULL || _fd == 0)
            return;

        if (ProcessInfo::preferMsyncOverFSync() ? msync(_view, _len, MS_SYNC) == 0
                                                : fsync(_fd) == 0) {
            return;
        }

        if (errno == EBADF) {
            // ok, we were unlocked, so this file was closed
            return;
        }

        // some error, lets see if we're supposed to exist
        LockMongoFilesShared mmfilesLock(opCtx);
        std::set<MongoFile*> mmfs = MongoFile::getAllFiles();
        std::set<MongoFile*>::const_iterator it = mmfs.find(_theFile);
        if ((it == mmfs.end()) || ((*it)->getUniqueId() != _id)) {
            log() << "msync failed with: " << errnoWithDescription()
                  << " but file doesn't exist anymore, so ignoring";
            // this was deleted while we were unlocked
            return;
        }

        // we got an error, and we still exist, so this is bad, we fail
        log() << "msync " << errnoWithDescription() << endl;
        dataSyncFailedHandler();
    }

    MemoryMappedFile* _theFile;
    void* _view;
    HANDLE _fd;
    long _len;
    const uint64_t _id;
};

MemoryMappedFile::Flushable* MemoryMappedFile::prepareFlush() {
    return new PosixFlushable(this, viewForFlushing(), fd, len);
}

}  // namespace mongo
