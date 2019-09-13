
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

#pragma once

#include <initializer_list>
#include <ostream>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/sync_tail_test_fixture.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {

class Collection;

namespace repl {

struct CollectionState {
    CollectionState() = default;
    CollectionState(CollectionOptions collectionOptions_,
                    BSONObjSet indexSpecs_,
                    std::string dataHash_);

    /**
     * Compares BSON objects (BSONObj) in two sets of BSON objects (BSONObjSet) to see if the two
     * sets are equivalent.
     *
     * Two sets are equivalent if and only if their sizes are the same and all of their elements
     * that share the same index position are also equivalent in value.
     */
    bool cmpIndexSpecs(const BSONObjSet& otherSpecs) const;

    /**
     * Returns a std::string representation of the CollectionState struct of which this is a member
     * function. Returns out its representation in the form:
     *
     * Collection options: {...}; Index options: [...]; MD5 hash: <md5 digest string>
     */
    std::string toString() const;

    const CollectionOptions collectionOptions = CollectionOptions();
    const BSONObjSet indexSpecs = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    const std::string dataHash = "";
    const bool exists = false;
};

bool operator==(const CollectionState& lhs, const CollectionState& rhs);
bool operator!=(const CollectionState& lhs, const CollectionState& rhs);
std::ostream& operator<<(std::ostream& stream, const CollectionState& state);
StringBuilderImpl<SharedBufferAllocator>& operator<<(StringBuilderImpl<SharedBufferAllocator>& sb,
                                                     const CollectionState& state);

class IdempotencyTest : public SyncTailTest {
protected:
    enum class SequenceType : int { kEntireSequence, kAnyPrefix, kAnySuffix, kAnyPrefixOrSuffix };
    OplogEntry createCollection(CollectionUUID uuid = UUID::gen());
    OplogEntry dropCollection();
    OplogEntry insert(const BSONObj& obj);
    template <class IdType>
    OplogEntry update(IdType _id, const BSONObj& obj);
    OplogEntry buildIndex(const BSONObj& indexSpec, const BSONObj& options = BSONObj());
    OplogEntry dropIndex(const std::string& indexName);
    virtual Status resetState();

    /**
     * This method returns true if running the list of operations a single time is equivalent to
     * running them two times. It returns false otherwise.
     */
    void testOpsAreIdempotent(std::vector<OplogEntry> ops,
                              SequenceType sequenceType = SequenceType::kEntireSequence);

    /**
     * This function exists to work around the issue described in SERVER-30470 by providing a
     * mechanism for the RandomizedIdempotencyTest class to avoid triggering failures caused by
     * differences in the ordering of fields within a document. By default it returns the document
     * unchanged.
     */
    virtual BSONObj canonicalizeDocumentForDataHash(const BSONObj& obj) {
        return obj;
    };

    std::string computeDataHash(Collection* collection);
    virtual std::string getStateString(const CollectionState& state1,
                                       const CollectionState& state2,
                                       const std::vector<OplogEntry>& ops);
    /**
     * Validate data and indexes. Return the MD5 hash of the documents ordered by _id.
     */
    CollectionState validate();

    NamespaceString nss{"test.foo"};
    NamespaceString nssIndex{"test.system.indexes"};
};

OplogEntry makeCreateCollectionOplogEntry(OpTime opTime,
                                          const NamespaceString& nss = NamespaceString("test.t"),
                                          const BSONObj& options = BSONObj());

OplogEntry makeInsertDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToInsert);

OplogEntry makeDeleteDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToDelete);

OplogEntry makeUpdateDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToUpdate,
                                        const BSONObj& updatedDocument);

OplogEntry makeCreateIndexOplogEntry(OpTime opTime,
                                     const NamespaceString& nss,
                                     const std::string& indexName,
                                     const BSONObj& keyPattern);

OplogEntry makeCommandOplogEntry(OpTime opTime, const NamespaceString& nss, const BSONObj& command);

OplogEntry makeInsertDocumentOplogEntryWithSessionInfo(OpTime opTime,
                                                       const NamespaceString& nss,
                                                       const BSONObj& documentToInsert,
                                                       OperationSessionInfo info);

OplogEntry makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(OpTime opTime,
                                                                const NamespaceString& nss,
                                                                const BSONObj& documentToInsert,
                                                                LogicalSessionId lsid,
                                                                TxnNumber txnNum,
                                                                StmtId stmtId);

}  // namespace repl
}  // namespace mongo
