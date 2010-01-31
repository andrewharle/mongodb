// namespace.h

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

#pragma once

#include "../stdafx.h"
#include "jsobj.h"
#include "queryutil.h"
#include "storage.h"
#include "../util/hashtab.h"
#include "../util/mmap.h"

namespace mongo {

    class Cursor;

#pragma pack(1)

	/* in the mongo source code, "client" means "database". */

    const int MaxDatabaseLen = 256; // max str len for the db name, including null char

	// "database.a.b.c" -> "database"
    inline void nsToDatabase(const char *ns, char *database) {
        const char *p = ns;
        char *q = database;
        while ( *p != '.' ) {
            if ( *p == 0 )
                break;
            *q++ = *p++;
        }
        *q = 0;
        if (q-database>=MaxDatabaseLen) {
            log() << "nsToDatabase: ns too long. terminating, buf overrun condition" << endl;
            dbexit( EXIT_POSSIBLE_CORRUPTION );
        }
    }
    inline string nsToDatabase(const char *ns) {
        char buf[MaxDatabaseLen];
        nsToDatabase(ns, buf);
        return buf;
    }

	/* e.g.
	   NamespaceString ns("acme.orders");
	   cout << ns.coll; // "orders"
	*/
    class NamespaceString {
    public:
        string db;
        string coll; // note collection names can have periods in them for organizing purposes (e.g. "system.indexes")
    private:
        void init(const char *ns) { 
            const char *p = strchr(ns, '.');
            if( p == 0 ) return;
            db = string(ns, p - ns);
            coll = p + 1;
        }
    public:
        NamespaceString( const char * ns ) { init(ns); }
        NamespaceString( const string& ns ) { init(ns.c_str()); }

        bool isSystem() { 
            return strncmp(coll.c_str(), "system.", 7) == 0;
        }
    };

	/* This helper class is used to make the HashMap below in NamespaceDetails */
    class Namespace {
    public:
        enum MaxNsLenValue { MaxNsLen = 128 };
        Namespace(const char *ns) {
            *this = ns;
        }
        Namespace& operator=(const char *ns) {
            uassert( 10080 , "ns name too long, max size is 128", strlen(ns) < MaxNsLen);
            //memset(buf, 0, MaxNsLen); /* this is just to keep stuff clean in the files for easy dumping and reading */
            strcpy_s(buf, MaxNsLen, ns);
            return *this;
        }

        /* for more than 10 indexes -- see NamespaceDetails::Extra */
        string extraName() { 
            string s = string(buf) + "$extra";
            massert( 10348 , "ns name too long", s.size() < MaxNsLen);
            return s;
        }

        void kill() {
            buf[0] = 0x7f;
        }

        bool operator==(const char *r) {
            return strcmp(buf, r) == 0;
        }
        bool operator==(const Namespace& r) {
            return strcmp(buf, r.buf) == 0;
        }
        int hash() const {
            unsigned x = 0;
            const char *p = buf;
            while ( *p ) {
                x = x * 131 + *p;
                p++;
            }
            return (x & 0x7fffffff) | 0x8000000; // must be > 0
        }

        /**
           ( foo.bar ).getSisterNS( "blah" ) == foo.blah
		   perhaps this should move to the NamespaceString helper?
         */
        string getSisterNS( const char * local ) {
            assert( local && local[0] != '.' );
            string old(buf);
            if ( old.find( "." ) != string::npos )
                old = old.substr( 0 , old.find( "." ) );
            return old + "." + local;
        }

        operator string() const {
            return (string)buf;
        }

        char buf[MaxNsLen];
    };

}

#include "index.h"

namespace mongo {

    /**
       @return true if a client can modify this namespace
       things like *.system.users
     */
    bool legalClientSystemNS( const string& ns , bool write );


    /* deleted lists -- linked lists of deleted records -- are placed in 'buckets' of various sizes
       so you can look for a deleterecord about the right size.
    */
    const int Buckets = 19;
    const int MaxBucket = 18;

    extern int bucketSizes[];

    /* this is the "header" for a collection that has all its details.  in the .ns file.
    */
    class NamespaceDetails {
        friend class NamespaceIndex;
        enum { NIndexesExtra = 30,
               NIndexesBase  = 10
        };
        struct Extra { 
            // note we could use this field for more chaining later, so don't waste it:
            unsigned long long reserved1; 
            IndexDetails details[NIndexesExtra];
            unsigned reserved2;
            unsigned reserved3;
        };
        Extra* extra() { 
            assert( extraOffset );
            return (Extra *) (((char *) this) + extraOffset);
        }
    public:
        void copyingFrom(const char *thisns, NamespaceDetails *src); // must be called when renaming a NS to fix up extra

        enum { NIndexesMax = 40 };

        BOOST_STATIC_ASSERT( NIndexesMax == NIndexesBase + NIndexesExtra );

        NamespaceDetails( const DiskLoc &loc, bool _capped ) {
            /* be sure to initialize new fields here -- doesn't default to zeroes the way we use it */
            firstExtent = lastExtent = capExtent = loc;
            datasize = nrecords = 0;
            lastExtentSize = 0;
            nIndexes = 0;
            capped = _capped;
            max = 0x7fffffff;
            paddingFactor = 1.0;
            flags = 0;
            capFirstNewRecord = DiskLoc();
            // Signal that we are on first allocation iteration through extents.
            capFirstNewRecord.setInvalid();
            // For capped case, signal that we are doing initial extent allocation.
            if ( capped )
                deletedList[ 1 ].setInvalid();
			assert( sizeof(dataFileVersion) == 2 );
			dataFileVersion = 0;
			indexFileVersion = 0;
            multiKeyIndexBits = 0;
            reservedA = 0;
            extraOffset = 0;
            backgroundIndexBuildInProgress = 0;
            memset(reserved, 0, sizeof(reserved));
        }
        DiskLoc firstExtent;
        DiskLoc lastExtent;

        /* NOTE: capped collections override the meaning of deleted list.  
                 deletedList[0] points to a list of free records (DeletedRecord's) for all extents in
                 the namespace.
                 deletedList[1] points to the last record in the prev extent.  When the "current extent" 
                 changes, this value is updated.  !deletedList[1].isValid() when this value is not 
                 yet computed.
        */
        DiskLoc deletedList[Buckets];

        long long datasize;
        long long nrecords;
        int lastExtentSize;
        int nIndexes;
    private:
        IndexDetails _indexes[NIndexesBase];
    public:
        int capped;
        int max; // max # of objects for a capped table.
        double paddingFactor; // 1.0 = no padding.
        int flags;
        DiskLoc capExtent;
        DiskLoc capFirstNewRecord;

        /* NamespaceDetails version.  So we can do backward compatibility in the future.
		   See filever.h
        */
		unsigned short dataFileVersion;
		unsigned short indexFileVersion;

        unsigned long long multiKeyIndexBits;
    private:
        unsigned long long reservedA;
        long long extraOffset; // where the $extra info is located (bytes relative to this)
    public:
        int backgroundIndexBuildInProgress; // 1 if in prog
        char reserved[76];

        /* NOTE: be careful with flags.  are we manipulating them in read locks?  if so, 
                 this isn't thread safe.  TODO
        */
        enum NamespaceFlags {
            Flag_HaveIdIndex = 1 << 0, // set when we have _id index (ONLY if ensureIdIndex was called -- 0 if that has never been called)
            Flag_CappedDisallowDelete = 1 << 1 // set when deletes not allowed during capped table allocation.
        };

        IndexDetails& idx(int idxNo) {
            if( idxNo < NIndexesBase ) 
                return _indexes[idxNo];
            return extra()->details[idxNo-NIndexesBase];
        }

        class IndexIterator { 
            friend class NamespaceDetails;
            int i;
            int n;
            NamespaceDetails *d;
            Extra *e;
            IndexIterator(NamespaceDetails *_d) { 
                d = _d;
                i = 0;
                n = d->nIndexes;
                if( n > NIndexesBase )
                    e = d->extra();
            }
        public:
            int pos() { return i; } // note this is the next one to come
            bool more() { return i < n; }
            IndexDetails& next() { 
                int k = i;
                i++;
                return k < NIndexesBase ? d->_indexes[k] : 
                    e->details[k-10];
            }
        };

        IndexIterator ii() { 
            return IndexIterator(this);
        }

        /* hackish - find our index # in the indexes array
        */
        int idxNo(IndexDetails& idx) { 
            IndexIterator i = ii();
            while( i.more() ) {
                if( &i.next() == &idx )
                    return i.pos()-1;
            }
            massert( 10349 , "E12000 idxNo fails", false);
            return -1;
        }

        /* multikey indexes are indexes where there are more than one key in the index
             for a single document. see multikey in wiki.
           for these, we have to do some dedup work on queries.
        */
        bool isMultikey(int i) {
            return (multiKeyIndexBits & (((unsigned long long) 1) << i)) != 0;
        }
        void setIndexIsMultikey(int i) { 
            dassert( i < NIndexesMax );
            multiKeyIndexBits |= (((unsigned long long) 1) << i);
        }
        void clearIndexIsMultikey(int i) { 
            dassert( i < NIndexesMax );
            multiKeyIndexBits &= ~(((unsigned long long) 1) << i);
        }

        /* add a new index.  does not add to system.indexes etc. - just to NamespaceDetails.
           caller must populate returned object. 
         */
        IndexDetails& addIndex(const char *thisns);

        void aboutToDeleteAnIndex() {
            flags &= ~Flag_HaveIdIndex;
        }

        void cappedDisallowDelete() {
            flags |= Flag_CappedDisallowDelete;
        }
        
        /* returns index of the first index in which the field is present. -1 if not present. */
        int fieldIsIndexed(const char *fieldName);

        void paddingFits() {
            double x = paddingFactor - 0.01;
            if ( x >= 1.0 )
                paddingFactor = x;
        }
        void paddingTooSmall() {
            double x = paddingFactor + 0.6;
            if ( x <= 2.0 )
                paddingFactor = x;
        }

        //returns offset in indexes[]
        int findIndexByName(const char *name) {
            IndexIterator i = ii();
            while( i.more() ) {
                if ( strcmp(i.next().info.obj().getStringField("name"),name) == 0 )
                    return i.pos()-1;
            }
            return -1;
        }

        //returns offset in indexes[]
        int findIndexByKeyPattern(const BSONObj& keyPattern) {
            IndexIterator i = ii();
            while( i.more() ) {
                if( i.next().keyPattern() == keyPattern ) 
                    return i.pos()-1;
            }
            return -1;
        }

        /* @return -1 = not found 
           generally id is first index, so not that expensive an operation (assuming present).
        */
        int findIdIndex() {
            IndexIterator i = ii();
            while( i.more() ) {
                if( i.next().isIdIndex() ) 
                    return i.pos()-1;
            }
            return -1;
        }

        /* return which "deleted bucket" for this size object */
        static int bucket(int n) {
            for ( int i = 0; i < Buckets; i++ )
                if ( bucketSizes[i] > n )
                    return i;
            return Buckets-1;
        }

        /* allocate a new record.  lenToAlloc includes headers. */
        DiskLoc alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc);

        /* add a given record to the deleted chains for this NS */
        void addDeletedRec(DeletedRecord *d, DiskLoc dloc);

        void dumpDeleted(set<DiskLoc> *extents = 0);

        bool capLooped() const {
            return capped && capFirstNewRecord.isValid();
        }

        // Start from firstExtent by default.
        DiskLoc firstRecord( const DiskLoc &startExtent = DiskLoc() ) const;

        // Start from lastExtent by default.
        DiskLoc lastRecord( const DiskLoc &startExtent = DiskLoc() ) const;

        bool inCapExtent( const DiskLoc &dl ) const;

        void checkMigrate();

        long long storageSize();

    private:
        bool cappedMayDelete() const {
            return !( flags & Flag_CappedDisallowDelete );
        }
        Extent *theCapExtent() const {
            return capExtent.ext();
        }
        void advanceCapExtent( const char *ns );
        void maybeComplain( const char *ns, int len ) const;
        DiskLoc __stdAlloc(int len);
        DiskLoc __capAlloc(int len);
        DiskLoc _alloc(const char *ns, int len);
        void compact(); // combine adjacent deleted records

        DiskLoc &firstDeletedInCapExtent();
        bool nextIsInCapExtent( const DiskLoc &dl ) const;
    };

#pragma pack()

    /* these are things we know / compute about a namespace that are transient -- things
       we don't actually store in the .ns file.  so mainly caching of frequently used
       information.

       CAUTION: Are you maintaining this properly on a collection drop()?  A dropdatabase()?  Be careful.
                The current field "allIndexKeys" may have too many keys in it on such an occurrence;
                as currently used that does not cause anything terrible to happen.

       todo: cleanup code, need abstractions and separation
    */
    class NamespaceDetailsTransient : boost::noncopyable {
        /* general ------------------------------------------------------------- */
    private:
        string _ns;
        void reset();
        static std::map< string, shared_ptr< NamespaceDetailsTransient > > _map;
    public:
        NamespaceDetailsTransient(const char *ns) : _ns(ns), _keysComputed(false), _qcWriteCount(), _cll_enabled() { }
        /* _get() is not threadsafe */
        static NamespaceDetailsTransient& _get(const char *ns);
        /* use get_w() when doing write operations */
        static NamespaceDetailsTransient& get_w(const char *ns) { 
            DEV assertInWriteLock();
            return _get(ns);
        }
        void addedIndex() { reset(); }
        void deletedIndex() { reset(); }
        /* Drop cached information on all namespaces beginning with the specified prefix.
           Can be useful as index namespaces share the same start as the regular collection. 
           SLOW - sequential scan of all NamespaceDetailsTransient objects */
        static void clearForPrefix(const char *prefix);

        /* indexKeys() cache ---------------------------------------------------- */
        /* assumed to be in write lock for this */
    private:
        bool _keysComputed;
        set<string> _indexKeys;
        void computeIndexKeys();
    public:
        /* get set of index keys for this namespace.  handy to quickly check if a given
           field is indexed (Note it might be a secondary component of a compound index.)
        */
        set<string>& indexKeys() {
            DEV assertInWriteLock();
            if ( !_keysComputed )
                computeIndexKeys();
            return _indexKeys;
        }

        /* IndexSpec caching */
    private:
        map<const IndexDetails*,IndexSpec> _indexSpecs;
    public:
        const IndexSpec& getIndexSpec( const IndexDetails * details ){
            DEV assertInWriteLock();
            IndexSpec& spec = _indexSpecs[details];
            if ( spec.meta.isEmpty() ){
                spec.reset( details->info );
            }
            return spec;
        }

        /* query cache (for query optimizer) ------------------------------------- */
    private:
        int _qcWriteCount;
        map< QueryPattern, pair< BSONObj, long long > > _qcCache;
    public:
        static boost::mutex _qcMutex;
        /* you must be in the qcMutex when calling this (and using the returned val): */
        static NamespaceDetailsTransient& get_inlock(const char *ns) {
            return _get(ns);
        }
        void clearQueryCache() { // public for unit tests
            _qcCache.clear();
            _qcWriteCount = 0;
        }
        /* you must notify the cache if you are doing writes, as query plan optimality will change */
        void notifyOfWriteOp() {
            if ( _qcCache.empty() )
                return;
            if ( ++_qcWriteCount >= 100 )
                clearQueryCache();
        }
        BSONObj indexForPattern( const QueryPattern &pattern ) {
            return _qcCache[ pattern ].first;
        }
        long long nScannedForPattern( const QueryPattern &pattern ) {
            return _qcCache[ pattern ].second;
        }
        void registerIndexForPattern( const QueryPattern &pattern, const BSONObj &indexKey, long long nScanned ) {
            _qcCache[ pattern ] = make_pair( indexKey, nScanned );
        }

        /* for collection-level logging -- see CmdLogCollection ----------------- */ 
        /* assumed to be in write lock for this */
    private:
        string _cll_ns; // "local.temp.oplog." + _ns;
        bool _cll_enabled;
        void cllDrop(); // drop _cll_ns
    public:
        string cllNS() const { return _cll_ns; }
        bool cllEnabled() const { return _cll_enabled; }
        void cllStart( int logSizeMb = 256 ); // begin collection level logging
        void cllInvalidate();
        bool cllValidateComplete();

    }; /* NamespaceDetailsTransient */

    inline NamespaceDetailsTransient& NamespaceDetailsTransient::_get(const char *ns) {
        shared_ptr< NamespaceDetailsTransient > &t = _map[ ns ];
        if ( t.get() == 0 )
            t.reset( new NamespaceDetailsTransient(ns) );
        return *t;
    }

    /* NamespaceIndex is the ".ns" file you see in the data directory.  It is the "system catalog"
       if you will: at least the core parts.  (Additional info in system.* collections.)
    */
    class NamespaceIndex {
        friend class NamespaceCursor;
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) <= sizeof(NamespaceDetails) );
    public:
        NamespaceIndex(const string &dir, const string &database) :
        ht( 0 ),
        dir_( dir ),
        database_( database ) {}

        /* returns true if new db will be created if we init lazily */
        bool exists() const;
        
        void init();

        void add_ns(const char *ns, DiskLoc& loc, bool capped) {
            NamespaceDetails details( loc, capped );
			add_ns( ns, details );
        }

		void add_ns( const char *ns, const NamespaceDetails &details ) {
            init();
            Namespace n(ns);
            uassert( 10081 , "too many namespaces/collections", ht->put(n, details));
		}

        /* just for diagnostics */
        size_t detailsOffset(NamespaceDetails *d) {
            if ( !ht )
                return -1;
            return ((char *) d) -  (char *) ht->nodes;
        }

        /* extra space for indexes when more than 10 */
        NamespaceDetails::Extra* allocExtra(const char *ns) { 
            Namespace n(ns);
            Namespace extra(n.extraName().c_str()); // throws userexception if ns name too long
            NamespaceDetails *d = details(ns);
            massert( 10350 ,  "allocExtra: base ns missing?", d );
            assert( d->extraOffset == 0 );
            massert( 10351 ,  "allocExtra: extra already exists", ht->get(extra) == 0 );
            NamespaceDetails::Extra temp;
            memset(&temp, 0, sizeof(temp));
            uassert( 10082 ,  "allocExtra: too many namespaces/collections", ht->put(extra, (NamespaceDetails&) temp));
            NamespaceDetails::Extra *e = (NamespaceDetails::Extra *) ht->get(extra);
            d->extraOffset = ((char *) e) - ((char *) d);
            assert( d->extra() == e );
            return e;
        }

        NamespaceDetails* details(const char *ns) {
            if ( !ht )
                return 0;
            Namespace n(ns);
            NamespaceDetails *d = ht->get(n);
            if ( d )
                d->checkMigrate();
            return d;
        }

        void kill_ns(const char *ns) {
            if ( !ht )
                return;
            Namespace n(ns);
            ht->kill(n);

            try {
                Namespace extra(n.extraName().c_str());
                ht->kill(extra);
            }
            catch(DBException&) { }
        }

        bool find(const char *ns, DiskLoc& loc) {
            NamespaceDetails *l = details(ns);
            if ( l ) {
                loc = l->firstExtent;
                return true;
            }
            return false;
        }

        bool allocated() const {
            return ht != 0;
        }

    private:
        boost::filesystem::path path() const;
        
        MemoryMappedFile f;
        HashTable<Namespace,NamespaceDetails> *ht;
        string dir_;
        string database_;
    };

    extern string dbpath; // --dbpath parm 

    // Rename a namespace within current 'client' db.
    // (Arguments should include db name)
    void renameNamespace( const char *from, const char *to );

} // namespace mongo
