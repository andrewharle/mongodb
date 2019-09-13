
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/data_file_sync.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/mmap.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;

DataFileSync dataFileSync;

DataFileSync::DataFileSync()
    : ServerStatusSection("backgroundFlushing"), _total_time(0), _flushes(0), _last() {}

void DataFileSync::run() {
    Client::initThread(name().c_str());

    if (storageGlobalParams.syncdelay == 0) {
        log() << "warning: --syncdelay 0 is not recommended and can have strange performance"
              << endl;
    } else if (storageGlobalParams.syncdelay == 1) {
        log() << "--syncdelay 1" << endl;
    } else if (storageGlobalParams.syncdelay != 60) {
        LOG(1) << "--syncdelay " << storageGlobalParams.syncdelay.load() << endl;
    }
    int time_flushing = 0;
    while (!globalInShutdownDeprecated()) {
        if (storageGlobalParams.syncdelay == 0) {
            // in case at some point we add an option to change at runtime
            sleepsecs(5);
            continue;
        }

        sleepmillis(
            (long long)std::max(0.0, (storageGlobalParams.syncdelay * 1000) - time_flushing));

        if (globalInShutdownDeprecated()) {
            // occasional issue trying to flush during shutdown when sleep interrupted
            break;
        }

        auto opCtx = cc().makeOperationContext();
        Date_t start = jsTime();
        StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();

        dur::notifyPreDataFileFlush();
        int numFiles = storageEngine->flushAllFiles(opCtx.get(), true);
        dur::notifyPostDataFileFlush();

        time_flushing = durationCount<Milliseconds>(jsTime() - start);

        _flushed(time_flushing);

        if (shouldLog(logger::LogSeverity::Debug(1)) || time_flushing >= 10000) {
            log() << "flushing mmaps took " << time_flushing << "ms "
                  << " for " << numFiles << " files" << endl;
        }
    }
}

BSONObj DataFileSync::generateSection(OperationContext* opCtx,
                                      const BSONElement& configElement) const {
    if (!running()) {
        return BSONObj();
    }

    BSONObjBuilder b;
    b.appendNumber("flushes", _flushes);
    b.appendNumber("total_ms", _total_time);
    b.appendNumber("average_ms", (_flushes ? (_total_time / double(_flushes)) : 0.0));
    b.appendNumber("last_ms", _last_time);
    b.append("last_finished", _last);
    return b.obj();
}

void DataFileSync::_flushed(int ms) {
    _flushes++;
    _total_time += ms;
    _last_time = ms;
    _last = jsTime();
}


class MemJournalServerStatusMetric : public ServerStatusMetric {
public:
    MemJournalServerStatusMetric() : ServerStatusMetric(".mem.mapped") {}
    virtual void appendAtLeaf(BSONObjBuilder& b) const {
        int m = MemoryMappedFile::totalMappedLengthInMB();
        b.appendNumber("mapped", m);

        if (storageGlobalParams.dur) {
            m *= 2;
            b.appendNumber("mappedWithJournal", m);
        }
    }
} memJournalServerStatusMetric;
}
