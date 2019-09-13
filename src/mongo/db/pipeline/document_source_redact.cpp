
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

#include "mongo/db/pipeline/document_source_redact.h"

#include <boost/optional.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

DocumentSourceRedact::DocumentSourceRedact(const intrusive_ptr<ExpressionContext>& expCtx,
                                           const intrusive_ptr<Expression>& expression)
    : DocumentSource(expCtx), _expression(expression) {}

REGISTER_DOCUMENT_SOURCE(redact,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceRedact::createFromBson);

const char* DocumentSourceRedact::getSourceName() const {
    return "$redact";
}

static const Value descendVal = Value("descend"_sd);
static const Value pruneVal = Value("prune"_sd);
static const Value keepVal = Value("keep"_sd);

DocumentSource::GetNextResult DocumentSourceRedact::getNext() {
    pExpCtx->checkForInterrupt();

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        auto& variables = pExpCtx->variables;
        variables.setValue(_currentId, Value(nextInput.getDocument()));
        if (boost::optional<Document> result = redactObject(nextInput.releaseDocument())) {
            return std::move(*result);
        }
    }

    return nextInput;
}

Pipeline::SourceContainer::iterator DocumentSourceRedact::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (nextMatch) {
        const BSONObj redactSafePortion = nextMatch->redactSafePortion();

        if (!redactSafePortion.isEmpty()) {
            // Because R-M turns into M-R-M without modifying the original $match, we cannot step
            // backwards and optimize from before the $redact, otherwise this will just loop and
            // create an infinite number of $matches.
            Pipeline::SourceContainer::iterator returnItr = std::next(itr);

            container->insert(itr, DocumentSourceMatch::create(redactSafePortion, pExpCtx));

            return returnItr;
        }
    }
    return std::next(itr);
}

Value DocumentSourceRedact::redactValue(const Value& in, const Document& root) {
    const BSONType valueType = in.getType();
    if (valueType == Object) {
        pExpCtx->variables.setValue(_currentId, in);
        const boost::optional<Document> result = redactObject(root);
        if (result) {
            return Value(*result);
        } else {
            return Value();
        }
    } else if (valueType == Array) {
        // TODO dont copy if possible
        vector<Value> newArr;
        const vector<Value>& arr = in.getArray();
        for (size_t i = 0; i < arr.size(); i++) {
            if (arr[i].getType() == Object || arr[i].getType() == Array) {
                const Value toAdd = redactValue(arr[i], root);
                if (!toAdd.missing()) {
                    newArr.push_back(toAdd);
                }
            } else {
                newArr.push_back(arr[i]);
            }
        }
        return Value(std::move(newArr));
    } else {
        return in;
    }
}

boost::optional<Document> DocumentSourceRedact::redactObject(const Document& root) {
    auto& variables = pExpCtx->variables;
    const Value expressionResult = _expression->evaluate(root, &variables);

    ValueComparator simpleValueCmp;
    if (simpleValueCmp.evaluate(expressionResult == keepVal)) {
        return variables.getDocument(_currentId, root);
    } else if (simpleValueCmp.evaluate(expressionResult == pruneVal)) {
        return boost::optional<Document>();
    } else if (simpleValueCmp.evaluate(expressionResult == descendVal)) {
        const Document in = variables.getDocument(_currentId, root);
        MutableDocument out;
        out.copyMetaDataFrom(in);
        FieldIterator fields(in);
        while (fields.more()) {
            const Document::FieldPair field(fields.next());

            // This changes CURRENT so don't read from variables after this
            const Value val = redactValue(field.second, root);
            if (!val.missing()) {
                out.addField(field.first, val);
            }
        }
        return out.freeze();
    } else {
        uasserted(17053,
                  str::stream() << "$redact's expression should not return anything "
                                << "aside from the variables $$KEEP, $$DESCEND, and "
                                << "$$PRUNE, but returned "
                                << expressionResult.toString());
    }
}

intrusive_ptr<DocumentSource> DocumentSourceRedact::optimize() {
    _expression = _expression->optimize();
    return this;
}

Value DocumentSourceRedact::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(DOC(getSourceName() << _expression.get()->serialize(static_cast<bool>(explain))));
}

intrusive_ptr<DocumentSource> DocumentSourceRedact::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    VariablesParseState vps = expCtx->variablesParseState;
    Variables::Id currentId = vps.defineVariable("CURRENT");  // will differ from ROOT
    Variables::Id decendId = vps.defineVariable("DESCEND");
    Variables::Id pruneId = vps.defineVariable("PRUNE");
    Variables::Id keepId = vps.defineVariable("KEEP");
    intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, elem, vps);
    intrusive_ptr<DocumentSourceRedact> source = new DocumentSourceRedact(expCtx, expression);

    // TODO figure out how much of this belongs in constructor and how much here.
    // Set up variables. Never need to reset DESCEND, PRUNE, or KEEP.
    source->_currentId = currentId;
    auto& variables = expCtx->variables;
    variables.setValue(decendId, descendVal);
    variables.setValue(pruneId, pruneVal);
    variables.setValue(keepId, keepVal);


    return source;
}
}
