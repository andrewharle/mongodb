// expression_array.cpp


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

#include "mongo/db/matcher/expression_array.h"

#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"

namespace mongo {

bool ArrayMatchingMatchExpression::matchesSingleElement(const BSONElement& elt,
                                                        MatchDetails* details) const {
    if (elt.type() != BSONType::Array) {
        return false;
    }

    return matchesArray(elt.embeddedObject(), details);
}


bool ArrayMatchingMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ArrayMatchingMatchExpression* realOther =
        static_cast<const ArrayMatchingMatchExpression*>(other);

    if (path() != realOther->path())
        return false;

    if (numChildren() != realOther->numChildren())
        return false;

    for (unsigned i = 0; i < numChildren(); i++)
        if (!getChild(i)->equivalent(realOther->getChild(i)))
            return false;
    return true;
}


// -------

ElemMatchObjectMatchExpression::ElemMatchObjectMatchExpression(StringData path,
                                                               MatchExpression* sub)
    : ArrayMatchingMatchExpression(ELEM_MATCH_OBJECT, path), _sub(sub) {}

bool ElemMatchObjectMatchExpression::matchesArray(const BSONObj& anArray,
                                                  MatchDetails* details) const {
    BSONObjIterator i(anArray);
    while (i.more()) {
        BSONElement inner = i.next();
        if (!inner.isABSONObj())
            continue;
        if (_sub->matchesBSON(inner.Obj(), NULL)) {
            if (details && details->needRecord()) {
                details->setElemMatchKey(inner.fieldName());
            }
            return true;
        }
    }
    return false;
}

void ElemMatchObjectMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " $elemMatch (obj)";

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
    _sub->debugString(debug, level + 1);
}

BSONObj ElemMatchObjectMatchExpression::getSerializedRightHandSide() const {
    BSONObjBuilder subBob;
    _sub->serialize(&subBob);
    return BSON("$elemMatch" << subBob.obj());
}

MatchExpression::ExpressionOptimizerFunc ElemMatchObjectMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        auto& elemExpression = static_cast<ElemMatchObjectMatchExpression&>(*expression);
        elemExpression._sub = MatchExpression::optimize(std::move(elemExpression._sub));

        return expression;
    };
}

// -------

ElemMatchValueMatchExpression::ElemMatchValueMatchExpression(StringData path, MatchExpression* sub)
    : ArrayMatchingMatchExpression(ELEM_MATCH_VALUE, path) {
    add(sub);
}

ElemMatchValueMatchExpression::ElemMatchValueMatchExpression(StringData path)
    : ArrayMatchingMatchExpression(ELEM_MATCH_VALUE, path) {}

ElemMatchValueMatchExpression::~ElemMatchValueMatchExpression() {
    for (unsigned i = 0; i < _subs.size(); i++)
        delete _subs[i];
    _subs.clear();
}

void ElemMatchValueMatchExpression::add(MatchExpression* sub) {
    verify(sub);
    _subs.push_back(sub);
}

bool ElemMatchValueMatchExpression::matchesArray(const BSONObj& anArray,
                                                 MatchDetails* details) const {
    BSONObjIterator i(anArray);
    while (i.more()) {
        BSONElement inner = i.next();

        if (_arrayElementMatchesAll(inner)) {
            if (details && details->needRecord()) {
                details->setElemMatchKey(inner.fieldName());
            }
            return true;
        }
    }
    return false;
}

bool ElemMatchValueMatchExpression::_arrayElementMatchesAll(const BSONElement& e) const {
    for (unsigned i = 0; i < _subs.size(); i++) {
        if (!_subs[i]->matchesSingleElement(e))
            return false;
    }
    return true;
}

void ElemMatchValueMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " $elemMatch (value)";

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
    debug << "\n";
    for (unsigned i = 0; i < _subs.size(); i++) {
        _subs[i]->debugString(debug, level + 1);
    }
}

BSONObj ElemMatchValueMatchExpression::getSerializedRightHandSide() const {
    BSONObjBuilder emBob;

    for (unsigned i = 0; i < _subs.size(); i++) {
        BSONObjBuilder predicate;
        _subs[i]->serialize(&predicate);
        BSONObj predObj = predicate.obj();
        emBob.appendElements(predObj.firstElement().embeddedObject());
    }

    return BSON("$elemMatch" << emBob.obj());
}

MatchExpression::ExpressionOptimizerFunc ElemMatchValueMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        auto& subs = static_cast<ElemMatchValueMatchExpression&>(*expression)._subs;

        for (MatchExpression*& subExpression : subs) {
            auto optimizedSubExpression =
                MatchExpression::optimize(std::unique_ptr<MatchExpression>(subExpression));
            subExpression = optimizedSubExpression.release();
        }

        return expression;
    };
}

// ---------

SizeMatchExpression::SizeMatchExpression(StringData path, int size)
    : ArrayMatchingMatchExpression(SIZE, path), _size(size) {}

bool SizeMatchExpression::matchesArray(const BSONObj& anArray, MatchDetails* details) const {
    if (_size < 0)
        return false;
    return anArray.nFields() == _size;
}

void SizeMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " $size : " << _size << "\n";

    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        debug << " ";
        td->debugString(&debug);
    }
}

BSONObj SizeMatchExpression::getSerializedRightHandSide() const {
    return BSON("$size" << _size);
}

bool SizeMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const SizeMatchExpression* realOther = static_cast<const SizeMatchExpression*>(other);
    return path() == realOther->path() && _size == realOther->_size;
}


// ------------------
}
