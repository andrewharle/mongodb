// matchertests.cpp : matcher unit tests
//


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

#include <iostream>

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/timer.h"

namespace MatcherTests {

using std::cout;
using std::endl;
using std::string;

class CollectionBase {
public:
    CollectionBase() {}

    virtual ~CollectionBase() {}
};

template <typename M>
class Basic {
public:
    void run() {
        BSONObj query = fromjson("{\"a\":\"b\"}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query, expCtx);
        ASSERT(m.matches(fromjson("{\"a\":\"b\"}")));
    }
};

template <typename M>
class DoubleEqual {
public:
    void run() {
        BSONObj query = fromjson("{\"a\":5}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query, expCtx);
        ASSERT(m.matches(fromjson("{\"a\":5}")));
    }
};

template <typename M>
class MixedNumericEqual {
public:
    void run() {
        BSONObjBuilder query;
        query.append("a", 5);
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query.done(), expCtx);
        ASSERT(m.matches(fromjson("{\"a\":5}")));
    }
};

template <typename M>
class MixedNumericGt {
public:
    void run() {
        BSONObj query = fromjson("{\"a\":{\"$gt\":4}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query, expCtx);
        BSONObjBuilder b;
        b.append("a", 5);
        ASSERT(m.matches(b.done()));
    }
};

template <typename M>
class MixedNumericIN {
public:
    void run() {
        BSONObj query = fromjson("{ a : { $in : [4,6] } }");
        ASSERT_EQUALS(4, query["a"].embeddedObject()["$in"].embeddedObject()["0"].number());
        ASSERT_EQUALS(NumberInt, query["a"].embeddedObject()["$in"].embeddedObject()["0"].type());

        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query, expCtx);

        {
            BSONObjBuilder b;
            b.append("a", 4.0);
            ASSERT(m.matches(b.done()));
        }

        {
            BSONObjBuilder b;
            b.append("a", 5);
            ASSERT(!m.matches(b.done()));
        }


        {
            BSONObjBuilder b;
            b.append("a", 4);
            ASSERT(m.matches(b.done()));
        }
    }
};

template <typename M>
class MixedNumericEmbedded {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(BSON("a" << BSON("x" << 1)), expCtx);
        ASSERT(m.matches(BSON("a" << BSON("x" << 1))));
        ASSERT(m.matches(BSON("a" << BSON("x" << 1.0))));
    }
};

template <typename M>
class Size {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(fromjson("{a:{$size:4}}"), expCtx);
        ASSERT(m.matches(fromjson("{a:[1,2,3,4]}")));
        ASSERT(!m.matches(fromjson("{a:[1,2,3]}")));
        ASSERT(!m.matches(fromjson("{a:[1,2,3,'a','b']}")));
        ASSERT(!m.matches(fromjson("{a:[[1,2,3,4]]}")));
    }
};

template <typename M>
class WithinBox {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(fromjson("{loc:{$within:{$box:[{x: 4, y:4},[6,6]]}}}"), expCtx);
        ASSERT(!m.matches(fromjson("{loc: [3,4]}")));
        ASSERT(m.matches(fromjson("{loc: [4,4]}")));
        ASSERT(m.matches(fromjson("{loc: [5,5]}")));
        ASSERT(m.matches(fromjson("{loc: [5,5.1]}")));
        ASSERT(m.matches(fromjson("{loc: {x: 5, y:5.1}}")));
    }
};

template <typename M>
class WithinPolygon {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(fromjson("{loc:{$within:{$polygon:[{x:0,y:0},[0,5],[5,5],[5,0]]}}}"), expCtx);
        ASSERT(m.matches(fromjson("{loc: [3,4]}")));
        ASSERT(m.matches(fromjson("{loc: [4,4]}")));
        ASSERT(m.matches(fromjson("{loc: {x:5,y:5}}")));
        ASSERT(!m.matches(fromjson("{loc: [5,5.1]}")));
        ASSERT(!m.matches(fromjson("{loc: {}}")));
    }
};

template <typename M>
class WithinCenter {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(fromjson("{loc:{$within:{$center:[{x:30,y:30},10]}}}"), expCtx);
        ASSERT(!m.matches(fromjson("{loc: [3,4]}")));
        ASSERT(m.matches(fromjson("{loc: {x:30,y:30}}")));
        ASSERT(m.matches(fromjson("{loc: [20,30]}")));
        ASSERT(m.matches(fromjson("{loc: [30,20]}")));
        ASSERT(m.matches(fromjson("{loc: [40,30]}")));
        ASSERT(m.matches(fromjson("{loc: [30,40]}")));
        ASSERT(!m.matches(fromjson("{loc: [31,40]}")));
    }
};

/** Test that MatchDetails::elemMatchKey() is set correctly after a match. */
template <typename M>
class ElemMatchKey {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M matcher(BSON("a.b" << 1), expCtx);
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT(!details.hasElemMatchKey());
        ASSERT(matcher.matches(fromjson("{ a:[ { b:1 } ] }"), &details));
        // The '0' entry of the 'a' array is matched.
        ASSERT(details.hasElemMatchKey());
        ASSERT_EQUALS(string("0"), details.elemMatchKey());
    }
};

template <typename M>
class WhereSimple1 {
public:
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        const NamespaceString nss("unittests.matchertests");
        AutoGetCollectionForReadCommand ctx(&opCtx, nss);

        const CollatorInterface* collator = nullptr;
        const boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(opCtxPtr.get(), collator));
        M m(BSON("$where"
                 << "function(){ return this.a == 1; }"),
            expCtx,
            ExtensionsCallbackReal(&opCtx, &nss),
            MatchExpressionParser::AllowedFeatures::kJavascript);
        ASSERT(m.matches(BSON("a" << 1)));
        ASSERT(!m.matches(BSON("a" << 2)));
    }
};

template <typename M>
class TimingBase {
public:
    long dotime(const BSONObj& patt, const BSONObj& obj) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(patt, expCtx);
        Timer t;
        for (int i = 0; i < 900000; i++) {
            if (!m.matches(obj)) {
                ASSERT(0);
            }
        }
        return t.millis();
    }
};

template <typename M>
class AllTiming : public TimingBase<M> {
public:
    void run() {
        long normal = TimingBase<M>::dotime(BSON("x" << 5), BSON("x" << 5));

        long all =
            TimingBase<M>::dotime(BSON("x" << BSON("$all" << BSON_ARRAY(5))), BSON("x" << 5));

        cout << "AllTiming " << demangleName(typeid(M)) << " normal: " << normal << " all: " << all
             << endl;
    }
};

/** Test that 'collator' is passed to MatchExpressionParser::parse(). */
template <typename M>
class NullCollator {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M matcher(BSON("a"
                       << "string"),
                  expCtx);
        ASSERT(!matcher.matches(BSON("a"
                                     << "string2")));
    }
};

/** Test that 'collator' is passed to MatchExpressionParser::parse(). */
template <typename M>
class Collator {
public:
    void run() {
        CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        expCtx->setCollator(&collator);
        M matcher(BSON("a"
                       << "string"),
                  expCtx);
        ASSERT(matcher.matches(BSON("a"
                                    << "string2")));
    }
};

class All : public Suite {
public:
    All() : Suite("matcher") {}

#define ADD_BOTH(TEST) add<TEST<Matcher>>();

    void setupTests() {
        ADD_BOTH(Basic);
        ADD_BOTH(DoubleEqual);
        ADD_BOTH(MixedNumericEqual);
        ADD_BOTH(MixedNumericGt);
        ADD_BOTH(MixedNumericIN);
        ADD_BOTH(Size);
        ADD_BOTH(MixedNumericEmbedded);
        ADD_BOTH(ElemMatchKey);
        ADD_BOTH(WhereSimple1);
        ADD_BOTH(AllTiming);
        ADD_BOTH(WithinBox);
        ADD_BOTH(WithinCenter);
        ADD_BOTH(WithinPolygon);
        ADD_BOTH(NullCollator);
        ADD_BOTH(Collator);
    }
};

SuiteInstance<All> dball;

}  // namespace MatcherTests
