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

#include "mongo/db/diskloc.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/key_generator.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class UpdateTicket;
    class InsertTicket;

    struct InsertDeleteOptions;
    struct PregeneratedKeysOnIndex;

    /**
     * An IndexAccessMethod is the interface through which all the mutation, lookup, and
     * traversal of index entries is done. The class is designed so that the underlying index
     * data structure is opaque to the caller.
     *
     * IndexAccessMethods for existing indices are obtained through the system catalog.
     *
     * We assume the caller has whatever locks required.  This interface is not thread safe.
     *
     */
    class IndexAccessMethod {
    public:
        virtual ~IndexAccessMethod() { }

        //
        // Lookup, traversal, and mutation support
        //

        /**
         * Internally generate the keys {k1, ..., kn} for 'obj'.  For each key k, insert (k ->
         * 'loc') into the index.  'obj' is the object at the location 'loc'.  If not NULL,
         * 'numInserted' will be set to the number of keys added to the index for the document.  If
         * there is more than one key for 'obj', either all keys will be inserted or none will.
         *
         * The behavior of the insertion can be specified through 'options'.
         *
         * prepared: if you generated keys before, you can pass the generator you used
         * and the keys you got.  If the generator matches, the keys are used.  Otherwise we
         * generate our own keys and you do not have to do anything.
         */
        virtual Status insert(const BSONObj& obj,
                              const DiskLoc& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numInserted,
                              const PregeneratedKeysOnIndex* prepared = NULL ) = 0;

        /**
         * Analogous to above, but remove the records instead of inserting them.  If not NULL,
         * numDeleted will be set to the number of keys removed from the index for the document.
         */
        virtual Status remove(const BSONObj& obj,
                              const DiskLoc& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numDeleted) = 0;

        /**
         * Checks whether the index entries for the document 'from', which is placed at location
         * 'loc' on disk, can be changed to the index entries for the doc 'to'. Provides a ticket
         * for actually performing the update.
         *
         * Returns an error if the update is invalid.  The ticket will also be marked as invalid.
         * Returns OK if the update should proceed without error.  The ticket is marked as valid.
         *
         * There is no obligation to perform the update after performing validation.
         */
        virtual Status validateUpdate(const BSONObj& from,
                                      const BSONObj& to,
                                      const DiskLoc& loc,
                                      const InsertDeleteOptions& options,
                                      UpdateTicket* ticket) = 0;

        /**
         * Perform a validated update.  The keys for the 'from' object will be removed, and the keys
         * for the object 'to' will be added.  Returns OK if the update succeeded, failure if it did
         * not.  If an update does not succeed, the index will be unmodified, and the keys for
         * 'from' will remain.  Assumes that the index has not changed since validateUpdate was
         * called.  If the index was changed, we may return an error, as our ticket may have been
         * invalidated.
         */
        virtual Status update(const UpdateTicket& ticket, int64_t* numUpdated) = 0;

        /**
         * Fills in '*out' with an IndexCursor.  Return a status indicating success or reason of
         * failure. If the latter, '*out' contains NULL.  See index_cursor.h for IndexCursor usage.
         */
        virtual Status newCursor(IndexCursor **out) const = 0;

        // ------ index level operations ------


        /**
         * initializes this index
         * only called once for the lifetime of the index
         * if called multiple times, is an error
         */
        virtual Status initializeAsEmpty() = 0;

        /**
         * Try to page-in the pages that contain the keys generated from 'obj'.
         * This can be used to speed up future accesses to an index by trying to ensure the
         * appropriate pages are not swapped out.
         * See prefetch.cpp.
         */
        virtual Status touch(const BSONObj& obj) = 0;

        /**
         * Try to page-in the pages that contain the keys.
         * This can be used to speed up future accesses to an index by trying to ensure the
         * appropriate pages are not swapped out.
         * See prefetch.cpp.
         */
        virtual Status touch(const BSONObjSet& keys) = 0;

        /**
         * Walk the entire index, checking the internal structure for consistency.
         * Set numKeys to the number of keys in the index.
         *
         * Return OK if the index is valid.
         *
         * Currently wasserts that the index is invalid.  This could/should be changed in
         * the future to return a Status.
         */
        virtual Status validate(int64_t* numKeys) = 0;

        //
        // Bulk operations support
        //

        /**
         * Starts a bulk operation.
         * You work on the returned IndexAccessMethod and then call commitBulk.
         * This can return NULL, meaning bulk mode is not available.
         *
         * Long term, you'll eventually be able to mix/match bulk, not bulk,
         * have as many as you want, etc..
         *
         * For now (1/8/14) you can only do bulk when the index is empty
         * it will fail if you try other times.
         */
        virtual IndexAccessMethod* initiateBulk() = 0;

        /**
         * Call this when you are ready to finish your bulk work.
         * Pass in the IndexAccessMethod gotten from initiateBulk.
         * After this method is called, the bulk index access method is invalid
         * and should not be used.
         * @param bulk - something created from initiateBulk
         * @param mayInterrupt - is this commit interruptable (will cancel)
         * @param dups - if NULL, error out on dups if not allowed
         *               if not NULL, put the bad DiskLocs there
         */
        virtual Status commitBulk( IndexAccessMethod* bulk,
                                   bool mayInterrupt,
                                   std::set<DiskLoc>* dups ) = 0;

        /**
         * this returns a shared_ptr so that someone can get all the generators in a lock,
         * then unlock, generate keys, and then re-lock and use those keys
         */
        virtual shared_ptr<KeyGenerator> getKeyGenerator() const = 0;
    };

    /**
     * Updates are two steps: verify that it's a valid update, and perform it.
     * validateUpdate fills out the UpdateStatus and update actually applies it.
     */
    class UpdateTicket {
    public:
        UpdateTicket() : _isValid(false) { }

    protected:
        // These friends are the classes that actually fill out an UpdateStatus.
        friend class BtreeBasedAccessMethod;

        class PrivateUpdateData;

        bool _isValid;

        // This is meant to be filled out only by the friends above.
        scoped_ptr<PrivateUpdateData> _indexSpecificUpdateData;
    };

    class UpdateTicket::PrivateUpdateData {
    public:
        virtual ~PrivateUpdateData() { }
    };

    /**
     * Flags we can set for inserts and deletes (and updates, which are kind of both).
     */
    struct InsertDeleteOptions {
        InsertDeleteOptions() : logIfError(false), dupsAllowed(false), ignoreKeyTooLong(false) { }

        // If there's an error, log() it.
        bool logIfError;

        // Are duplicate keys allowed in the index?
        bool dupsAllowed;

        // Ignore key too long failures.
        bool ignoreKeyTooLong;
    };

}  // namespace mongo
