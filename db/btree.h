// btree.h

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
#include "storage.h"
#include "pdfile.h"

namespace mongo {

#pragma pack(1)

    struct _KeyNode {
        DiskLoc prevChildBucket;
        DiskLoc recordLoc;
        short keyDataOfs() const {
            return (short) _kdo;
        }
        unsigned short _kdo;
        void setKeyDataOfs(short s) {
            _kdo = s;
            assert(s>=0);
        }
        void setKeyDataOfsSavingUse(short s) {
            _kdo = s;
            assert(s>=0);
        }
        void setUsed() { 
            recordLoc.GETOFS() &= ~1;
        }
        void setUnused() {
            /* Setting ofs to odd is the sentinel for unused, as real recordLoc's are always
               even numbers.
               Note we need to keep its value basically the same as we use the recordLoc
               as part of the key in the index (to handle duplicate keys efficiently).
            */
            recordLoc.GETOFS() |= 1;
        }
        int isUnused() {
            return recordLoc.getOfs() & 1;
        }
        int isUsed() {
            return !isUnused();
        }
    };

#pragma pack()

    class BucketBasics;

    /* wrapper - this is our in memory representation of the key.  _KeyNode is the disk representation. */
    class KeyNode {
    public:
        KeyNode(const BucketBasics& bb, const _KeyNode &k);
        const DiskLoc& prevChildBucket;
        const DiskLoc& recordLoc;
        BSONObj key;
    };

#pragma pack(1)

    /* this class is all about the storage management */
    class BucketBasics {
        friend class BtreeBuilder;
        friend class KeyNode;
    public:
        void dumpTree(DiskLoc thisLoc, const BSONObj &order);
        bool isHead() { return parent.isNull(); }
        void assertValid(const BSONObj &order, bool force = false);
        int fullValidate(const DiskLoc& thisLoc, const BSONObj &order); /* traverses everything */
    protected:
        void modified(const DiskLoc& thisLoc);
        KeyNode keyNode(int i) const {
            if ( i >= n ){
                massert( (string)"invalid keyNode: " +  BSON( "i" << i << "n" << n ).jsonString() , i < n );
            }
            return KeyNode(*this, k(i));
        }

        char * dataAt(short ofs) {
            return data + ofs;
        }

        void init(); // initialize a new node

        /* returns false if node is full and must be split
           keypos is where to insert -- inserted after that key #.  so keypos=0 is the leftmost one.
        */
        bool basicInsert(const DiskLoc& thisLoc, int keypos, const DiskLoc& recordLoc, const BSONObj& key, const BSONObj &order);
        
        /**
         * @return true if works, false if not enough space
         */
        bool _pushBack(const DiskLoc& recordLoc, BSONObj& key, const BSONObj &order, DiskLoc prevChild);
        void pushBack(const DiskLoc& recordLoc, BSONObj& key, const BSONObj &order, DiskLoc prevChild){
            bool ok = _pushBack( recordLoc , key , order , prevChild );
            assert(ok);
        }
        void popBack(DiskLoc& recLoc, BSONObj& key);
        void _delKeyAtPos(int keypos); // low level version that doesn't deal with child ptrs.

        /* !Packed means there is deleted fragment space within the bucket.
           We "repack" when we run out of space before considering the node
           to be full.
           */
        enum Flags { Packed=1 };

        DiskLoc& childForPos(int p) {
            return p == n ? nextChild : k(p).prevChildBucket;
        }

        int totalDataSize() const;
        void pack( const BSONObj &order );
        void setNotPacked();
        void setPacked();
        int _alloc(int bytes);
        void _unalloc(int bytes);
        void truncateTo(int N, const BSONObj &order);
        void markUnused(int keypos);

        /* BtreeBuilder uses the parent var as a temp place to maintain a linked list chain. 
           we use tempNext() when we do that to be less confusing. (one might have written a union in C)
           */
        DiskLoc& tempNext() { return parent; }

    public:
        DiskLoc parent;

        string bucketSummary() const {
            stringstream ss;
            ss << "  Bucket info:" << endl;
            ss << "    n: " << n << endl;
            ss << "    parent: " << parent.toString() << endl;
            ss << "    nextChild: " << parent.toString() << endl;
            ss << "    Size: " << _Size << " flags:" << flags << endl;
            ss << "    emptySize: " << emptySize << " topSize: " << topSize << endl;
            return ss.str();
        }

    protected:
        void _shape(int level, stringstream&);
        DiskLoc nextChild; // child bucket off and to the right of the highest key.
        int _Size; // total size of this btree node in bytes. constant.
        int Size() const;
        int flags;
        int emptySize; // size of the empty region
        int topSize; // size of the data at the top of the bucket (keys are at the beginning or 'bottom')
        int n; // # of keys so far.
        int reserved;
        const _KeyNode& k(int i) const {
            return ((_KeyNode*)data)[i];
        }
        _KeyNode& k(int i) {
            return ((_KeyNode*)data)[i];
        }
        char data[4];
    };

    class BtreeBucket : public BucketBasics {
        friend class BtreeCursor;
    public:
        void dump();

        /* @return true if key exists in index 

           order - indicates order of keys in the index.  this is basically the index's key pattern, e.g.:
             BSONObj order = ((IndexDetails&)idx).keyPattern();
           likewise below in bt_insert() etc.
        */
        bool exists(const IndexDetails& idx, DiskLoc thisLoc, const BSONObj& key, BSONObj order);

        static DiskLoc addBucket(IndexDetails&); /* start a new index off, empty */
        void deallocBucket(const DiskLoc &thisLoc); // clear bucket memory, placeholder for deallocation
        
        static void renameIndexNamespace(const char *oldNs, const char *newNs);

        int bt_insert(DiskLoc thisLoc, DiskLoc recordLoc,
                   const BSONObj& key, const BSONObj &order, bool dupsAllowed,
                   IndexDetails& idx, bool toplevel = true);

        bool unindex(const DiskLoc& thisLoc, IndexDetails& id, BSONObj& key, const DiskLoc& recordLoc);

        /* locate may return an "unused" key that is just a marker.  so be careful.
             looks for a key:recordloc pair.

           found - returns true if exact match found.  note you can get back a position 
                   result even if found is false.
        */
        DiskLoc locate(const IndexDetails& , const DiskLoc& thisLoc, const BSONObj& key, const BSONObj &order, 
                       int& pos, bool& found, DiskLoc recordLoc, int direction=1);
        
        /**
         * find the first instance of the key
         * does not handle dups
         * returned DiskLock isNull if can't find anything with that
         */
        DiskLoc findSingle( const IndexDetails& , const DiskLoc& thisLoc, const BSONObj& key );

        /* advance one key position in the index: */
        DiskLoc advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller);
        DiskLoc getHead(const DiskLoc& thisLoc);

        /* get tree shape */
        void shape(stringstream&);

        static void a_test(IndexDetails&);

    private:
        void fixParentPtrs(const DiskLoc& thisLoc);
        void delBucket(const DiskLoc& thisLoc, IndexDetails&);
        void delKeyAtPos(const DiskLoc& thisLoc, IndexDetails& id, int p);
        BSONObj keyAt(int keyOfs) {
            return keyOfs >= n ? BSONObj() : keyNode(keyOfs).key;
        }
        static BtreeBucket* allocTemp(); /* caller must release with free() */
        void insertHere(DiskLoc thisLoc, int keypos,
                        DiskLoc recordLoc, const BSONObj& key, const BSONObj &order,
                        DiskLoc lchild, DiskLoc rchild, IndexDetails&);
        int _insert(DiskLoc thisLoc, DiskLoc recordLoc,
                    const BSONObj& key, const BSONObj &order, bool dupsAllowed,
                    DiskLoc lChild, DiskLoc rChild, IndexDetails&);
        bool find(const IndexDetails& idx, const BSONObj& key, DiskLoc recordLoc, const BSONObj &order, int& pos, bool assertIfDup);
        static void findLargestKey(const DiskLoc& thisLoc, DiskLoc& largestLoc, int& largestKey);
    public:
        // simply builds and returns a dup key error message string
        static string dupKeyError( const IndexDetails& idx , const BSONObj& key );
    };

    class BtreeCursor : public Cursor {
    public:
        BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails&, const BSONObj &startKey, const BSONObj &endKey, bool endKeyInclusive, int direction );

        BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, const BoundList &_bounds, int _direction );

        virtual bool ok() {
            return !bucket.isNull();
        }
        bool eof() {
            return !ok();
        }
        virtual bool advance();

        virtual void noteLocation(); // updates keyAtKeyOfs...
        virtual void checkLocation();

        /* used for multikey index traversal to avoid sending back dups. see JSMatcher::matches().
           if a multikey index traversal:
             if loc has already been sent, returns true.
             otherwise, marks loc as sent.
             @return true if the loc has not been seen
        */
        set<DiskLoc> dups;
        virtual bool getsetdup(DiskLoc loc) {
            if( multikey ) { 
                pair<set<DiskLoc>::iterator, bool> p = dups.insert(loc);
                return !p.second;
            }
            return false;
        }

        _KeyNode& _currKeyNode() {
            assert( !bucket.isNull() );
            _KeyNode& kn = bucket.btree()->k(keyOfs);
            assert( kn.isUsed() );
            return kn;
        }
        KeyNode currKeyNode() const {
            assert( !bucket.isNull() );
            return bucket.btree()->keyNode(keyOfs);
        }
        virtual BSONObj currKey() const {
            return currKeyNode().key;
        }

        virtual BSONObj indexKeyPattern() {
            return indexDetails.keyPattern();
        }

        virtual void aboutToDeleteBucket(const DiskLoc& b) {
            if ( bucket == b )
                keyOfs = -1;
        }

        virtual DiskLoc currLoc() {
            return !bucket.isNull() ? _currKeyNode().recordLoc : DiskLoc();
        }
        virtual DiskLoc refLoc() {
            return currLoc();
        }
        virtual Record* _current() {
            return currLoc().rec();
        }
        virtual BSONObj current() {
            return BSONObj(_current());
        }
        virtual string toString() {
            string s = string("BtreeCursor ") + indexDetails.indexName();
            if ( direction < 0 ) s += " reverse";
            if ( bounds_.size() > 1 ) s += " multi";
            return s;
        }

        BSONObj prettyKey( const BSONObj &key ) const {
            return key.replaceFieldNames( indexDetails.keyPattern() ).clientReadable();
        }

        virtual BSONObj prettyStartKey() const {
            return prettyKey( startKey );
        }
        virtual BSONObj prettyEndKey() const {
            return prettyKey( endKey );
        }
        
        void forgetEndKey() { endKey = BSONObj(); }
        
    private:
        /* Our btrees may (rarely) have "unused" keys when items are deleted.
           Skip past them.
        */
        void skipUnusedKeys();

        /* Check if the current key is beyond endKey. */
        void checkEnd();

        // selective audits on construction
        void audit();

        // set initial bucket
        void init();

        // init start / end keys with a new range
        void initInterval();

        friend class BtreeBucket;
        NamespaceDetails *d;
        int idxNo;
        BSONObj startKey;
        BSONObj endKey;
        bool endKeyInclusive_;
        bool multikey; // note this must be updated every getmore batch in case someone added a multikey...

        const IndexDetails& indexDetails;
        BSONObj order;
        DiskLoc bucket;
        int keyOfs;
        int direction; // 1=fwd,-1=reverse
        BSONObj keyAtKeyOfs; // so we can tell if things moved around on us between the query and the getMore call
        DiskLoc locAtKeyOfs;
        BoundList bounds_;
        unsigned boundIndex_;
    };

#pragma pack()

    inline bool IndexDetails::hasKey(const BSONObj& key) { 
        return head.btree()->exists(*this, head, key, keyPattern());
    }

    /* build btree from the bottom up */
    /* _ TODO dropDups */
    class BtreeBuilder {
        bool dupsAllowed; 
        IndexDetails& idx;
        unsigned long long n;
        BSONObj keyLast;
        BSONObj order;
        bool committed;

        DiskLoc cur, first;
        BtreeBucket *b;

        void newBucket();
        void buildNextLevel(DiskLoc);

    public:
        ~BtreeBuilder();

        BtreeBuilder(bool _dupsAllowed, IndexDetails& _idx);

        /* keys must be added in order */
        void addKey(BSONObj& key, DiskLoc loc);

        /* commit work.  if not called, destructor will clean up partially completed work 
           (in case exception has happened).
        */
        void commit();

        unsigned long long getn() { return n; }
    };

} // namespace mongo;
