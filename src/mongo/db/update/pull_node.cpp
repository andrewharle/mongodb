
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

#include "mongo/db/update/pull_node.h"

#include "mongo/db/matcher/copyable_match_expression.h"
#include "mongo/db/query/collation/collator_interface.h"

namespace mongo {

/**
 * The ObjectMatcher is used when the $pull condition is specified as an object and the first field
 * of that object is not an operator (like $gt).
 */
class PullNode::ObjectMatcher final : public PullNode::ElementMatcher {
public:
    ObjectMatcher(BSONObj matchCondition, const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : _matchExpr(matchCondition,
                     expCtx,
                     stdx::make_unique<ExtensionsCallbackNoop>(),
                     MatchExpressionParser::kBanAllSpecialFeatures) {}

    std::unique_ptr<ElementMatcher> clone() const final {
        return stdx::make_unique<ObjectMatcher>(*this);
    }

    bool match(const mutablebson::ConstElement& element) final {
        if (element.getType() == mongo::Object) {
            return _matchExpr->matchesBSON(element.getValueObject());
        } else {
            return false;
        }
    }

    void setCollator(const CollatorInterface* collator) final {
        _matchExpr.setCollator(collator);
    }

private:
    CopyableMatchExpression _matchExpr;
};

/**
 * The WrappedObjectMatcher is used when the condition is a regex or an object with an operator as
 * its first field (e.g., {$gt: ...}). It is possible that the element we want to compare is not an
 * object, so we wrap it in an object before comparing it. We also wrap the MatchExpression in an
 * empty object so that we are comparing the MatchCondition and the array element at the same level.
 * This hack allows us to use a MatchExpression to check a BSONElement.
 */
class PullNode::WrappedObjectMatcher final : public PullNode::ElementMatcher {
public:
    WrappedObjectMatcher(BSONElement matchCondition,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : _matchExpr(matchCondition.wrap(""),
                     expCtx,
                     stdx::make_unique<ExtensionsCallbackNoop>(),
                     MatchExpressionParser::kBanAllSpecialFeatures) {}

    std::unique_ptr<ElementMatcher> clone() const final {
        return stdx::make_unique<WrappedObjectMatcher>(*this);
    }

    bool match(const mutablebson::ConstElement& element) final {
        BSONObj candidate = element.getValue().wrap("");
        return _matchExpr->matchesBSON(candidate);
    }

    void setCollator(const CollatorInterface* collator) final {
        _matchExpr.setCollator(collator);
    }

private:
    CopyableMatchExpression _matchExpr;
};

/**
 * The EqualityMatcher is used when the condition is a primitive value or an array value. We require
 * an exact match.
 */
class PullNode::EqualityMatcher final : public PullNode::ElementMatcher {
public:
    EqualityMatcher(BSONElement modExpr, const CollatorInterface* collator)
        : _modExpr(modExpr), _collator(collator) {}

    std::unique_ptr<ElementMatcher> clone() const final {
        return stdx::make_unique<EqualityMatcher>(*this);
    }

    bool match(const mutablebson::ConstElement& element) final {
        return (element.compareWithBSONElement(_modExpr, _collator, false) == 0);
    }

    void setCollator(const CollatorInterface* collator) final {
        _collator = collator;
    }

private:
    BSONElement _modExpr;
    const CollatorInterface* _collator;
};

Status PullNode::init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());

    try {
        if (modExpr.type() == mongo::Object &&
            !MatchExpressionParser::parsePathAcceptingKeyword(
                modExpr.embeddedObject().firstElement())) {
            _matcher = stdx::make_unique<ObjectMatcher>(modExpr.embeddedObject(), expCtx);
        } else if (modExpr.type() == mongo::Object || modExpr.type() == mongo::RegEx) {
            _matcher = stdx::make_unique<WrappedObjectMatcher>(modExpr, expCtx);
        } else {
            _matcher = stdx::make_unique<EqualityMatcher>(modExpr, expCtx->getCollator());
        }
    } catch (AssertionException& exception) {
        return exception.toStatus();
    }

    return Status::OK();
}

}  // namespace mongo
