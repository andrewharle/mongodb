
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

#include "mongo/base/initializer_dependency_graph.h"

#include <algorithm>
#include <iterator>
#include <sstream>

namespace mongo {

InitializerDependencyGraph::InitializerDependencyGraph() {}
InitializerDependencyGraph::~InitializerDependencyGraph() {}

Status InitializerDependencyGraph::addInitializer(std::string name,
                                                  InitializerFunction initFn,
                                                  DeinitializerFunction deinitFn,
                                                  std::vector<std::string> prerequisites,
                                                  std::vector<std::string> dependents) {
    if (!initFn)
        return Status(ErrorCodes::BadValue, "Illegal to supply a NULL function");

    InitializerDependencyNode& newNode = _nodes[name];
    if (newNode.initFn) {
        return Status(ErrorCodes::DuplicateKey, name);
    }

    newNode.initFn = std::move(initFn);
    newNode.deinitFn = std::move(deinitFn);

    for (size_t i = 0; i < prerequisites.size(); ++i) {
        newNode.prerequisites.insert(prerequisites[i]);
    }

    for (size_t i = 0; i < dependents.size(); ++i) {
        _nodes[dependents[i]].prerequisites.insert(name);
    }

    return Status::OK();
}

InitializerDependencyNode* InitializerDependencyGraph::getInitializerNode(const std::string& name) {
    NodeMap::iterator iter = _nodes.find(name);
    if (iter == _nodes.end())
        return nullptr;

    return &iter->second;
}

Status InitializerDependencyGraph::topSort(std::vector<std::string>* sortedNames) const {
    /*
     * This top-sort is implemented by performing a depth-first traversal of the dependency
     * graph, once for each node.  "visitedNodeNames" tracks the set of node names ever visited,
     * and it is used to prune each DFS.  A node that has been visited once on any DFS is never
     * visited again.  Complexity of this implementation is O(n+m) where "n" is the number of
     * nodes and "m" is the number of prerequisite edges.  Space complexity is O(n), in both
     * stack space and size of the "visitedNodeNames" set.
     *
     * "inProgressNodeNames" is used to detect and report cycles.
     */

    std::vector<std::string> inProgressNodeNames;
    stdx::unordered_set<std::string> visitedNodeNames;

    sortedNames->clear();
    for (const auto& node : _nodes) {
        Status status =
            recursiveTopSort(_nodes, node, &inProgressNodeNames, &visitedNodeNames, sortedNames);
        if (Status::OK() != status)
            return status;
    }
    for (const auto& node : _nodes) {
        if (!node.second.initFn) {
            std::ostringstream os;
            os << "No implementation provided for initializer " << node.first;
            return {ErrorCodes::BadValue, os.str()};
        }
    }
    return Status::OK();
}

Status InitializerDependencyGraph::recursiveTopSort(
    const NodeMap& nodeMap,
    const Node& currentNode,
    std::vector<std::string>* inProgressNodeNames,
    stdx::unordered_set<std::string>* visitedNodeNames,
    std::vector<std::string>* sortedNames) {

    /*
     * The top sort is performed by depth-first traversal starting at each node in the
     * dependency graph, short-circuited any time a node is seen that has already been visited
     * in any traversal.  "visitedNodeNames" is the set of nodes that have been successfully
     * visited, while "inProgressNodeNames" are nodes currently in the exploration chain.  This
     * structure is kept explicitly to facilitate cycle detection.
     *
     * This function implements a depth-first traversal, and is called once for each node in the
     * graph by topSort(), above.
     */

    if ((*visitedNodeNames).count(currentNode.first))
        return Status::OK();

    inProgressNodeNames->push_back(currentNode.first);

    auto firstOccurence =
        std::find(inProgressNodeNames->begin(), inProgressNodeNames->end(), currentNode.first);
    if (std::next(firstOccurence) != inProgressNodeNames->end()) {
        sortedNames->clear();
        std::copy(firstOccurence, inProgressNodeNames->end(), std::back_inserter(*sortedNames));
        std::ostringstream os;
        os << "Cycle in dependendcy graph: " << sortedNames->at(0);
        for (size_t i = 1; i < sortedNames->size(); ++i)
            os << " -> " << sortedNames->at(i);
        return Status(ErrorCodes::GraphContainsCycle, os.str());
    }

    for (const auto& prereq : currentNode.second.prerequisites) {
        auto nextNode = nodeMap.find(prereq);
        if (nextNode == nodeMap.end()) {
            std::ostringstream os;
            os << "Initializer " << currentNode.first << " depends on missing initializer "
               << prereq;
            return {ErrorCodes::BadValue, os.str()};
        }

        Status status = recursiveTopSort(
            nodeMap, *nextNode, inProgressNodeNames, visitedNodeNames, sortedNames);
        if (Status::OK() != status)
            return status;
    }
    sortedNames->push_back(currentNode.first);
    if (inProgressNodeNames->back() != currentNode.first)
        return Status(ErrorCodes::InternalError, "inProgressNodeNames stack corrupt");
    inProgressNodeNames->pop_back();
    visitedNodeNames->insert(currentNode.first);
    return Status::OK();
}

}  // namespace mongo
