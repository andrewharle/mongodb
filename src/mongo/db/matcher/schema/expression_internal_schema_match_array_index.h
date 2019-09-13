
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

#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/stdx/memory.h"

namespace mongo {

/**
 * Matches arrays based on whether or not a specific element in the array matches a sub-expression.
 */
class InternalSchemaMatchArrayIndexMatchExpression final : public ArrayMatchingMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaMatchArrayIndex"_sd;

    InternalSchemaMatchArrayIndexMatchExpression(
        StringData path, long long index, std::unique_ptr<ExpressionWithPlaceholder> expression);

    void debugString(StringBuilder& debug, int level) const final;

    bool equivalent(const MatchExpression* expr) const final;

    /**
     * Matches 'array' if the element at '_index' matches '_expression', or if its size is less than
     * '_index'.
     */
    bool matchesArray(const BSONObj& array, MatchDetails* details) const final {
        BSONElement element;
        auto iterator = BSONObjIterator(array);

        // Skip ahead to the element we want, bailing early if there aren't enough elements.
        for (auto i = 0LL; i <= _index; ++i) {
            if (!iterator.more()) {
                return true;
            }
            element = iterator.next();
        }

        return _expression->matchesBSONElement(element, details);
    }

    BSONObj getSerializedRightHandSide() const final;

    std::unique_ptr<MatchExpression> shallowClone() const final;

    std::vector<MatchExpression*>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const final {
        return 1;
    }

    MatchExpression* getChild(size_t i) const final {
        invariant(i == 0);
        return _expression->getFilter();
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    long long _index = 0;
    std::unique_ptr<ExpressionWithPlaceholder> _expression;
};

}  // namespace mongo
