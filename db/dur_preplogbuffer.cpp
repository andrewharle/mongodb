// @file dur_preplogbuffer.cpp

/**
*    Copyright (C) 2009 10gen Inc.
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

/*
     PREPLOGBUFFER
       we will build an output buffer ourself and then use O_DIRECT
       we could be in read lock for this
       for very large objects write directly to redo log in situ?
     @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc
*/

#include "pch.h"
#include "cmdline.h"
#include "dur.h"
#include "dur_journal.h"
#include "dur_journalimpl.h"
#include "dur_commitjob.h"
#include "../util/mongoutils/hash.h"
#include "../util/mongoutils/str.h"
#include "../util/alignedbuilder.h"
#include "../util/timer.h"
#include "dur_stats.h"
#include "../server.h"

using namespace mongoutils;

namespace mongo {
    namespace dur {

        extern Journal j;

        RelativePath local = RelativePath::fromRelativePath("local");

        MongoMMF* findMMF_inlock(void *ptr, size_t &ofs) {
            MongoMMF *f = privateViews.find_inlock(ptr, ofs);
            if( f == 0 ) {
                string s = str::stream() << "view pointer cannot be resolved " << (size_t) ptr;
                journalingFailure(s.c_str()); // asserts
            }
            return f;
        }

        /** put the basic write operation into the buffer (bb) to be journaled */
        void prepBasicWrite_inlock(AlignedBuilder&bb, const WriteIntent *i, RelativePath& lastDbPath) {
            size_t ofs = 1;
            MongoMMF *mmf = findMMF_inlock(i->start(), /*out*/ofs);

            if( unlikely(!mmf->willNeedRemap()) ) {
                // tag this mmf as needed a remap of its private view later.
                // usually it will already be dirty/already set, so we do the if above first
                // to avoid possibility of cpu cache line contention
                mmf->willNeedRemap() = true;
            }

            // since we have already looked up the mmf, we go ahead and remember the write view location
            // so we don't have to find the MongoMMF again later in WRITETODATAFILES()
            // 
            // this was for WRITETODATAFILES_Impl2 so commented out now
            //
            /*
            dassert( i->w_ptr == 0 );
            i->w_ptr = ((char*)mmf->view_write()) + ofs;
            */

            JEntry e;
            e.len = min(i->length(), (unsigned)(mmf->length() - ofs)); //dont write past end of file
            assert( ofs <= 0x80000000 );
            e.ofs = (unsigned) ofs;
            e.setFileNo( mmf->fileSuffixNo() );
            if( mmf->relativePath() == local ) {
                e.setLocalDbContextBit();
            }
            else if( mmf->relativePath() != lastDbPath ) {
                lastDbPath = mmf->relativePath();
                JDbContext c;
                bb.appendStruct(c);
                bb.appendStr(lastDbPath.toString());
            }
            bb.appendStruct(e);
#if defined(_EXPERIMENTAL)
            i->ofsInJournalBuffer = bb.len();
#endif
            bb.appendBuf(i->start(), e.len);

            if (unlikely(e.len != (unsigned)i->length())) {
                log() << "journal info splitting prepBasicWrite at boundary" << endl;

                // This only happens if we write to the last byte in a file and
                // the fist byte in another file that is mapped adjacently. I
                // think most OSs leave at least a one page gap between
                // mappings, but better to be safe.

                WriteIntent next ((char*)i->start() + e.len, i->length() - e.len);
                prepBasicWrite_inlock(bb, &next, lastDbPath);
            }
        }

        /** basic write ops / write intents.  note there is no particular order to these : if we have
            two writes to the same location during the group commit interval, it is likely
            (although not assured) that it is journaled here once.
        */
        void prepBasicWrites(AlignedBuilder& bb) {
            scoped_lock lk(privateViews._mutex());

            // each time events switch to a different database we journal a JDbContext
            RelativePath lastDbPath;

            for( set<WriteIntent>::iterator i = commitJob.writes().begin(); i != commitJob.writes().end(); i++ ) {
                prepBasicWrite_inlock(bb, &(*i), lastDbPath);
            }
        }

        void resetLogBuffer(/*out*/JSectHeader& h, AlignedBuilder& bb) {
            bb.reset();

            h.setSectionLen(0xffffffff);  // total length, will fill in later
            h.seqNumber = getLastDataFileFlushTime();
            h.fileId = j.curFileId();
        }

        /** we will build an output buffer ourself and then use O_DIRECT
            we could be in read lock for this
            caller handles locking
            @return partially populated sectheader and _ab set
        */
        void _PREPLOGBUFFER(JSectHeader& h) {
            assert( cmdLine.dur );

            {
                // now that we are locked, fully drain deferred notes of write intents
                DEV dbMutex.assertAtLeastReadLocked();
                Writes& writes = commitJob.wi();
                writes._deferred.invoke();
                writes._drained = true;
            }

            AlignedBuilder& bb = commitJob._ab;
            resetLogBuffer(h, bb); // adds JSectHeader

            // ops other than basic writes (DurOp's)
            {
                for( vector< shared_ptr<DurOp> >::iterator i = commitJob.ops().begin(); i != commitJob.ops().end(); ++i ) {
                    (*i)->serialize(bb);
                }
            }

            prepBasicWrites(bb);

            return;
        }
        void PREPLOGBUFFER(/*out*/ JSectHeader& h) {
            Timer t;
            j.assureLogFileOpen(); // so fileId is set
            _PREPLOGBUFFER(h);
            stats.curr->_prepLogBufferMicros += t.micros();
        }

    }
}
