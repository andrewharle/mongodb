
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

#include "mongo/db/update/update_array_node.h"

namespace mongo {

// static
std::unique_ptr<UpdateNode> UpdateArrayNode::createUpdateNodeByMerging(
    const UpdateArrayNode& leftNode, const UpdateArrayNode& rightNode, FieldRef* pathTaken) {
    invariant(&leftNode._arrayFilters == &rightNode._arrayFilters);

    auto mergedNode = stdx::make_unique<UpdateArrayNode>(leftNode._arrayFilters);

    const bool wrapFieldNameAsArrayFilterIdentifier = true;
    mergedNode->_children = createUpdateNodeMapByMerging(
        leftNode._children, rightNode._children, pathTaken, wrapFieldNameAsArrayFilterIdentifier);

    return std::move(mergedNode);
}

UpdateNode::ApplyResult UpdateArrayNode::apply(ApplyParams applyParams) const {
    if (!applyParams.pathToCreate->empty()) {
        for (size_t i = 0; i < applyParams.pathToCreate->numParts(); ++i) {
            applyParams.pathTaken->appendPart(applyParams.pathToCreate->getPart(i));
        }
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "The path '" << applyParams.pathTaken->dottedField()
                                << "' must exist in the document in order to apply array updates.");
    }

    uassert(ErrorCodes::BadValue,
            str::stream() << "Cannot apply array updates to non-array element "
                          << applyParams.element.toString(),
            applyParams.element.getType() == BSONType::Array);

    // Construct a map from the array index to the set of updates that should be applied to the
    // array element at that index. We do not apply the updates yet because we need to know how many
    // array elements will be updated in order to know whether to pass 'logBuilder' on to the
    // UpdateNode children.
    std::map<size_t, std::vector<UpdateNode*>> matchingElements;
    size_t i = 0;
    for (auto childElement = applyParams.element.leftChild(); childElement.ok();
         childElement = childElement.rightSibling()) {

        // 'childElement' will always be serialized because no updates have been performed on the
        // array yet, and when we populate an upserted document with equality fields from the query,
        // arrays can only be added in entirety.
        invariant(childElement.hasValue());
        auto arrayElement = childElement.getValue();

        for (const auto& update : _children) {
            if (update.first.empty()) {

                // If the identifier is the empty string (e.g. came from 'a.$[].b'), the update
                // should be applied to all array elements.
                matchingElements[i].push_back(update.second.get());
            } else {
                auto filter = _arrayFilters.find(update.first);
                invariant(filter != _arrayFilters.end());
                if (filter->second->matchesBSONElement(arrayElement)) {
                    matchingElements[i].push_back(update.second.get());
                }
            }
        }

        ++i;
    }

    // If at most one array element will be updated, pass 'logBuilder' to the UpdateNode child when
    // applying it to that element.
    const bool childrenShouldLogThemselves = matchingElements.size() <= 1;

    // Keep track of which array elements were actually modified (non-noop updates) for logging
    // purposes. We only need to keep track of one element, since if more than one element is
    // modified, we log the whole array.
    boost::optional<mutablebson::Element> modifiedElement;
    size_t nModified = 0;

    // Update array elements.
    auto applyResult = ApplyResult::noopResult();
    i = 0;
    for (auto childElement = applyParams.element.leftChild(); childElement.ok();
         childElement = childElement.rightSibling()) {
        auto updates = matchingElements.find(i);
        if (updates != matchingElements.end()) {

            // Merge all of the updates for this array element.
            invariant(updates->second.size() > 0);
            auto mergedChild = updates->second[0];
            FieldRefTempAppend tempAppend(*(applyParams.pathTaken), childElement.getFieldName());
            for (size_t j = 1; j < updates->second.size(); ++j) {

                // Use the cached merge result, if it is available.
                const auto& cachedResult = _mergedChildrenCache[mergedChild][updates->second[j]];
                if (cachedResult.get()) {
                    mergedChild = cachedResult.get();
                    continue;
                }

                // The cached merge result is not available, so perform the merge and cache the
                // result.
                _mergedChildrenCache[mergedChild][updates->second[j]] =
                    UpdateNode::createUpdateNodeByMerging(
                        *mergedChild, *updates->second[j], applyParams.pathTaken.get());
                mergedChild = _mergedChildrenCache[mergedChild][updates->second[j]].get();
            }

            auto childApplyParams = applyParams;
            childApplyParams.element = childElement;
            if (!childrenShouldLogThemselves) {
                childApplyParams.logBuilder = nullptr;
            }

            auto childApplyResult = mergedChild->apply(childApplyParams);

            applyResult.indexesAffected =
                applyResult.indexesAffected || childApplyResult.indexesAffected;
            applyResult.noop = applyResult.noop && childApplyResult.noop;
            if (!childApplyResult.noop) {
                modifiedElement = childElement;
                ++nModified;
            }
        }

        ++i;
    }

    // If the child updates have not been logged, log the updated array elements.
    if (!childrenShouldLogThemselves && applyParams.logBuilder) {
        if (nModified > 1) {

            // Log the entire array.
            auto logElement = applyParams.logBuilder->getDocument().makeElementWithNewFieldName(
                applyParams.pathTaken->dottedField(), applyParams.element);
            invariant(logElement.ok());
            uassertStatusOK(applyParams.logBuilder->addToSets(logElement));
        } else if (nModified == 1) {

            // Log the modified array element.
            invariant(modifiedElement);
            FieldRefTempAppend tempAppend(*(applyParams.pathTaken),
                                          modifiedElement->getFieldName());
            auto logElement = applyParams.logBuilder->getDocument().makeElementWithNewFieldName(
                applyParams.pathTaken->dottedField(), *modifiedElement);
            invariant(logElement.ok());
            uassertStatusOK(applyParams.logBuilder->addToSets(logElement));
        }
    }

    return applyResult;
}

UpdateNode* UpdateArrayNode::getChild(const std::string& field) const {
    auto child = _children.find(field);
    if (child == _children.end()) {
        return nullptr;
    }
    return child->second.get();
}

void UpdateArrayNode::setChild(std::string field, std::unique_ptr<UpdateNode> child) {
    invariant(_children.find(field) == _children.end());
    _children[std::move(field)] = std::move(child);
}

}  // namespace mongo
