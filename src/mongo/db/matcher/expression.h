// expression.h


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


#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/match_details.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class CollatorInterface;
class MatchExpression;
class TreeMatchExpression;

typedef StatusWith<std::unique_ptr<MatchExpression>> StatusWithMatchExpression;

class MatchExpression {
    MONGO_DISALLOW_COPYING(MatchExpression);

public:
    enum MatchType {
        // tree types
        AND,
        OR,

        // array types
        ELEM_MATCH_OBJECT,
        ELEM_MATCH_VALUE,
        SIZE,

        // leaf types
        EQ,
        LTE,
        LT,
        GT,
        GTE,
        REGEX,
        MOD,
        EXISTS,
        MATCH_IN,
        BITS_ALL_SET,
        BITS_ALL_CLEAR,
        BITS_ANY_SET,
        BITS_ANY_CLEAR,

        // Negations.
        NOT,
        NOR,

        // special types
        TYPE_OPERATOR,
        GEO,
        WHERE,
        EXPRESSION,

        // Boolean expressions.
        ALWAYS_FALSE,
        ALWAYS_TRUE,

        // Things that we parse but cannot be answered without an index.
        GEO_NEAR,
        TEXT,

        // Expressions that are only created internally
        INTERNAL_2DSPHERE_KEY_IN_REGION,
        INTERNAL_2D_KEY_IN_REGION,
        INTERNAL_2D_POINT_IN_ANNULUS,

        // Used to represent an expression language equality in a match expression tree, since $eq
        // in the expression language has different semantics than the equality match expression.
        INTERNAL_EXPR_EQ,

        // JSON Schema expressions.
        INTERNAL_SCHEMA_ALLOWED_PROPERTIES,
        INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX,
        INTERNAL_SCHEMA_COND,
        INTERNAL_SCHEMA_EQ,
        INTERNAL_SCHEMA_FMOD,
        INTERNAL_SCHEMA_MATCH_ARRAY_INDEX,
        INTERNAL_SCHEMA_MAX_ITEMS,
        INTERNAL_SCHEMA_MAX_LENGTH,
        INTERNAL_SCHEMA_MAX_PROPERTIES,
        INTERNAL_SCHEMA_MIN_ITEMS,
        INTERNAL_SCHEMA_MIN_LENGTH,
        INTERNAL_SCHEMA_MIN_PROPERTIES,
        INTERNAL_SCHEMA_OBJECT_MATCH,
        INTERNAL_SCHEMA_ROOT_DOC_EQ,
        INTERNAL_SCHEMA_TYPE,
        INTERNAL_SCHEMA_UNIQUE_ITEMS,
        INTERNAL_SCHEMA_XOR,
    };

    /**
     * Make simplifying changes to the structure of a MatchExpression tree without altering its
     * semantics. This function may return:
     *   - a pointer to the original, unmodified MatchExpression,
     *   - a pointer to the original MatchExpression that has been mutated, or
     *   - a pointer to a new MatchExpression.
     *
     * The value of 'expression' must not be nullptr.
     */
    static std::unique_ptr<MatchExpression> optimize(std::unique_ptr<MatchExpression> expression) {
        auto optimizer = expression->getOptimizer();
        return optimizer(std::move(expression));
    }

    MatchExpression(MatchType type);
    virtual ~MatchExpression() {}

    //
    // Structural/AST information
    //

    /**
     * What type is the node?  See MatchType above.
     */
    MatchType matchType() const {
        return _matchType;
    }

    /**
     * Returns the number of child MatchExpression nodes contained by this node. It is expected that
     * a node that does not support child nodes will return 0.
     */
    virtual size_t numChildren() const = 0;

    /**
     * Returns the child of the current node at zero-based position 'index'. 'index' must be within
     * the range of [0, numChildren()).
     */
    virtual MatchExpression* getChild(size_t index) const = 0;

    /**
     * For MatchExpression nodes that can participate in tree restructuring (like AND/OR), returns a
     * non-const vector of MatchExpression* child nodes.
     * Do not use to traverse the MatchExpression tree. Use numChildren() and getChild(), which
     * provide access to all nodes.
     */
    virtual std::vector<MatchExpression*>* getChildVector() = 0;

    /**
     * Get the path of the leaf.  Returns StringData() if there is no path (node is logical).
     */
    virtual const StringData path() const {
        return StringData();
    }

    enum class MatchCategory {
        // Expressions that are leaves on the AST, these do not have any children.
        kLeaf,
        // Logical Expressions such as $and, $or, etc. that do not have a path and may have
        // one or more children.
        kLogical,
        // Expressions that operate on arrays only.
        kArrayMatching,
        // Expressions that don't fall into any particular bucket.
        kOther,
    };

    virtual MatchCategory getCategory() const = 0;

    // XXX: document
    virtual std::unique_ptr<MatchExpression> shallowClone() const = 0;

    // XXX document
    virtual bool equivalent(const MatchExpression* other) const = 0;

    //
    // Determine if a document satisfies the tree-predicate.
    //

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const = 0;

    virtual bool matchesBSON(const BSONObj& doc, MatchDetails* details = nullptr) const;

    /**
     * Determines if 'elem' would satisfy the predicate if wrapped with the top-level field name of
     * the predicate. Does not check that the predicate has a single top-level field name. For
     * example, given the object obj={a: [5]}, the predicate {i: {$gt: 0}} would match the element
     * obj["a"]["0"] because it performs the match as if the element at "a.0" were the BSONObj {i:
     * 5}.
     */
    virtual bool matchesBSONElement(BSONElement elem, MatchDetails* details = nullptr) const;

    /**
     * Determines if the element satisfies the tree-predicate.
     * Not valid for all expressions (e.g. $where); in those cases, returns false.
     */
    virtual bool matchesSingleElement(const BSONElement& e,
                                      MatchDetails* details = nullptr) const = 0;

    //
    // Tagging mechanism: Hang data off of the tree for retrieval later.
    //

    class TagData {
    public:
        enum class Type { IndexTag, RelevantTag, OrPushdownTag };
        virtual ~TagData() {}
        virtual void debugString(StringBuilder* builder) const = 0;
        virtual TagData* clone() const = 0;
        virtual Type getType() const = 0;
    };

    /**
     * Takes ownership
     */
    void setTag(TagData* data) {
        _tagData.reset(data);
    }
    TagData* getTag() const {
        return _tagData.get();
    }
    virtual void resetTag() {
        setTag(nullptr);
        for (size_t i = 0; i < numChildren(); ++i) {
            getChild(i)->resetTag();
        }
    }

    /**
     * Set the collator 'collator' on this match expression and all its children.
     *
     * 'collator' must outlive the match expression.
     */
    void setCollator(const CollatorInterface* collator);

    /**
     * Add the fields required for matching to 'deps'.
     */
    void addDependencies(DepsTracker* deps) const;

    /**
     * Serialize the MatchExpression to BSON, appending to 'out'. Output of this method is expected
     * to be a valid query object, that, when parsed, produces a logically equivalent
     * MatchExpression.
     */
    virtual void serialize(BSONObjBuilder* out) const = 0;

    /**
     * Returns true if this expression will always evaluate to false, such as an $or with no
     * children.
     */
    virtual bool isTriviallyFalse() const {
        return false;
    }

    /**
     * Returns true if this expression will always evaluate to true, such as an $and with no
     * children.
     */
    virtual bool isTriviallyTrue() const {
        return false;
    }

    //
    // Debug information
    //
    virtual std::string toString() const;
    virtual void debugString(StringBuilder& debug, int level = 0) const = 0;

protected:
    /**
     * An ExpressionOptimizerFunc implements tree simplifications for a MatchExpression tree with a
     * specific type of MatchExpression at the root. Except for requiring a specific MatchExpression
     * subclass, an ExpressionOptimizerFunc has the same requirements and functionality as described
     * in the specification of MatchExpression::getOptimizer(std::unique_ptr<MatchExpression>).
     */
    using ExpressionOptimizerFunc =
        stdx::function<std::unique_ptr<MatchExpression>(std::unique_ptr<MatchExpression>)>;

    /**
     * Subclasses that are collation-aware must implement this method in order to capture changes
     * to the collator that occur after initialization time.
     */
    virtual void _doSetCollator(const CollatorInterface* collator){};

    virtual void _doAddDependencies(DepsTracker* deps) const {}

    void _debugAddSpace(StringBuilder& debug, int level) const;

private:
    /**
     * Subclasses should implement this function to provide an ExpressionOptimizerFunc specific to
     * the subclass. This function is only called by
     * MatchExpression::optimize(std::unique_ptr<MatchExpression>), which is responsible for calling
     * MatchExpression::getOptimizer() on its input MatchExpression and then passing the same
     * MatchExpression to the resulting ExpressionOptimizerFunc. There should be no other callers
     * to this function.
     *
     * Any MatchExpression subclass that stores child MatchExpression objects is responsible for
     * returning an ExpressionOptimizerFunc that recursively calls
     * MatchExpression::optimize(std::unique_ptr<MatchExpression>) on those children.
     *
     * See the descriptions of MatchExpression::optimize(std::unique_ptr<MatchExpression>) and
     * ExpressionOptimizerFunc for additional explanation of their interfaces and functionality.
     */
    virtual ExpressionOptimizerFunc getOptimizer() const = 0;

    MatchType _matchType;
    std::unique_ptr<TagData> _tagData;
};
}
