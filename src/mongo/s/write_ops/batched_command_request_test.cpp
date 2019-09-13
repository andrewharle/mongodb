
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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/ops/write_ops_parsers_test_helpers.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(BatchedCommandRequest, BasicInsert) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    BSONObj origInsertRequestObj = BSON("insert"
                                        << "test"
                                        << "documents"
                                        << insertArray
                                        << "writeConcern"
                                        << BSON("w" << 1)
                                        << "ordered"
                                        << true
                                        << "allowImplicitCollectionCreation"
                                        << false);

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));
        const auto insertRequest(BatchedCommandRequest::parseInsert(opMsgRequest));

        ASSERT_EQ("TestDB.test", insertRequest.getInsertRequest().getNamespace().ns());
        ASSERT(!insertRequest.hasShardVersion());
        ASSERT_FALSE(insertRequest.isImplicitCreateAllowed());
    }
}

TEST(BatchedCommandRequest, InsertWithShardVersion) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    const OID epoch = OID::gen();

    BSONObj origInsertRequestObj = BSON("insert"
                                        << "test"
                                        << "documents"
                                        << insertArray
                                        << "writeConcern"
                                        << BSON("w" << 1)
                                        << "ordered"
                                        << true
                                        << "shardVersion"
                                        << BSON_ARRAY(Timestamp(1, 2) << epoch));

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));
        const auto insertRequest(BatchedCommandRequest::parseInsert(opMsgRequest));

        ASSERT_EQ("TestDB.test", insertRequest.getInsertRequest().getNamespace().ns());
        ASSERT(insertRequest.hasShardVersion());
        ASSERT_EQ(ChunkVersion(1, 2, epoch).toString(), insertRequest.getShardVersion().toString());
    }
}

TEST(BatchedCommandRequest, InsertCloneWithIds) {
    BatchedCommandRequest batchedRequest([&] {
        write_ops::Insert insertOp(NamespaceString("xyz.abc"));
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(true);
            wcb.setBypassDocumentValidation(true);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    batchedRequest.setWriteConcern(BSON("w" << 2));

    const auto clonedRequest(BatchedCommandRequest::cloneInsertWithIds(std::move(batchedRequest)));

    ASSERT_EQ("xyz.abc", clonedRequest.getNS().ns());
    ASSERT_EQ("xyz.abc", clonedRequest.getTargetingNS().ns());
    ASSERT(clonedRequest.getWriteCommandBase().getOrdered());
    ASSERT(clonedRequest.getWriteCommandBase().getBypassDocumentValidation());
    ASSERT_BSONOBJ_EQ(BSON("w" << 2), clonedRequest.getWriteConcern());

    const auto& insertDocs = clonedRequest.getInsertRequest().getDocuments();
    ASSERT_EQ(2u, insertDocs.size());

    ASSERT_EQ(jstOID, insertDocs[0]["_id"].type());
    ASSERT_EQ(1, insertDocs[0]["x"].numberLong());

    ASSERT_EQ(jstOID, insertDocs[1]["_id"].type());
    ASSERT_EQ(2, insertDocs[1]["x"].numberLong());
}

TEST(BatchedCommandRequest, IndexInsertCloneWithIds) {
    const auto indexSpec = BSON("v" << 1 << "key" << BSON("x" << -1) << "name"
                                    << "Test index"
                                    << "ns"
                                    << "xyz.abc");

    BatchedCommandRequest batchedRequest([&] {
        write_ops::Insert insertOp(NamespaceString("xyz.system.indexes"));
        insertOp.setDocuments({indexSpec});
        return insertOp;
    }());
    batchedRequest.setWriteConcern(BSON("w" << 2));

    const auto clonedRequest(BatchedCommandRequest::cloneInsertWithIds(std::move(batchedRequest)));

    ASSERT_EQ("xyz.system.indexes", clonedRequest.getNS().ns());
    ASSERT_EQ("xyz.abc", clonedRequest.getTargetingNS().ns());
    ASSERT(clonedRequest.getWriteCommandBase().getOrdered());
    ASSERT(!clonedRequest.getWriteCommandBase().getBypassDocumentValidation());
    ASSERT_BSONOBJ_EQ(BSON("w" << 2), clonedRequest.getWriteConcern());

    const auto& insertDocs = clonedRequest.getInsertRequest().getDocuments();
    ASSERT_EQ(1u, insertDocs.size());

    ASSERT_BSONOBJ_EQ(indexSpec, insertDocs[0]);
}

}  // namespace
}  // namespace mongo
