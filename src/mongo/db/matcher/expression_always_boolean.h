
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

#include "mongo/db/matcher/expression.h"

namespace mongo {

class AlwaysBooleanMatchExpression : public MatchExpression {
public:
    AlwaysBooleanMatchExpression(MatchType type, bool value)
        : MatchExpression(type), _value(value) {}

    virtual ~AlwaysBooleanMatchExpression() = default;

    /**
     * The name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final {
        return _value;
    }

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final {
        return _value;
    }

    void debugString(StringBuilder& debug, int level = 0) const final {
        _debugAddSpace(debug, level);
        debug << name() << ": 1\n";
    }

    void serialize(BSONObjBuilder* out) const final {
        out->append(name(), 1);
    }

    bool equivalent(const MatchExpression* other) const final {
        return other->matchType() == matchType();
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    size_t numChildren() const override {
        return 0;
    }

    MatchExpression* getChild(size_t i) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<MatchExpression*>* getChildVector() override {
        return nullptr;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    bool _value;
};

class AlwaysFalseMatchExpression final : public AlwaysBooleanMatchExpression {
public:
    static constexpr StringData kName = "$alwaysFalse"_sd;

    AlwaysFalseMatchExpression() : AlwaysBooleanMatchExpression(MatchType::ALWAYS_FALSE, false) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        return stdx::make_unique<AlwaysFalseMatchExpression>();
    }

    bool isTriviallyFalse() const final {
        return true;
    }
};

class AlwaysTrueMatchExpression final : public AlwaysBooleanMatchExpression {
public:
    static constexpr StringData kName = "$alwaysTrue"_sd;

    AlwaysTrueMatchExpression() : AlwaysBooleanMatchExpression(MatchType::ALWAYS_TRUE, true) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        return stdx::make_unique<AlwaysTrueMatchExpression>();
    }

    bool isTriviallyTrue() const final {
        return true;
    }
};

}  // namespace mongo
