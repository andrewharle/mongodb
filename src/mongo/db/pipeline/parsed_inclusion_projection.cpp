
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

#include "mongo/db/pipeline/parsed_inclusion_projection.h"

#include <algorithm>

namespace mongo {

namespace parsed_aggregation_projection {

using std::string;
using std::unique_ptr;

//
// InclusionNode
//

InclusionNode::InclusionNode(std::string pathToNode) : _pathToNode(std::move(pathToNode)) {}

void InclusionNode::optimize() {
    for (auto&& expressionIt : _expressions) {
        _expressions[expressionIt.first] = expressionIt.second->optimize();
    }
    for (auto&& childPair : _children) {
        childPair.second->optimize();
    }
}

void InclusionNode::serialize(MutableDocument* output,
                              boost::optional<ExplainOptions::Verbosity> explain) const {
    // Always put "_id" first if it was included (implicitly or explicitly).
    if (_inclusions.find("_id") != _inclusions.end()) {
        output->addField("_id", Value(true));
    }

    for (auto&& includedField : _inclusions) {
        if (includedField == "_id") {
            // Handled above.
            continue;
        }
        output->addField(includedField, Value(true));
    }

    for (auto&& field : _orderToProcessAdditionsAndChildren) {
        auto childIt = _children.find(field);
        if (childIt != _children.end()) {
            MutableDocument subDoc;
            childIt->second->serialize(&subDoc, explain);
            output->addField(field, subDoc.freezeToValue());
        } else {
            auto expressionIt = _expressions.find(field);
            invariant(expressionIt != _expressions.end());
            output->addField(field, expressionIt->second->serialize(static_cast<bool>(explain)));
        }
    }
}

void InclusionNode::addDependencies(DepsTracker* deps) const {
    for (auto&& includedField : _inclusions) {
        deps->fields.insert(FieldPath::getFullyQualifiedPath(_pathToNode, includedField));
    }

    if (!_pathToNode.empty() && !_expressions.empty()) {
        // The shape of any computed fields in the output will change depending on if the field is
        // an array or not, so in addition to any dependencies of the expression itself, we need to
        // add this field to our dependencies.
        deps->fields.insert(_pathToNode);
    }

    for (auto&& expressionPair : _expressions) {
        expressionPair.second->addDependencies(deps);
    }
    for (auto&& childPair : _children) {
        childPair.second->addDependencies(deps);
    }
}

void InclusionNode::applyInclusions(const Document& inputDoc, MutableDocument* outputDoc) const {
    auto it = inputDoc.fieldIterator();
    while (it.more()) {
        auto fieldPair = it.next();
        auto fieldName = fieldPair.first.toString();
        if (_inclusions.find(fieldName) != _inclusions.end()) {
            outputDoc->addField(fieldName, fieldPair.second);
            continue;
        }

        auto childIt = _children.find(fieldName);
        if (childIt != _children.end()) {
            outputDoc->addField(fieldName,
                                childIt->second->applyInclusionsToValue(fieldPair.second));
        }
    }
}

Value InclusionNode::applyInclusionsToValue(Value inputValue) const {
    if (inputValue.getType() == BSONType::Object) {
        MutableDocument output;
        applyInclusions(inputValue.getDocument(), &output);
        return output.freezeToValue();
    } else if (inputValue.getType() == BSONType::Array) {
        std::vector<Value> values = inputValue.getArray();
        for (auto it = values.begin(); it != values.end(); ++it) {
            *it = applyInclusionsToValue(*it);
        }
        return Value(std::move(values));
    } else {
        // This represents the case where we are including children of a field which does not have
        // any children. e.g. applying the projection {"a.b": true} to the document {a: 2}. It is
        // somewhat weird, but our semantics are to return a document without the field "a". To do
        // so, we return the "missing" value here.
        return Value();
    }
}

void InclusionNode::addComputedFields(MutableDocument* outputDoc, const Document& root) const {
    for (auto&& field : _orderToProcessAdditionsAndChildren) {
        auto childIt = _children.find(field);
        if (childIt != _children.end()) {
            outputDoc->setField(field,
                                childIt->second->addComputedFields(outputDoc->peek()[field], root));
        } else {
            auto expressionIt = _expressions.find(field);
            invariant(expressionIt != _expressions.end());
            outputDoc->setField(
                field,
                expressionIt->second->evaluate(
                    root, &(expressionIt->second->getExpressionContext()->variables)));
        }
    }
}

Value InclusionNode::addComputedFields(Value inputValue, const Document& root) const {
    if (inputValue.getType() == BSONType::Object) {
        MutableDocument outputDoc(inputValue.getDocument());
        addComputedFields(&outputDoc, root);
        return outputDoc.freezeToValue();
    } else if (inputValue.getType() == BSONType::Array) {
        std::vector<Value> values = inputValue.getArray();
        for (auto it = values.begin(); it != values.end(); ++it) {
            *it = addComputedFields(*it, root);
        }
        return Value(std::move(values));
    } else {
        if (subtreeContainsComputedFields()) {
            // Our semantics in this case are to replace whatever existing value we find with a new
            // document of all the computed values. This case represents applying a projection like
            // {"a.b": {$literal: 1}} to the document {a: 1}. This should yield {a: {b: 1}}.
            MutableDocument outputDoc;
            addComputedFields(&outputDoc, root);
            return outputDoc.freezeToValue();
        }
        // We didn't have any expressions, so just return the missing value.
        return Value();
    }
}

bool InclusionNode::subtreeContainsComputedFields() const {
    return (!_expressions.empty()) ||
        std::any_of(
               _children.begin(),
               _children.end(),
               [](const std::pair<const std::string, std::unique_ptr<InclusionNode>>& childPair) {
                   return childPair.second->subtreeContainsComputedFields();
               });
}

void InclusionNode::addComputedField(const FieldPath& path, boost::intrusive_ptr<Expression> expr) {
    if (path.getPathLength() == 1) {
        auto fieldName = path.fullPath();
        _expressions[fieldName] = expr;
        _orderToProcessAdditionsAndChildren.push_back(fieldName);
        return;
    }
    addOrGetChild(path.getFieldName(0).toString())->addComputedField(path.tail(), expr);
}

void InclusionNode::addIncludedField(const FieldPath& path) {
    if (path.getPathLength() == 1) {
        _inclusions.insert(path.fullPath());
        return;
    }
    addOrGetChild(path.getFieldName(0).toString())->addIncludedField(path.tail());
}

InclusionNode* InclusionNode::addOrGetChild(std::string field) {
    auto child = getChild(field);
    return child ? child : addChild(field);
}

InclusionNode* InclusionNode::getChild(string field) const {
    auto childIt = _children.find(field);
    return childIt == _children.end() ? nullptr : childIt->second.get();
}

InclusionNode* InclusionNode::addChild(string field) {
    invariant(!str::contains(field, "."));
    _orderToProcessAdditionsAndChildren.push_back(field);
    auto childPath = FieldPath::getFullyQualifiedPath(_pathToNode, field);
    auto insertedPair = _children.emplace(
        std::make_pair(std::move(field), stdx::make_unique<InclusionNode>(std::move(childPath))));
    return insertedPair.first->second.get();
}

void InclusionNode::addPreservedPaths(std::set<std::string>* preservedPaths) const {
    // Only our inclusion paths are preserved. This inclusion node may also have paths with
    // associated expressions, but those paths are modified and therefore are not considered
    // "preserved".
    for (auto&& includedField : _inclusions) {
        preservedPaths->insert(FieldPath::getFullyQualifiedPath(_pathToNode, includedField));
    }
    for (auto&& childPair : _children) {
        childPair.second->addPreservedPaths(preservedPaths);
    }
}

void InclusionNode::addComputedPaths(std::set<std::string>* computedPaths,
                                     StringMap<std::string>* renamedPaths) const {
    for (auto&& computedPair : _expressions) {
        // The expression's path is the concatenation of the path to this inclusion node,
        // plus the field name associated with the expression.
        auto exprPath = FieldPath::getFullyQualifiedPath(_pathToNode, computedPair.first);
        auto exprComputedPaths = computedPair.second->getComputedPaths(exprPath);
        computedPaths->insert(exprComputedPaths.paths.begin(), exprComputedPaths.paths.end());

        for (auto&& rename : exprComputedPaths.renames) {
            (*renamedPaths)[rename.first] = rename.second;
        }
    }
    for (auto&& childPair : _children) {
        childPair.second->addComputedPaths(computedPaths, renamedPaths);
    }
}

//
// ParsedInclusionProjection
//

void ParsedInclusionProjection::parse(const BSONObj& spec) {
    // It is illegal to specify a projection with no output fields.
    bool atLeastOneFieldInOutput = false;

    // Tracks whether or not we should implicitly include "_id".
    bool idSpecified = false;

    for (auto elem : spec) {
        auto fieldName = elem.fieldNameStringData();
        idSpecified = idSpecified || fieldName == "_id" || fieldName.startsWith("_id.");
        if (fieldName == "_id") {
            const bool idIsExcluded = (!elem.trueValue() && (elem.isNumber() || elem.isBoolean()));
            if (idIsExcluded) {
                // Ignoring "_id" here will cause it to be excluded from result documents.
                _idExcluded = true;
                continue;
            }

            // At least part of "_id" is included or a computed field. Fall through to below to
            // parse what exactly "_id" was specified as.
        }

        atLeastOneFieldInOutput = true;
        switch (elem.type()) {
            case BSONType::Bool:
            case BSONType::NumberInt:
            case BSONType::NumberLong:
            case BSONType::NumberDouble:
            case BSONType::NumberDecimal: {
                // This is an inclusion specification.
                invariant(elem.trueValue());
                _root->addIncludedField(FieldPath(elem.fieldName()));
                break;
            }
            case BSONType::Object: {
                // This is either an expression, or a nested specification.
                if (parseObjectAsExpression(fieldName, elem.Obj(), _expCtx->variablesParseState)) {
                    // It was an expression.
                    break;
                }

                // The field name might be a dotted path. If so, we need to keep adding children
                // to our tree until we create a child that represents that path.
                auto remainingPath = FieldPath(elem.fieldName());
                auto child = _root.get();
                while (remainingPath.getPathLength() > 1) {
                    child = child->addOrGetChild(remainingPath.getFieldName(0).toString());
                    remainingPath = remainingPath.tail();
                }
                // It is illegal to construct an empty FieldPath, so the above loop ends one
                // iteration too soon. Add the last path here.
                child = child->addOrGetChild(remainingPath.fullPath());

                parseSubObject(elem.Obj(), _expCtx->variablesParseState, child);
                break;
            }
            default: {
                // This is a literal value.
                _root->addComputedField(
                    FieldPath(elem.fieldName()),
                    Expression::parseOperand(_expCtx, elem, _expCtx->variablesParseState));
            }
        }
    }

    if (!idSpecified) {
        // "_id" wasn't specified, so include it by default.
        atLeastOneFieldInOutput = true;
        _root->addIncludedField(FieldPath("_id"));
    }

    uassert(16403,
            str::stream() << "$project requires at least one output field: " << spec.toString(),
            atLeastOneFieldInOutput);
}

Document ParsedInclusionProjection::applyProjection(const Document& inputDoc) const {
    // All expressions will be evaluated in the context of the input document, before any
    // transformations have been applied.
    MutableDocument output;
    _root->applyInclusions(inputDoc, &output);
    _root->addComputedFields(&output, inputDoc);

    // Always pass through the metadata.
    output.copyMetaDataFrom(inputDoc);
    return output.freeze();
}

bool ParsedInclusionProjection::parseObjectAsExpression(
    StringData pathToObject,
    const BSONObj& objSpec,
    const VariablesParseState& variablesParseState) {
    if (objSpec.firstElementFieldName()[0] == '$') {
        // This is an expression like {$add: [...]}. We have already verified that it has only one
        // field.
        invariant(objSpec.nFields() == 1);
        _root->addComputedField(pathToObject,
                                Expression::parseExpression(_expCtx, objSpec, variablesParseState));
        return true;
    }
    return false;
}

void ParsedInclusionProjection::parseSubObject(const BSONObj& subObj,
                                               const VariablesParseState& variablesParseState,
                                               InclusionNode* node) {
    for (auto elem : subObj) {
        invariant(elem.fieldName()[0] != '$');
        // Dotted paths in a sub-object have already been disallowed in
        // ParsedAggregationProjection's parsing.
        invariant(elem.fieldNameStringData().find('.') == std::string::npos);

        switch (elem.type()) {
            case BSONType::Bool:
            case BSONType::NumberInt:
            case BSONType::NumberLong:
            case BSONType::NumberDouble:
            case BSONType::NumberDecimal: {
                // This is an inclusion specification.
                invariant(elem.trueValue());
                node->addIncludedField(FieldPath(elem.fieldName()));
                break;
            }
            case BSONType::Object: {
                // This is either an expression, or a nested specification.
                auto fieldName = elem.fieldNameStringData().toString();
                if (parseObjectAsExpression(
                        FieldPath::getFullyQualifiedPath(node->getPath(), fieldName),
                        elem.Obj(),
                        variablesParseState)) {
                    break;
                }
                auto child = node->addOrGetChild(fieldName);
                parseSubObject(elem.Obj(), variablesParseState, child);
                break;
            }
            default: {
                // This is a literal value.
                node->addComputedField(
                    FieldPath(elem.fieldName()),
                    Expression::parseOperand(_expCtx, elem, variablesParseState));
            }
        }
    }
}

bool ParsedInclusionProjection::isSubsetOfProjection(const BSONObj& proj) const {
    std::set<std::string> preservedPaths;
    _root->addPreservedPaths(&preservedPaths);
    for (auto&& includedField : preservedPaths) {
        if (!proj.hasField(includedField))
            return false;
    }

    // If the inclusion has any computed fields or renamed fields, then it's not a subset.
    std::set<std::string> computedPaths;
    StringMap<std::string> renamedPaths;
    _root->addComputedPaths(&computedPaths, &renamedPaths);
    return computedPaths.empty() && renamedPaths.empty();
}

}  // namespace parsed_aggregation_projection
}  // namespace mongo
