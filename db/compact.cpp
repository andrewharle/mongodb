/** @file compact.cpp
   compaction of deleted space in pdfiles (datafiles)
*/

/* NOTE 6Oct2010 : this file PRELIMINARY, EXPERIMENTAL, NOT DONE, NOT USED YET (not in SConstruct) */

/**
*    Copyright (C) 2010 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "pdfile.h"
#include "concurrency.h"
#include "commands.h"
#include "curop-inl.h"
#include "background.h"
#include "extsort.h"
#include "compact.h"
#include "../util/concurrency/task.h"

namespace mongo {

    char faux;

    void addRecordToRecListInExtent(Record *r, DiskLoc loc);
    DiskLoc allocateSpaceForANewRecord(const char *ns, NamespaceDetails *d, int lenWHdr, bool god);
    void freeExtents(DiskLoc firstExt, DiskLoc lastExt);

    /** @return number of skipped (invalid) documents */
    unsigned compactExtent(const char *ns, NamespaceDetails *d, const DiskLoc ext, int n,
                const scoped_array<IndexSpec> &indexSpecs,
                scoped_array<SortPhaseOne>& phase1, int nidx, bool validate)
    {
        log() << "compact extent #" << n << endl;

        Extent *e = ext.ext();
        e->assertOk();
        assert( e->validates() );
        unsigned skipped = 0;

        {
            // the next/prev pointers within the extent might not be in order so we first page the whole thing in 
            // sequentially
            log() << "compact paging in len=" << e->length/1000000.0 << "MB" << endl;
            Timer t;
            MAdvise adv(e, e->length, MAdvise::Sequential);
            const char *p = (const char *) e;
            for( int i = 0; i < e->length; i += 4096 ) { 
                faux += *p;
            }
            int ms = t.millis();
            if( ms > 1000 ) 
                log() << "compact end paging in " << ms << "ms " << e->length/1000000.0/ms << "MB/sec" << endl;
        }

        {
            log() << "compact copying records" << endl;
            unsigned totalSize = 0;
            int nrecs = 0;
            DiskLoc L = e->firstRecord;
            if( !L.isNull() )
            while( 1 ) {
                Record *recOld = L.rec();
                L = recOld->nextInExtent(L);
                nrecs++;
                BSONObj objOld(recOld);

                if( !validate || objOld.valid() ) {
                    unsigned sz = objOld.objsize();
                    unsigned lenWHdr = sz + Record::HeaderSize;
                    totalSize += lenWHdr;
                    DiskLoc extentLoc;
                    DiskLoc loc = allocateSpaceForANewRecord(ns, d, lenWHdr, false);
                    uassert(14024, "compact error out of space during compaction", !loc.isNull());
                    Record *recNew = loc.rec();
                    recNew = (Record *) getDur().writingPtr(recNew, lenWHdr);
                    addRecordToRecListInExtent(recNew, loc);
                    memcpy(recNew->data, objOld.objdata(), sz);

                    {
                        // extract keys for all indexes we will be rebuilding
                        for( int x = 0; x < nidx; x++ ) { 
                            phase1[x].addKeys(indexSpecs[x], objOld, loc);
                        }
                    }
                }
                else { 
                    if( ++skipped <= 10 )
                        log() << "compact skipping invalid object" << endl;
                }

                if( L.isNull() ) { 
                    // we just did the very last record from the old extent.  it's still pointed to 
                    // by the old extent ext, but that will be fixed below after this loop
                    break;
                }

                // remove the old records (orphan them) periodically so our commit block doesn't get too large
                bool stopping = false;
                RARELY stopping = *killCurrentOp.checkForInterruptNoAssert(false) != 0;
                if( stopping || getDur().aCommitIsNeeded() ) {
                    e->firstRecord.writing() = L;
                    Record *r = L.rec();
                    getDur().writingInt(r->prevOfs) = DiskLoc::NullOfs;
                    getDur().commitIfNeeded();
                    killCurrentOp.checkForInterrupt(false);
                }
            }

            assert( d->firstExtent == ext );
            assert( d->lastExtent != ext );
            DiskLoc newFirst = e->xnext;
            d->firstExtent.writing() = newFirst;
            newFirst.ext()->xprev.writing().Null();
            getDur().writing(e)->markEmpty();
            freeExtents(ext,ext);
            getDur().commitIfNeeded();

            log() << "compact " << nrecs << " documents " << totalSize/1000000.0 << "MB" << endl;
        }

        return skipped;
    }

    extern SortPhaseOne *precalced;

    bool _compact(const char *ns, NamespaceDetails *d, string& errmsg, bool validate, BSONObjBuilder& result) { 
        //int les = d->lastExtentSize;

        // this is a big job, so might as well make things tidy before we start just to be nice.
        getDur().commitNow();

        list<DiskLoc> extents;
        for( DiskLoc L = d->firstExtent; !L.isNull(); L = L.ext()->xnext ) 
            extents.push_back(L);
        log() << "compact " << extents.size() << " extents" << endl;

        ProgressMeterHolder pm( cc().curop()->setMessage( "compact extent" , extents.size() ) );

        // same data, but might perform a little different after compact?
        NamespaceDetailsTransient::get_w(ns).clearQueryCache();

        int nidx = d->nIndexes;
        scoped_array<IndexSpec> indexSpecs( new IndexSpec[nidx] );
        scoped_array<SortPhaseOne> phase1( new SortPhaseOne[nidx] );
        {
            NamespaceDetails::IndexIterator ii = d->ii(); 
            int x = 0;
            while( ii.more() ) { 
                BSONObjBuilder b;
                IndexDetails& idx = ii.next();
                BSONObj::iterator i(idx.info.obj());
                while( i.more() ) { 
                    BSONElement e = i.next();
                    if( !str::equals(e.fieldName(), "v") && !str::equals(e.fieldName(), "background") ) {
                        b.append(e);
                    }
                }
                BSONObj o = b.obj().getOwned();
                phase1[x].sorter.reset( new BSONObjExternalSorter( idx.idxInterface(), o.getObjectField("key") ) );
                phase1[x].sorter->hintNumObjects( d->stats.nrecords );
                indexSpecs[x++].reset(o);
            }
        }

        log() << "compact orphan deleted lists" << endl;
        for( int i = 0; i < Buckets; i++ ) { 
            d->deletedList[i].writing().Null();
        }

        // before dropping indexes, at least make sure we can allocate one extent!
        uassert(14025, "compact error no space available to allocate", !allocateSpaceForANewRecord(ns, d, Record::HeaderSize+1, false).isNull());

        // note that the drop indexes call also invalidates all clientcursors for the namespace, which is important and wanted here
        log() << "compact dropping indexes" << endl;
        BSONObjBuilder b;
        if( !dropIndexes(d, ns, "*", errmsg, b, true) ) { 
            errmsg = "compact drop indexes failed";
            log() << errmsg << endl;
            return false;
        }

        getDur().commitNow();

        long long skipped = 0;
        int n = 0;
        for( list<DiskLoc>::iterator i = extents.begin(); i != extents.end(); i++ ) { 
            skipped += compactExtent(ns, d, *i, n++, indexSpecs, phase1, nidx, validate);
            pm.hit();
        }

        if( skipped ) {
            result.append("invalidObjects", skipped);
        }

        assert( d->firstExtent.ext()->xprev.isNull() );

        // indexes will do their own progress meter?
        pm.finished();

        // build indexes
        NamespaceString s(ns);
        string si = s.db + ".system.indexes";
        for( int i = 0; i < nidx; i++ ) {
            killCurrentOp.checkForInterrupt(false);
            BSONObj info = indexSpecs[i].info;
            log() << "compact create index " << info["key"].Obj().toString() << endl;
            try {
                precalced = &phase1[i];
                theDataFileMgr.insert(si.c_str(), info.objdata(), info.objsize());
            }
            catch(...) { 
                precalced = 0;
                throw;
            }
            precalced = 0;
        }

        return true;
    }

    bool compact(const string& ns, string &errmsg, bool validate, BSONObjBuilder& result) {
        massert( 14028, "bad ns", NamespaceString::normal(ns.c_str()) );
        massert( 14027, "can't compact a system namespace", !str::contains(ns, ".system.") ); // items in system.indexes cannot be moved there are pointers to those disklocs in NamespaceDetails

        bool ok;
        {
            writelock lk;
            BackgroundOperation::assertNoBgOpInProgForNs(ns.c_str());
            Client::Context ctx(ns);
            NamespaceDetails *d = nsdetails(ns.c_str());
            massert( 13660, str::stream() << "namespace " << ns << " does not exist", d );
            massert( 13661, "cannot compact capped collection", !d->capped );
            log() << "compact " << ns << " begin" << endl;
            try { 
                ok = _compact(ns.c_str(), d, errmsg, validate, result);
            }
            catch(...) { 
                log() << "compact " << ns << " end (with error)" << endl;
                throw;
            }
            log() << "compact " << ns << " end" << endl;
        }
        return ok;
    }

    bool isCurrentlyAReplSetPrimary();

    class CompactCmd : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool maintenanceMode() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual void help( stringstream& help ) const {
            help << "compact collection\n"
                "warning: this operation blocks the server and is slow. you can cancel with cancelOp()\n"
                "{ compact : <collection_name>, [force:true], [validate:true] }\n"
                "  force - allows to run on a replica set primary\n"
                "  validate - check records are noncorrupt before adding to newly compacting extents. slower but safer (default is true in this version)\n";
        }
        virtual bool requiresAuth() { return true; }
        CompactCmd() : Command("compact") { }

        virtual bool run(const string& db, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string coll = cmdObj.firstElement().valuestr();
            if( coll.empty() || db.empty() ) {
                errmsg = "no collection name specified";
                return false;
            }

            if( isCurrentlyAReplSetPrimary() && !cmdObj["force"].trueValue() ) { 
                errmsg = "will not run compact on an active replica set primary as this is a slow blocking operation. use force:true to force";
                return false;
            }

            string ns = db + '.' + coll;
            bool validate = !cmdObj.hasElement("validate") || cmdObj["validate"].trueValue(); // default is true at the moment
            bool ok = compact(ns, errmsg, validate, result);
            return ok;
        }
    };
    static CompactCmd compactCmd;

}
