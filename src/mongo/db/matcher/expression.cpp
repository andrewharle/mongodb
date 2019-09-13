// expression.cpp


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

#include "mongo/db/matcher/expression.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {

using std::string;

MatchExpression::MatchExpression(MatchType type) : _matchType(type) {}

string MatchExpression::toString() const {
    StringBuilder buf;
    debugString(buf, 0);
    return buf.str();
}

void MatchExpression::_debugAddSpace(StringBuilder& debug, int level) const {
    for (int i = 0; i < level; i++)
        debug << "    ";
}

bool MatchExpression::matchesBSON(const BSONObj& doc, MatchDetails* details) const {
    BSONMatchableDocument mydoc(doc);
    return matches(&mydoc, details);
}

bool MatchExpression::matchesBSONElement(BSONElement elem, MatchDetails* details) const {
    BSONElementViewMatchableDocument matchableDoc(elem);
    return matches(&matchableDoc, details);
}

void MatchExpression::setCollator(const CollatorInterface* collator) {
    for (size_t i = 0; i < numChildren(); ++i) {
        getChild(i)->setCollator(collator);
    }

    _doSetCollator(collator);
}

void MatchExpression::addDependencies(DepsTracker* deps) const {
    for (size_t i = 0; i < numChildren(); ++i) {

        // Don't recurse through MatchExpression nodes which require an entire array or entire
        // subobject for matching.
        const auto type = matchType();
        switch (type) {
            case MatchExpression::ELEM_MATCH_VALUE:
            case MatchExpression::ELEM_MATCH_OBJECT:
            case MatchExpression::INTERNAL_SCHEMA_OBJECT_MATCH:
                continue;
            default:
                getChild(i)->addDependencies(deps);
        }
    }

    _doAddDependencies(deps);
}
}
