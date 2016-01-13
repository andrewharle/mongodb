/**
*    Copyright (C) 2013 10gen Inc.
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

#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/index/btree_interface.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

    class BtreeIndexCursor : public IndexCursor {
    public:
        virtual ~BtreeIndexCursor();

        bool isEOF() const;

        /**
         * Called from btree.cpp when we're about to delete a Btree bucket. The index descriptor
         * is needed as just the DiskLoc of the bucket is not unique across databases, which might
         * result in incorrect invalidation of cursors in other unlocked databases.
         */
        static void aboutToDeleteBucket(const IndexCatalogEntry* index,
                                        const DiskLoc& bucket);

        virtual Status setOptions(const CursorOptions& options);

        virtual Status seek(const BSONObj& position);

        // Btree-specific seeking functions.
        Status seek(const vector<const BSONElement*>& position,
                    const vector<bool>& inclusive);

        /**
         * Seek to the key 'position'.  If 'afterKey' is true, seeks to the first
         * key that is oriented after 'position'.
         *
         * Btree-specific.
         */
        void seek(const BSONObj& position, bool afterKey);

        Status skip(const BSONObj &keyBegin, int keyBeginLen, bool afterKey,
                    const vector<const BSONElement*>& keyEnd,
                    const vector<bool>& keyEndInclusive);

        virtual BSONObj getKey() const;
        virtual DiskLoc getValue() const;
        virtual void next();

        /**
         * BtreeIndexCursor-only.
         * Returns true if 'this' points at the same exact key as 'other'.
         * Returns false otherwise.
         */
        bool pointsAt(const BtreeIndexCursor& other);

        virtual Status savePosition();

        virtual Status restorePosition();

        virtual string toString();

    private:
        // We keep the constructor private and only allow the AM to create us.
        friend class BtreeBasedAccessMethod;

        // For handling bucket deletion.
        static unordered_set<BtreeIndexCursor*> _activeCursors;
        static SimpleMutex _activeCursorsMutex;

        /**
         * btreeState is the ICE of the Btree that we're going to traverse.
         * head is the head of the Btree.
         * interface is an abstraction to hide the fact that we have two types of Btrees.
         *
         * Go forward by default.
         *
         * Intentionally private, we're friends with the only class allowed to call it.
         */
        BtreeIndexCursor(const IndexCatalogEntry* btreeState,
                         const DiskLoc head,
                         BtreeInterface *interface);

        void skipUnusedKeys();

        bool isSavedPositionValid();

        // Move to the next/prev. key.  Used by normal getNext and also skipping unused keys.
        void advance(const char* caller);

        // For saving/restoring position.
        BSONObj _savedKey;
        DiskLoc _savedLoc;

        BSONObj _emptyObj;

        int _direction;
        const IndexCatalogEntry* _btreeState; // not-owned
        BtreeInterface* _interface;

        // What are we looking at RIGHT NOW?  We look at a bucket.
        DiskLoc _bucket;
        // And we look at an offset in the bucket.
        int _keyOffset;
    };

}  // namespace mongo
