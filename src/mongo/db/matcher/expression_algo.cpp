// expression_algo.cpp

/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"

namespace mongo {
namespace {

bool isComparisonMatchExpression(const MatchExpression* expr) {
    switch (expr->matchType()) {
        case MatchExpression::LT:
        case MatchExpression::LTE:
        case MatchExpression::EQ:
        case MatchExpression::GTE:
        case MatchExpression::GT:
            return true;
        default:
            return false;
    }
}

bool supportsEquality(const ComparisonMatchExpression* expr) {
    switch (expr->matchType()) {
        case MatchExpression::LTE:
        case MatchExpression::EQ:
        case MatchExpression::GTE:
            return true;
        default:
            return false;
    }
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 */
bool _isSubsetOf(const ComparisonMatchExpression* lhs, const ComparisonMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    const BSONElement lhsData = lhs->getData();
    const BSONElement rhsData = rhs->getData();

    if (lhsData.canonicalType() != rhsData.canonicalType()) {
        return false;
    }

    // Special case the handling for NaN values: NaN compares equal only to itself.
    if (std::isnan(lhsData.numberDouble()) || std::isnan(rhsData.numberDouble())) {
        if (supportsEquality(lhs) && supportsEquality(rhs)) {
            return std::isnan(lhsData.numberDouble()) && std::isnan(rhsData.numberDouble());
        }
        return false;
    }

    int cmp = compareElementValues(lhsData, rhsData);

    // Check whether the two expressions are equivalent.
    if (lhs->matchType() == rhs->matchType() && cmp == 0) {
        return true;
    }

    switch (rhs->matchType()) {
        case MatchExpression::LT:
        case MatchExpression::LTE:
            switch (lhs->matchType()) {
                case MatchExpression::LT:
                case MatchExpression::LTE:
                case MatchExpression::EQ:
                    if (rhs->matchType() == MatchExpression::LTE) {
                        return cmp <= 0;
                    }
                    return cmp < 0;
                default:
                    return false;
            }
        case MatchExpression::GT:
        case MatchExpression::GTE:
            switch (lhs->matchType()) {
                case MatchExpression::GT:
                case MatchExpression::GTE:
                case MatchExpression::EQ:
                    if (rhs->matchType() == MatchExpression::GTE) {
                        return cmp >= 0;
                    }
                    return cmp > 0;
                default:
                    return false;
            }
        default:
            return false;
    }
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 */
bool _isSubsetOf(const MatchExpression* lhs, const ComparisonMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    if (isComparisonMatchExpression(lhs)) {
        return _isSubsetOf(static_cast<const ComparisonMatchExpression*>(lhs), rhs);
    }

    if (lhs->matchType() == MatchExpression::MATCH_IN) {
        const InMatchExpression* ime = static_cast<const InMatchExpression*>(lhs);
        const ArrayFilterEntries& arrayEntries = ime->getData();
        if (arrayEntries.numRegexes() > 0) {
            return false;
        }
        for (BSONElement elem : arrayEntries.equalities()) {
            // Each element in the $in-array represents an equality predicate.
            EqualityMatchExpression equality;
            equality.init(lhs->path(), elem);
            if (!_isSubsetOf(&equality, rhs)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 */
bool _isSubsetOf(const MatchExpression* lhs, const ExistsMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field. Defer checking the path for $not expressions until the
    // subexpression is examined.
    if (lhs->matchType() != MatchExpression::NOT && lhs->path() != rhs->path()) {
        return false;
    }

    if (isComparisonMatchExpression(lhs)) {
        const ComparisonMatchExpression* cme = static_cast<const ComparisonMatchExpression*>(lhs);
        // CompareMatchExpression::init() prohibits creating a match expression with EOO or
        // Undefined types, so only need to ensure that the value is not of type jstNULL.
        return cme->getData().type() != jstNULL;
    }

    switch (lhs->matchType()) {
        case MatchExpression::ELEM_MATCH_VALUE:
        case MatchExpression::ELEM_MATCH_OBJECT:
        case MatchExpression::EXISTS:
        case MatchExpression::GEO:
        case MatchExpression::MOD:
        case MatchExpression::REGEX:
        case MatchExpression::SIZE:
        case MatchExpression::TYPE_OPERATOR:
            return true;
        case MatchExpression::MATCH_IN: {
            const InMatchExpression* ime = static_cast<const InMatchExpression*>(lhs);
            return !ime->getData().hasNull();
        }
        case MatchExpression::NOT:
            // An expression can only match a subset of the documents matched by another if they are
            // comparing the same field.
            if (lhs->getChild(0)->path() != rhs->path()) {
                return false;
            }

            switch (lhs->getChild(0)->matchType()) {
                case MatchExpression::EQ: {
                    const ComparisonMatchExpression* cme =
                        static_cast<const ComparisonMatchExpression*>(lhs->getChild(0));
                    return cme->getData().type() == jstNULL;
                }
                case MatchExpression::MATCH_IN: {
                    const InMatchExpression* ime =
                        static_cast<const InMatchExpression*>(lhs->getChild(0));
                    return ime->getData().hasNull();
                }
                default:
                    return false;
            }
        default:
            return false;
    }
}

}  // namespace

namespace expression {

bool isSubsetOf(const MatchExpression* lhs, const MatchExpression* rhs) {
    invariant(lhs);
    invariant(rhs);

    if (lhs->equivalent(rhs)) {
        return true;
    }

    if (rhs->matchType() == MatchExpression::AND) {
        // 'lhs' must match a subset of the documents matched by each clause of 'rhs'.
        for (size_t i = 0; i < rhs->numChildren(); i++) {
            if (!isSubsetOf(lhs, rhs->getChild(i))) {
                return false;
            }
        }
        return true;
    }

    if (lhs->matchType() == MatchExpression::AND) {
        // At least one clause of 'lhs' must match a subset of the documents matched by 'rhs'.
        for (size_t i = 0; i < lhs->numChildren(); i++) {
            if (isSubsetOf(lhs->getChild(i), rhs)) {
                return true;
            }
        }
        return false;
    }

    if (lhs->matchType() == MatchExpression::OR) {
        // Every clause of 'lhs' must match a subset of the documents matched by 'rhs'.
        for (size_t i = 0; i < lhs->numChildren(); i++) {
            if (!isSubsetOf(lhs->getChild(i), rhs)) {
                return false;
            }
        }
        return true;
    }

    if (isComparisonMatchExpression(rhs)) {
        return _isSubsetOf(lhs, static_cast<const ComparisonMatchExpression*>(rhs));
    }

    if (rhs->matchType() == MatchExpression::EXISTS) {
        return _isSubsetOf(lhs, static_cast<const ExistsMatchExpression*>(rhs));
    }

    return false;
}

}  // namespace expression
}  // namespace mongo
