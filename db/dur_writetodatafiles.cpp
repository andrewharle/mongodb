// @file dur_writetodatafiles.cpp apply the writes back to the non-private MMF after they are for certain in redo log

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

#include "pch.h"
#include "dur_commitjob.h"
#include "dur_stats.h"
#include "dur_recover.h"
#include "../util/timer.h"

namespace mongo {
    namespace dur {

        void debugValidateAllMapsMatch();

        /** apply the writes back to the non-private MMF after they are for certain in redo log

            (1) todo we don't need to write back everything every group commit.  we MUST write back
            that which is going to be a remapped on its private view - but that might not be all
            views.

            (2) todo should we do this using N threads?  would be quite easy
                see Hackenberg paper table 5 and 6.  2 threads might be a good balance.

            (3) with enough work, we could do this outside the read lock.  it's a bit tricky though.
                - we couldn't do it from the private views then as they may be changing.  would have to then
                  be from the journal alignedbuffer.
                - we need to be careful the file isn't unmapped on us -- perhaps a mutex or something
                  with MongoMMF on closes or something to coordinate that.

            locking: in read lock when called

            @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc&hl=en
        */

        void WRITETODATAFILES_Impl1() {
            RecoveryJob::get().processSection(commitJob._ab.buf(), commitJob._ab.len());
        }

        // the old implementation
        void WRITETODATAFILES_Impl2() {
            /* we go backwards as what is at the end is most likely in the cpu cache.  it won't be much, but we'll take it. */
            for( set<WriteIntent>::const_iterator it(commitJob.writes().begin()), end(commitJob.writes().end()); it != end; ++it ) {
                const WriteIntent& intent = *it;
                stats.curr->_writeToDataFilesBytes += intent.length();
                dassert(intent.w_ptr);
                memcpy(intent.w_ptr, intent.start(), intent.length());
            }
        }

#if defined(_EXPERIMENTAL)
        void WRITETODATAFILES_Impl3() {
            /* we go backwards as what is at the end is most likely in the cpu cache.  it won't be much, but we'll take it. */
            for( set<WriteIntent>::const_iterator it(commitJob.writes().begin()), end(commitJob.writes().end()); it != end; ++it ) {
                const WriteIntent& intent = *it;
                stats.curr->_writeToDataFilesBytes += intent.length();
                dassert(intent.w_ptr);
                memcpy(intent.w_ptr,
                       commitJob._ab.atOfs(intent.ofsInJournalBuffer),
                       intent.length());
            }
        }
#endif

        void WRITETODATAFILES() {
            dbMutex.assertAtLeastReadLocked();

            MongoFile::markAllWritable(); // for _DEBUG. normally we don't write in a read lock

            Timer t;
#if defined(_EXPERIMENTAL)
            WRITETODATAFILES_Impl3();
#else
            WRITETODATAFILES_Impl1();
#endif
            stats.curr->_writeToDataFilesMicros += t.micros();

            if (!dbMutex.isWriteLocked())
                MongoFile::unmarkAllWritable();

            debugValidateAllMapsMatch();
        }

    }
}
