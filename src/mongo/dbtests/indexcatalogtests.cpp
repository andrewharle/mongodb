// indexcatalogtests.cpp : index_catalog.{h,cpp} unit tests.

/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/db.h"
#include "mongo/db/index/index_descriptor.h"

#include "mongo/dbtests/dbtests.h"

namespace IndexCatalogTests {

    static const char* const _ns = "unittests.indexcatalog";

    class IndexIteratorTests {
    public:
        IndexIteratorTests() {
            Client::WriteContext ctx(_ns);
            _db = ctx.ctx().db();
            _coll = _db->createCollection(_ns);
            _catalog = _coll->getIndexCatalog();
        }

        ~IndexIteratorTests() {
            Client::WriteContext ctx(_ns);
            _db->dropCollection(_ns);
        }

        void run() {
            Client::WriteContext ctx(_ns);
            int numFinishedIndexesStart = _catalog->numIndexesReady();

            BSONObjBuilder b1;
            b1.append("key", BSON("x" << 1));
            b1.append("ns", _ns);
            b1.append("name", "_x_0");
            _catalog->createIndex(b1.obj(), true);

            BSONObjBuilder b2;
            b2.append("key", BSON("y" << 1));
            b2.append("ns", _ns);
            b2.append("name", "_y_0");
            _catalog->createIndex(b2.obj(), true);

            ASSERT_TRUE(_catalog->numIndexesReady() == numFinishedIndexesStart+2);

            IndexCatalog::IndexIterator ii = _catalog->getIndexIterator(false);
            int indexesIterated = 0;
            bool foundIndex = false;
            while (ii.more()) {
                IndexDescriptor* indexDesc = ii.next();
                indexesIterated++;
                BSONObjIterator boit(indexDesc->infoObj());
                while (boit.more() && !foundIndex) {
                    BSONElement e = boit.next();
                    if (str::equals(e.fieldName(), "name") &&
                            str::equals(e.valuestrsafe(), "_y_0")) {
                        foundIndex = true;
                        break;
                    }
                }
            }

            ASSERT_TRUE(indexesIterated == _catalog->numIndexesReady());
            ASSERT_TRUE(foundIndex);
        }

    private:
        IndexCatalog* _catalog;
        Collection* _coll;
        Database* _db;
    };

    /**
     * Test for IndexCatalog::updateTTLSetting().
     */
    class UpdateTTLSetting {
    public:
        UpdateTTLSetting() {
            Client::WriteContext ctx(_ns);
            _db = ctx.ctx().db();
            _coll = _db->createCollection(_ns);
            _catalog = _coll->getIndexCatalog();
        }

        ~UpdateTTLSetting () {
            Client::WriteContext ctx(_ns);
            _db->dropCollection(_ns);
        }

        void run() {
            Client::WriteContext ctx(_ns);
            const std::string indexName = "x_1";

            BSONObjBuilder indexSpecBuilder;
            indexSpecBuilder.append("key", BSON("x" << 1));
            indexSpecBuilder.append("ns", _ns);
            indexSpecBuilder.append("name", indexName);
            indexSpecBuilder.append("expireAfterSeconds", 5);
            _catalog->createIndex(indexSpecBuilder.obj(), true);

            const IndexDescriptor* desc = _catalog->findIndexByName(indexName);
            ASSERT(desc);
            ASSERT_EQUALS(5, desc->infoObj()["expireAfterSeconds"].numberLong());

            desc = _catalog->updateTTLSetting(desc, 10);

            ASSERT_EQUALS(10, desc->infoObj()["expireAfterSeconds"].numberLong());
        }

    private:
        IndexCatalog* _catalog;
        Collection* _coll;
        Database* _db;
    };

    class IndexCatalogTests : public Suite {
    public:
        IndexCatalogTests() : Suite( "indexcatalogtests" ) {
        }
        void setupTests() {
            add<IndexIteratorTests>();
            add<UpdateTTLSetting>();
        }
    } indexCatalogTests;
}
