
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

#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

/**
 * An InternalExprEqMatchExpression is an equality expression with similar semantics to the $eq
 * expression. It differs from the regular equality match expression in the following ways:
 *
 * - The document will match if there is an array anywhere along the path. By always returning true
 *   in such cases, we match a superset of documents that the related aggregation expression would
 *   match. This sidesteps us having to implement field path expression evaluation as part of this
 *   match expression.
 *
 * - Equality to null matches literal nulls, but not documents in which the field path is missing or
 *   undefined.
 *
 * - Equality to an array is illegal. It is invalid usage to construct a
 *   InternalExprEqMatchExpression node which compares to an array.
 */
class InternalExprEqMatchExpression final : public ComparisonMatchExpressionBase {
public:
    static constexpr StringData kName = "$_internalExprEq"_sd;

    InternalExprEqMatchExpression(StringData path, BSONElement value)
        : ComparisonMatchExpressionBase(MatchType::INTERNAL_EXPR_EQ,
                                        path,
                                        value,
                                        ElementPath::LeafArrayBehavior::kNoTraversal,
                                        ElementPath::NonLeafArrayBehavior::kMatchSubpath) {
        invariant(_rhs.type() != BSONType::Undefined);
        invariant(_rhs.type() != BSONType::Array);
    }

    StringData name() const final {
        return kName;
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails*) const final;

    std::unique_ptr<MatchExpression> shallowClone() const final;
};

}  // namespace mongo
