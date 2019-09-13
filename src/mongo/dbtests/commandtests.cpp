
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

#include <unordered_set>

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/rpc/op_msg.h"

using namespace mongo;

namespace CommandTests {

TEST(CommandTests, InputDocumentSequeceWorksEndToEnd) {
    const auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    NamespaceString ns("test", "doc_seq");
    DBDirectClient db(opCtx);
    db.dropCollection(ns.ns());
    ASSERT_EQ(db.count(ns.ns()), 0u);

    OpMsgRequest request;
    request.body = BSON("insert" << ns.coll() << "$db" << ns.db());
    request.sequences = {{"documents",
                          {
                              BSON("_id" << 1),
                              BSON("_id" << 2),
                              BSON("_id" << 3),
                              BSON("_id" << 4),
                              BSON("_id" << 5),
                          }}};

    const auto reply = db.runCommand(std::move(request));
    ASSERT_EQ(int(reply->getProtocol()), int(rpc::Protocol::kOpMsg));
    ASSERT_BSONOBJ_EQ(reply->getCommandReply(), BSON("n" << 5 << "ok" << 1.0));
    ASSERT_EQ(db.count(ns.ns()), 5u);
}

using std::string;

/**
 * Default suite base, unless otherwise overridden in test specific namespace.
 */
class Base {
public:
    Base() : db(&_opCtx) {
        db.dropCollection(ns());
    }

    const char* ns() {
        return "test.testCollection";
    }
    const char* nsDb() {
        return "test";
    }
    const char* nsColl() {
        return "testCollection";
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    DBDirectClient db;
};

// one namespace per command
namespace FileMD5 {
struct Base {
    Base() : db(&_opCtx) {
        db.dropCollection(ns());
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns(), BSON("files_id" << 1 << "n" << 1)));
    }

    const char* ns() {
        return "test.fs.chunks";
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    DBDirectClient db;
};
struct Type0 : Base {
    void run() {
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("files_id", 0);
            b.append("n", 0);
            b.appendBinData("data", 6, BinDataGeneral, "hello ");
            db.insert(ns(), b.obj());
        }
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("files_id", 0);
            b.append("n", 1);
            b.appendBinData("data", 5, BinDataGeneral, "world");
            db.insert(ns(), b.obj());
        }

        BSONObj result;
        ASSERT(db.runCommand("test", BSON("filemd5" << 0), result));
        ASSERT_EQUALS(string("5eb63bbbe01eeed093cb22bb8f5acdc3"), result["md5"].valuestr());
    }
};
struct Type2 : Base {
    void run() {
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("files_id", 0);
            b.append("n", 0);
            b.appendBinDataArrayDeprecated("data", "hello ", 6);
            db.insert(ns(), b.obj());
        }
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("files_id", 0);
            b.append("n", 1);
            b.appendBinDataArrayDeprecated("data", "world", 5);
            db.insert(ns(), b.obj());
        }

        BSONObj result;
        ASSERT(db.runCommand("test", BSON("filemd5" << 0), result));
        ASSERT_EQUALS(string("5eb63bbbe01eeed093cb22bb8f5acdc3"), result["md5"].valuestr());
    }
};
}

namespace SymbolArgument {
// SERVER-16260
// The Ruby driver expects server commands to accept the Symbol BSON type as a collection name.
// This is a historical quirk that we shall support until corrected versions of the Ruby driver
// can be distributed. Retain these tests until MongoDB 3.0

class Drop : Base {
public:
    void run() {
        ASSERT(db.createCollection(ns()));
        {
            BSONObjBuilder cmd;
            cmd.appendSymbol("drop", nsColl());  // Use Symbol for SERVER-16260

            BSONObj result;
            bool ok = db.runCommand(nsDb(), cmd.obj(), result);
            log() << result.jsonString();
            ASSERT(ok);
        }
    }
};

class DropIndexes : Base {
public:
    void run() {
        ASSERT(db.createCollection(ns()));

        BSONObjBuilder cmd;
        cmd.appendSymbol("dropIndexes", nsColl());  // Use Symbol for SERVER-16260
        cmd.append("index", "*");

        BSONObj result;
        bool ok = db.runCommand(nsDb(), cmd.obj(), result);
        log() << result.jsonString();
        ASSERT(ok);
    }
};

class CreateIndexWithNoKey : Base {
public:
    void run() {
        ASSERT(db.createCollection(ns()));

        BSONObjBuilder indexSpec;

        BSONArrayBuilder indexes;
        indexes.append(indexSpec.obj());

        BSONObjBuilder cmd;
        cmd.append("createIndexes", nsColl());
        cmd.append("indexes", indexes.arr());

        BSONObj result;
        bool ok = db.runCommand(nsDb(), cmd.obj(), result);
        log() << result.jsonString();
        ASSERT(!ok);
    }
};

class CreateIndexWithDuplicateKey : Base {
public:
    void run() {
        ASSERT(db.createCollection(ns()));

        BSONObjBuilder indexSpec;
        indexSpec.append("key", BSON("a" << 1 << "a" << 1 << "b" << 1));

        BSONArrayBuilder indexes;
        indexes.append(indexSpec.obj());

        BSONObjBuilder cmd;
        cmd.append("createIndexes", nsColl());
        cmd.append("indexes", indexes.arr());

        BSONObj result;
        bool ok = db.runCommand(nsDb(), cmd.obj(), result);
        log() << result.jsonString();
        ASSERT(!ok);
    }
};

class FindAndModify : Base {
public:
    void run() {
        ASSERT(db.createCollection(ns()));
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("name", "Tom");
            b.append("rating", 0);
            db.insert(ns(), b.obj());
        }

        BSONObjBuilder cmd;
        cmd.appendSymbol("findAndModify", nsColl());  // Use Symbol for SERVER-16260
        cmd.append("update", BSON("$inc" << BSON("score" << 1)));
        cmd.append("new", true);

        BSONObj result;
        bool ok = db.runCommand(nsDb(), cmd.obj(), result);
        log() << result.jsonString();
        ASSERT(ok);
        // TODO(kangas) test that Tom's score is 1
    }
};

class GeoSearch : Base {
public:
    void run() {
        // Subset of geo_haystack1.js

        int n = 0;
        for (int x = 0; x < 20; x++) {
            for (int y = 0; y < 20; y++) {
                db.insert(ns(), BSON("_id" << n << "loc" << BSON_ARRAY(x << y) << "z" << n % 5));
                n++;
            }
        }

        // Build geoHaystack index. Can's use db.ensureIndex, no way to pass "bucketSize".
        // So run createIndexes command instead.
        //
        // Shell example:
        // t.ensureIndex( { loc : "geoHaystack" , z : 1 }, { bucketSize : .7 } );

        {
            BSONObjBuilder cmd;
            cmd.append("createIndexes", nsColl());
            cmd.append("indexes",
                       BSON_ARRAY(BSON("key" << BSON("loc"
                                                     << "geoHaystack"
                                                     << "z"
                                                     << 1.0)
                                             << "name"
                                             << "loc_geoHaystack_z_1"
                                             << "bucketSize"
                                             << static_cast<double>(0.7))));

            BSONObj result;
            ASSERT(db.runCommand(nsDb(), cmd.obj(), result));
        }

        {
            BSONObjBuilder cmd;
            cmd.appendSymbol("geoSearch", nsColl());  // Use Symbol for SERVER-16260
            cmd.append("near", BSON_ARRAY(7 << 8));
            cmd.append("maxDistance", 3);
            cmd.append("search", BSON("z" << 3));

            BSONObj result;
            bool ok = db.runCommand(nsDb(), cmd.obj(), result);
            log() << result.jsonString();
            ASSERT(ok);
        }
    }
};

class Touch : Base {
public:
    void run() {
        ASSERT(db.createCollection(ns()));
        {
            BSONObjBuilder cmd;
            cmd.appendSymbol("touch", nsColl());  // Use Symbol for SERVER-16260
            cmd.append("data", true);
            cmd.append("index", true);

            BSONObj result;
            bool ok = db.runCommand(nsDb(), cmd.obj(), result);
            log() << result.jsonString();
            ASSERT(ok || result["code"].Int() == ErrorCodes::CommandNotSupported);
        }
    }
};

}  // namespace SymbolArgument

/**
 * Tests that the 'rolesInfo' command does not return duplicate field names.
 */
class RolesInfoShouldNotReturnDuplicateFieldNames : Base {
public:
    void run() {
        BSONObj result;
        bool ok = db.runCommand(nsDb(), BSON("rolesInfo" << 1), result);
        ASSERT(ok);

        stdx::unordered_set<std::string> observedFields;
        for (const auto& field : result) {
            ASSERT(observedFields.find(field) == observedFields.end());
            observedFields.insert(field);
        }
    }
};

class All : public Suite {
public:
    All() : Suite("commands") {}

    void setupTests() {
        add<FileMD5::Type0>();
        add<FileMD5::Type2>();
        add<FileMD5::Type2>();
        add<SymbolArgument::DropIndexes>();
        add<SymbolArgument::FindAndModify>();
        add<SymbolArgument::Touch>();
        add<SymbolArgument::Drop>();
        add<SymbolArgument::GeoSearch>();
        add<SymbolArgument::CreateIndexWithNoKey>();
        add<SymbolArgument::CreateIndexWithDuplicateKey>();
        add<RolesInfoShouldNotReturnDuplicateFieldNames>();
    }
};

SuiteInstance<All> all;
}
