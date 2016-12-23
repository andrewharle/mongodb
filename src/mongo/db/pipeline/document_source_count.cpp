/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_count.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;
using std::string;

REGISTER_MULTI_STAGE_ALIAS(count,
                           LiteParsedDocumentSourceDefault::parse,
                           DocumentSourceCount::createFromBson);

vector<intrusive_ptr<DocumentSource>> DocumentSourceCount::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40156,
            str::stream() << "the count field must be a non-empty string",
            elem.type() == String);

    StringData elemString = elem.valueStringData();
    uassert(
        40157, str::stream() << "the count field must be a non-empty string", !elemString.empty());

    uassert(40158,
            str::stream() << "the count field cannot be a $-prefixed path",
            elemString[0] != '$');

    uassert(40159,
            str::stream() << "the count field cannot contain a null byte",
            elemString.find('\0') == string::npos);

    uassert(40160,
            str::stream() << "the count field cannot contain '.'",
            elemString.find('.') == string::npos);

    BSONObj groupObj = BSON("$group" << BSON("_id" << BSONNULL << elemString << BSON("$sum" << 1)));
    BSONObj projectObj = BSON("$project" << BSON("_id" << 0 << elemString << 1));

    auto groupSource = DocumentSourceGroup::createFromBson(groupObj.firstElement(), pExpCtx);
    auto projectSource = DocumentSourceProject::createFromBson(projectObj.firstElement(), pExpCtx);

    return {groupSource, projectSource};
}
}  // namespace mongo
