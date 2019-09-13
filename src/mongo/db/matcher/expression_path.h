
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

/**
 * A PathMatchExpression is an expression that acts on a field path with syntax
 * like "path.to.something": {$operator: ...}. Many such expressions are leaves in
 * the AST, such as $gt, $mod, $exists, and so on. But expressions that are not
 * leaves, such as $_internalSchemaObjectMatch, may also match against a field
 * path.
 */
class PathMatchExpression : public MatchExpression {
public:
    PathMatchExpression(MatchType matchType,
                        StringData path,
                        ElementPath::LeafArrayBehavior leafArrBehavior,
                        ElementPath::NonLeafArrayBehavior nonLeafArrayBehavior)
        : MatchExpression(matchType), _path(path) {
        _elementPath.init(_path);
        _elementPath.setLeafArrayBehavior(leafArrBehavior);
        _elementPath.setNonLeafArrayBehavior(nonLeafArrayBehavior);
    }

    virtual ~PathMatchExpression() {}

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final {
        MatchableDocument::IteratorHolder cursor(doc, &_elementPath);
        while (cursor->more()) {
            ElementIterator::Context e = cursor->next();
            if (!matchesSingleElement(e.element(), details)) {
                continue;
            }
            if (details && details->needRecord() && !e.arrayOffset().eoo()) {
                details->setElemMatchKey(e.arrayOffset().fieldName());
            }
            return true;
        }
        return false;
    }

    const StringData path() const final {
        return _path;
    }

    void setPath(StringData path) {
        _path = path;
        _elementPath.init(_path);
    }

    /**
     * Finds an applicable rename from 'renameList' (if one exists) and applies it to the expression
     * path. Each pair in 'renameList' specifies a path prefix that should be renamed (as the first
     * element) and the path components that should replace the renamed prefix (as the second
     * element).
     */
    void applyRename(const StringMap<std::string>& renameList) {
        FieldRef pathFieldRef(_path);

        size_t renamesFound = 0u;
        for (auto rename : renameList) {
            if (rename.first == _path) {
                _rewrittenPath = rename.second;

                ++renamesFound;
            }

            FieldRef prefixToRename(rename.first);
            if (prefixToRename.isPrefixOf(pathFieldRef)) {
                // Get the 'pathTail' by chopping off the 'prefixToRename' path components from the
                // beginning of the 'pathFieldRef' path.
                auto pathTail = pathFieldRef.dottedSubstring(prefixToRename.numParts(),
                                                             pathFieldRef.numParts());
                // Replace the chopped off components with the component names resulting from the
                // rename.
                _rewrittenPath = str::stream() << rename.second << "." << pathTail.toString();

                ++renamesFound;
            }
        }

        // There should never be multiple applicable renames.
        invariant(renamesFound <= 1u);
        if (renamesFound == 1u) {
            // There is an applicable rename. Modify the path of this expression to use the new
            // name.
            setPath(_rewrittenPath);
        }
    }

    void serialize(BSONObjBuilder* out) const override {
        out->append(path(), getSerializedRightHandSide());
    }

    /**
     * Returns a BSONObj that represents the right-hand-side of a PathMatchExpression. Used for
     * serialization of PathMatchExpression in cases where we do not want to serialize the path in
     * line with the expression. For example {x: {$not: {$eq: 1}}}, where $eq is the
     * PathMatchExpression.
     */
    virtual BSONObj getSerializedRightHandSide() const = 0;

protected:
    void _doAddDependencies(DepsTracker* deps) const final {
        if (!_path.empty()) {
            deps->fields.insert(_path.toString());
        }
    }

private:
    StringData _path;
    ElementPath _elementPath;

    // We use this when we rewrite the value in '_path' and we need a backing store for the
    // rewritten string.
    std::string _rewrittenPath;
};
}  // namespace mongo
