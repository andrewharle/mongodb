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

#include "mongo/db/views/view.h"

#include <memory>

#include "mongo/base/string_data.h"

namespace mongo {

ViewDefinition::ViewDefinition(StringData dbName,
                               StringData viewName,
                               StringData viewOnName,
                               const BSONObj& pipeline,
                               std::unique_ptr<CollatorInterface> collator)
    : _viewNss(dbName, viewName), _viewOnNss(dbName, viewOnName), _collator(std::move(collator)) {
    for (BSONElement e : pipeline) {
        _pipeline.push_back(e.Obj().getOwned());
    }
}

ViewDefinition::ViewDefinition(const ViewDefinition& other)
    : _viewNss(other._viewNss),
      _viewOnNss(other._viewOnNss),
      _collator(CollatorInterface::cloneCollator(other._collator.get())),
      _pipeline(other._pipeline) {}

ViewDefinition& ViewDefinition::operator=(const ViewDefinition& other) {
    _viewNss = other._viewNss;
    _viewOnNss = other._viewOnNss;
    _collator = CollatorInterface::cloneCollator(other._collator.get());
    _pipeline = other._pipeline;

    return *this;
}

void ViewDefinition::setViewOn(const NamespaceString& viewOnNss) {
    invariant(_viewNss.db() == viewOnNss.db());
    _viewOnNss = viewOnNss;
}

void ViewDefinition::setPipeline(const BSONElement& pipeline) {
    invariant(pipeline.type() == Array);
    _pipeline.clear();
    for (BSONElement e : pipeline.Obj()) {
        BSONObj value = e.Obj();
        _pipeline.push_back(value.copy());
    }
}
}  // namespace mongo
