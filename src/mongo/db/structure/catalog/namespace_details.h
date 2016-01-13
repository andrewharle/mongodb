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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/pch.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/structure/catalog/index_details.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/storage/durable_mapped_file.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/structure/catalog/namespace.h"
#include "mongo/db/structure/catalog/namespace_index.h"
#include "mongo/db/update_index_data.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    class Collection;
    class IndexCatalogEntry;
    class Database;
    class IndexCatalog;

    /** @return true if a client can modify this namespace even though it is under ".system."
        For example <dbname>.system.users is ok for regular clients to update.
        @param write used when .system.js
    */
    bool legalClientSystemNS( const StringData& ns , bool write );

    /* deleted lists -- linked lists of deleted records -- are placed in 'buckets' of various sizes
       so you can look for a deleterecord about the right size.
    */
    const int Buckets = 19;
    const int MaxBucket = 18;

    extern int bucketSizes[];

#pragma pack(1)
    /* NamespaceDetails : this is the "header" for a collection that has all its details.
       It's in the .ns file and this is a memory mapped region (thus the pack pragma above).
    */
    class NamespaceDetails {
    public:
        enum { NIndexesMax = 64, NIndexesExtra = 30, NIndexesBase  = 10 };

    private:

        /*-------- data fields, as present on disk : */

        DiskLoc _firstExtent;
        DiskLoc _lastExtent;

        /* NOTE: capped collections v1 override the meaning of deletedList.
                 deletedList[0] points to a list of free records (DeletedRecord's) for all extents in
                 the capped namespace.
                 deletedList[1] points to the last record in the prev extent.  When the "current extent"
                 changes, this value is updated.  !deletedList[1].isValid() when this value is not
                 yet computed.
        */
        DiskLoc _deletedList[Buckets];

        // ofs 168 (8 byte aligned)
        struct Stats {
            // datasize and nrecords MUST Be adjacent code assumes!
            long long datasize; // this includes padding, but not record headers
            long long nrecords;
        } _stats;

        int _lastExtentSize;
        int _nIndexes;

        // ofs 192
        IndexDetails _indexes[NIndexesBase];

        // ofs 352 (16 byte aligned)
        int _isCapped;                         // there is wasted space here if I'm right (ERH)
        int _maxDocsInCapped;                  // max # of objects for a capped table, -1 for inf.

        double _paddingFactor;                 // 1.0 = no padding.
        // ofs 368 (16)
        int _systemFlags; // things that the system sets/cares about

        DiskLoc _capExtent; // the "current" extent we're writing too for a capped collection
        DiskLoc _capFirstNewRecord;

        unsigned short _dataFileVersion;       // NamespaceDetails version.  So we can do backward compatibility in the future. See filever.h
        unsigned short _indexFileVersion;
        unsigned long long _multiKeyIndexBits;

        // ofs 400 (16)
        unsigned long long _reservedA;
        long long _extraOffset;               // where the $extra info is located (bytes relative to this)

        int _indexBuildsInProgress;            // Number of indexes currently being built

        int _userFlags;
        char _reserved[72];
        /*-------- end data 496 bytes */
    public:
        explicit NamespaceDetails( const DiskLoc &loc, bool _capped );

        class Extra {
            long long _next;
        public:
            IndexDetails details[NIndexesExtra];
        private:
            unsigned reserved2;
            unsigned reserved3;
            Extra(const Extra&) { verify(false); }
            Extra& operator=(const Extra& r) { verify(false); return *this; }
        public:
            Extra() { }
            long ofsFrom(NamespaceDetails *d) {
                return ((char *) this) - ((char *) d);
            }
            void init() { memset(this, 0, sizeof(Extra)); }
            Extra* next(NamespaceDetails *d) {
                if( _next == 0 ) return 0;
                return (Extra*) (((char *) d) + _next);
            }
            void setNext(long ofs) { *getDur().writing(&_next) = ofs;  }
            void copy(NamespaceDetails *d, const Extra& e) {
                memcpy(this, &e, sizeof(Extra));
                _next = 0;
            }
        };
        Extra* extra() {
            if( _extraOffset == 0 ) return 0;
            return (Extra *) (((char *) this) + _extraOffset);
        }
        /* add extra space for indexes when more than 10 */
        Extra* allocExtra(const char *ns, int nindexessofar);
        void copyingFrom(const char *thisns, NamespaceDetails *src); // must be called when renaming a NS to fix up extra

    public:
        const DiskLoc& capExtent() const { return _capExtent; }
        const DiskLoc capFirstNewRecord() const { return _capFirstNewRecord; }

        DiskLoc& capExtent() { return _capExtent; }
        DiskLoc& capFirstNewRecord() { return _capFirstNewRecord; }

    private:
        Extent *theCapExtent() const { return _capExtent.ext(); }
        void advanceCapExtent( const StringData& ns );
        DiskLoc __capAlloc(int len);
        DiskLoc cappedAlloc(Collection* collection, const StringData& ns, int len);
        DiskLoc &cappedFirstDeletedInCurExtent();
        bool nextIsInCapExtent( const DiskLoc &dl ) const;

    public:

        const DiskLoc& firstExtent() const { return _firstExtent; }
        const DiskLoc& lastExtent() const { return _lastExtent; }

        DiskLoc& firstExtent() { return _firstExtent; }
        DiskLoc& lastExtent() { return _lastExtent; }

        void setFirstExtent( DiskLoc newFirstExtent );
        void setLastExtent( DiskLoc newLastExtent );

        void setFirstExtentInvalid();
        void setLastExtentInvalid();


        long long dataSize() const { return _stats.datasize; }
        long long numRecords() const { return _stats.nrecords; }

        void incrementStats( long long dataSizeIncrement,
                             long long numRecordsIncrement );

        void setStats( long long dataSizeIncrement,
                       long long numRecordsIncrement );


        bool isCapped() const { return _isCapped; }
        long long maxCappedDocs() const;
        void setMaxCappedDocs( long long max );

        int lastExtentSize() const { return _lastExtentSize; }
        void setLastExtentSize( int newMax );

        const DiskLoc& deletedListEntry( int bucket ) const { return _deletedList[bucket]; }
        DiskLoc& deletedListEntry( int bucket ) { return _deletedList[bucket]; }

        void orphanDeletedList();

        /**
         * @param max in and out, will be adjusted
         * @return if the value is valid at all
         */
        static bool validMaxCappedDocs( long long* max );

        DiskLoc& cappedListOfAllDeletedRecords() { return _deletedList[0]; }
        DiskLoc& cappedLastDelRecLastExtent()    { return _deletedList[1]; }
        bool capLooped() const { return _isCapped && _capFirstNewRecord.isValid();  }
        bool inCapExtent( const DiskLoc &dl ) const;
        void cappedCheckMigrate();
        /**
         * Truncate documents newer than the document at 'end' from the capped
         * collection.  The collection cannot be completely emptied using this
         * function.  An assertion will be thrown if that is attempted.
         * @param inclusive - Truncate 'end' as well iff true
         */
        void cappedTruncateAfter(const char *ns, DiskLoc end, bool inclusive);
        /** Remove all documents from the capped collection */
        void emptyCappedCollection(const char *ns);

        /* when a background index build is in progress, we don't count the index in nIndexes until
           complete, yet need to still use it in _indexRecord() - thus we use this function for that.
        */
        int getTotalIndexCount() const { return _nIndexes + _indexBuildsInProgress; }

        int getCompletedIndexCount() const { return _nIndexes; }

        int getIndexBuildsInProgress() const { return _indexBuildsInProgress; }

        /* NOTE: be careful with flags.  are we manipulating them in read locks?  if so,
                 this isn't thread safe.  TODO
        */
        enum SystemFlags {
            Flag_HaveIdIndex = 1 << 0 // set when we have _id index (ONLY if ensureIdIndex was called -- 0 if that has never been called)
        };

        enum UserFlags {
            Flag_UsePowerOf2Sizes = 1 << 0
        };

        IndexDetails& idx(int idxNo, bool missingExpected = false );

        class IndexIterator {
        public:
            int pos() { return i; } // note this is the next one to come
            bool more() { return i < n; }
            IndexDetails& next() { return d->idx(i++); }
        private:
            friend class NamespaceDetails;
            int i, n;
            NamespaceDetails *d;
            IndexIterator(NamespaceDetails *_d, bool includeBackgroundInProgress);
        };

        IndexIterator ii( bool includeBackgroundInProgress = false ) {
            return IndexIterator(this, includeBackgroundInProgress);
        }

        /* multikey indexes are indexes where there are more than one key in the index
             for a single document. see multikey in docs.
           for these, we have to do some dedup work on queries.
        */
        bool isMultikey(int i) const { return (_multiKeyIndexBits & (((unsigned long long) 1) << i)) != 0; }

        /**
         * @return - if any state was changed
         */
        bool setIndexIsMultikey(int i, bool multikey = true);

        /**
         * This fetches the IndexDetails for the next empty index slot. The caller must populate
         * returned object.  This handles allocating extra index space, if necessary.
         */
        IndexDetails& getNextIndexDetails(const char* thisns);

        /**
         * @return the actual size to create
         *         will be >= oldRecordSize
         *         based on padding and any other flags
         */
        int getRecordAllocationSize( int minRecordSize );

        double paddingFactor() const { return _paddingFactor; }

        void setPaddingFactor( double paddingFactor );

        /* called to indicate that an update fit in place.  
           fits also called on an insert -- idea there is that if you had some mix and then went to
           pure inserts it would adapt and PF would trend to 1.0.  note update calls insert on a move
           so there is a double count there that must be adjusted for below.

           todo: greater sophistication could be helpful and added later.  for example the absolute 
                 size of documents might be considered -- in some cases smaller ones are more likely 
                 to grow than larger ones in the same collection? (not always)
        */
        void paddingFits() {
            MONGO_SOMETIMES(sometimes, 4) { // do this on a sampled basis to journal less
                double x = max(1.0, _paddingFactor - 0.001 );
                setPaddingFactor( x );
            }
        }
        void paddingTooSmall() {            
            MONGO_SOMETIMES(sometimes, 4) { // do this on a sampled basis to journal less       
                /* the more indexes we have, the higher the cost of a move.  so we take that into 
                   account herein.  note on a move that insert() calls paddingFits(), thus
                   here for example with no inserts and nIndexes = 1 we have
                   .001*4-.001 or a 3:1 ratio to non moves -> 75% nonmoves.  insert heavy 
                   can pushes this down considerably. further tweaking will be a good idea but 
                   this should be an adequate starting point.
                */
                double N = min(_nIndexes,7) + 3;
                double x = min(2.0,_paddingFactor + (0.001 * N));
                setPaddingFactor( x );
            }
        }

        const int systemFlags() const { return _systemFlags; }
        bool isSystemFlagSet( int flag ) const { return _systemFlags & flag; }
        void setSystemFlag( int flag );
        void clearSystemFlag( int flag );

        const int userFlags() const { return _userFlags; }
        bool isUserFlagSet( int flag ) const { return _userFlags & flag; }

        /**
         * these methods only modify NamespaceDetails and do not
         * sync changes back to system.namespaces
         * a typical call might
         if ( nsd->setUserFlag( 4 ) ) {
            nsd->syncUserFlags();
         }
         * these methods all return true iff only something was modified
         */

        bool setUserFlag( int flag );
        bool clearUserFlag( int flag );
        bool replaceUserFlags( int flags );

        void syncUserFlags( const string& ns );

        /* return which "deleted bucket" for this size object */
        static int bucket(int size) {
            for ( int i = 0; i < Buckets; i++ ) {
                if ( bucketSizes[i] > size ) {
                    // Return the first bucket sized _larger_ than the requested size.
                    return i;
                }
            }
            return MaxBucket;
        }

        /* @return the size for an allocated record quantized to 1/16th of the BucketSize.
           @param allocSize    requested size to allocate
           The returned size will be greater than or equal to 'allocSize'.
        */
        static int quantizeAllocationSpace(int allocSize);

        /**
         * Quantize 'allocSize' to the nearest bucketSize (or nearest 1mb boundary for large sizes).
         */
        static int quantizePowerOf2AllocationSpace(int allocSize);

        /** allocate space for a new record from deleted lists.
            @param lenToAlloc is WITH header
            @return null diskloc if no room - allocate a new extent then
        */
        DiskLoc alloc(Collection* collection, const StringData& ns, int lenToAlloc);

        /* add a given record to the deleted chains for this NS */
        void addDeletedRec(DeletedRecord *d, DiskLoc dloc);

        // Start from firstExtent by default.
        DiskLoc firstRecord( const DiskLoc &startExtent = DiskLoc() ) const;
        // Start from lastExtent by default.
        DiskLoc lastRecord( const DiskLoc &startExtent = DiskLoc() ) const;

        NamespaceDetails *writingWithoutExtra() {
            return ( NamespaceDetails* ) getDur().writingPtr( this, sizeof( NamespaceDetails ) );
        }
        /** Make all linked Extra objects writeable as well */
        NamespaceDetails *writingWithExtra();

    private:
        // @return offset in indexes[]
        int _catalogFindIndexByName( const StringData& name,
                                     bool includeBackgroundInProgress = false);

        void _removeIndexFromMe( int idx );

        /**
         * swaps all meta data for 2 indexes
         * a and b are 2 index ids, whose contents will be swapped
         * must have a lock on the entire collection to do this
         */
        void swapIndex( int a, int b );

        DiskLoc _alloc(Collection* collection, const StringData& ns, int len);
        void maybeComplain( const StringData& ns, int len ) const;
        DiskLoc __stdAlloc(int len, bool willBeAt);
        void compact(); // combine adjacent deleted records

        friend class Database;
        friend class NamespaceIndex;
        friend class IndexCatalog;
        friend class IndexCatalogEntry;

        struct ExtraOld {
            // note we could use this field for more chaining later, so don't waste it:
            unsigned long long reserved1;
            IndexDetails details[NIndexesExtra];
            unsigned reserved2;
            unsigned reserved3;
        };
        /** Update cappedLastDelRecLastExtent() after capExtent changed in cappedTruncateAfter() */
        void cappedTruncateLastDelUpdate();
        BOOST_STATIC_ASSERT( NIndexesMax <= NIndexesBase + NIndexesExtra*2 );
        BOOST_STATIC_ASSERT( NIndexesMax <= 64 ); // multiKey bits
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::ExtraOld) == 496 );
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) == 496 );
    }; // NamespaceDetails
#pragma pack()

} // namespace mongo
