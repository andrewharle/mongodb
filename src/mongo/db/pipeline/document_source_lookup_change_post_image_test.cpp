
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

#include <boost/intrusive_ptr.hpp>
#include <deque>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
namespace {
using boost::intrusive_ptr;
using std::deque;
using std::vector;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
class DocumentSourceLookupChangePostImageTest : public AggregationContextFixture {
public:
    /**
     * This method is required to avoid a static initialization fiasco resulting from calling
     * UUID::gen() in file static scope.
     */
    static const UUID& testUuid() {
        static const UUID* uuid_gen = new UUID(UUID::gen());
        return *uuid_gen;
    }

    Document makeResumeToken(ImplicitValue id = Value()) {
        const Timestamp ts(100, 1);
        if (id.missing()) {
            ResumeTokenData tokenData;
            tokenData.clusterTime = ts;
            return ResumeToken(tokenData).toDocument(ResumeToken::SerializationFormat::kHexString);
        }
        return ResumeToken(ResumeTokenData(ts, 0, 0, testUuid(), Value(Document{{"_id", id}})))
            .toDocument(ResumeToken::SerializationFormat::kHexString);
    }
};

/**
 * A mock MongoProcessInterface which allows mocking a foreign pipeline.
 */
class MockMongoInterface final : public StubMongoProcessInterface {
public:
    MockMongoInterface(deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) final {
        return false;
    }

    StatusWith<std::unique_ptr<Pipeline, PipelineDeleter>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts = MakePipelineOptions{}) final {
        auto pipeline = Pipeline::parse(rawPipeline, expCtx);
        if (!pipeline.isOK()) {
            return pipeline.getStatus();
        }

        if (opts.optimize) {
            pipeline.getValue()->optimizePipeline();
        }

        if (opts.attachCursorSource) {
            uassertStatusOK(attachCursorSourceToPipeline(expCtx, pipeline.getValue().get()));
        }

        return pipeline;
    }

    Status attachCursorSourceToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        Pipeline* pipeline) final {
        pipeline->addInitialSource(DocumentSourceMock::create(_mockResults));
        return Status::OK();
    }

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) {
        // The namespace 'nss' may be different than the namespace on the ExpressionContext in the
        // case of a change stream on a whole database so we need to make a copy of the
        // ExpressionContext with the new namespace.
        auto foreignExpCtx = expCtx->copyWith(nss, collectionUUID, boost::none);
        auto swPipeline = makePipeline({BSON("$match" << documentKey)}, foreignExpCtx);
        if (swPipeline == ErrorCodes::NamespaceNotFound) {
            return boost::none;
        }
        auto pipeline = uassertStatusOK(std::move(swPipeline));

        auto lookedUpDocument = pipeline->getNext();
        if (auto next = pipeline->getNext()) {
            uasserted(ErrorCodes::TooManyMatchingDocuments,
                      str::stream() << "found more than one document matching "
                                    << documentKey.toString()
                                    << " ["
                                    << lookedUpDocument->toString()
                                    << ", "
                                    << next->toString()
                                    << "]");
        }
        return lookedUpDocument;
    }

private:
    deque<DocumentSource::GetNextResult> _mockResults;
};

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfMissingDocumentKeyOnUpdate) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "documentKey" field.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"_id", makeResumeToken(0)},
                 {"operationType", "update"_sd},
                 {"fullDocument", Document{{"_id", 0}}},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        stdx::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfMissingOperationType) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"fullDocument", Document{{"_id", 0}}},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        stdx::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfMissingNamespace) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::create(Document{
        {"_id", makeResumeToken(0)},
        {"documentKey", Document{{"_id", 0}}},
        {"operationType", "update"_sd},
    });

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        stdx::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfNsFieldHasWrongType) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource =
        DocumentSourceMock::create(Document{{"_id", makeResumeToken(0)},
                                            {"documentKey", Document{{"_id", 0}}},
                                            {"operationType", "update"_sd},
                                            {"ns", 4}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        stdx::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40578);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfNsFieldDoesNotMatchPipeline) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", "DIFFERENT"_sd}, {"coll", expCtx->ns.coll()}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        stdx::make_unique<MockMongoInterface>(deque<DocumentSource::GetNextResult>{});

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40579);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfDatabaseMismatchOnCollectionlessNss) {
    auto expCtx = getExpCtx();

    expCtx->ns = NamespaceString::makeCollectionlessAggregateNSS("test");

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with a document without a "ns" field.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", "DIFFERENT"_sd}, {"coll", "irrelevant"_sd}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->mongoProcessInterface = stdx::make_unique<MockMongoInterface>(mockForeignContents);

    ASSERT_THROWS_CODE(lookupChangeStage->getNext(), AssertionException, 40579);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldPassIfDatabaseMatchesOnCollectionlessNss) {
    auto expCtx = getExpCtx();

    expCtx->ns = NamespaceString::makeCollectionlessAggregateNSS("test");

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}}};
    expCtx->mongoProcessInterface = stdx::make_unique<MockMongoInterface>(mockForeignContents);

    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", "irrelevant"_sd}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"_id", makeResumeToken(0)},
                  {"documentKey", Document{{"_id", 0}}},
                  {"operationType", "update"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", "irrelevant"_sd}}},
                  {"fullDocument", Document{{"_id", 0}}}}));
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldErrorIfDocumentKeyIsNotUnique) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input with an update document.
    auto mockLocalSource = DocumentSourceMock::create(
        Document{{"_id", makeResumeToken(0)},
                 {"documentKey", Document{{"_id", 0}}},
                 {"operationType", "update"_sd},
                 {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection to have two documents with the same document key.
    deque<DocumentSource::GetNextResult> foreignCollection = {Document{{"_id", 0}},
                                                              Document{{"_id", 0}}};
    getExpCtx()->mongoProcessInterface =
        stdx::make_unique<MockMongoInterface>(std::move(foreignCollection));

    ASSERT_THROWS_CODE(
        lookupChangeStage->getNext(), AssertionException, ErrorCodes::TooManyMatchingDocuments);
}

TEST_F(DocumentSourceLookupChangePostImageTest, ShouldPropagatePauses) {
    auto expCtx = getExpCtx();

    // Set up the lookup change post image stage.
    auto lookupChangeStage = DocumentSourceLookupChangePostImage::create(expCtx);

    // Mock its input, pausing every other result.
    auto mockLocalSource = DocumentSourceMock::create(
        {Document{{"_id", makeResumeToken(0)},
                  {"documentKey", Document{{"_id", 0}}},
                  {"operationType", "insert"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}},
                  {"fullDocument", Document{{"_id", 0}}}},
         DocumentSource::GetNextResult::makePauseExecution(),
         Document{{"_id", makeResumeToken(1)},
                  {"documentKey", Document{{"_id", 1}}},
                  {"operationType", "update"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}}},
         DocumentSource::GetNextResult::makePauseExecution()});

    lookupChangeStage->setSource(mockLocalSource.get());

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                             Document{{"_id", 1}}};
    getExpCtx()->mongoProcessInterface =
        stdx::make_unique<MockMongoInterface>(std::move(mockForeignContents));

    auto next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"_id", makeResumeToken(0)},
                  {"documentKey", Document{{"_id", 0}}},
                  {"operationType", "insert"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}},
                  {"fullDocument", Document{{"_id", 0}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    next = lookupChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"_id", makeResumeToken(1)},
                  {"documentKey", Document{{"_id", 1}}},
                  {"operationType", "update"_sd},
                  {"ns", Document{{"db", expCtx->ns.db()}, {"coll", expCtx->ns.coll()}}},
                  {"fullDocument", Document{{"_id", 1}}}}));

    ASSERT_TRUE(lookupChangeStage->getNext().isPaused());

    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
    ASSERT_TRUE(lookupChangeStage->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
