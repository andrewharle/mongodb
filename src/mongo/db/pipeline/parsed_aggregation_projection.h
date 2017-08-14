/**
 *    Copyright (C) 2016 MongoDB, Inc.
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

#pragma once

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <memory>

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {

class BSONObj;
class Document;
class ExpressionContext;

namespace parsed_aggregation_projection {

enum class ProjectionType { kExclusion, kInclusion, kComputed };

/**
 * This class ensures that the specification was valid: that none of the paths specified conflict
 * with one another, that there is at least one field, etc. Here "projection" includes both
 * $project specifications and $addFields specifications.
 */
class ProjectionSpecValidator {
public:
    /**
     * Returns a Status: either a Status::OK() if the specification is valid for a projection, or a
     * non-OK Status, error number, and message with why not.
     */
    static Status validate(const BSONObj& spec);

private:
    ProjectionSpecValidator(const BSONObj& spec) : _rawObj(spec) {}

    /**
     * Uses '_seenPaths' to see if 'path' conflicts with any paths that have already been specified.
     *
     * For example, a user is not allowed to specify {'a': 1, 'a.b': 1}, or some similar conflicting
     * paths.
     */
    Status ensurePathDoesNotConflictOrThrow(StringData path);

    /**
     * Returns the relevant error if an invalid projection specification is detected.
     */
    Status validate();

    /**
     * Parses a single BSONElement. 'pathToElem' should include the field name of 'elem'.
     *
     * Delegates to parseSubObject() if 'elem' is an object. Otherwise adds the full path to 'elem'
     * to '_seenPaths'.
     *
     * Calls ensurePathDoesNotConflictOrThrow with the path to this element, which sets the _status
     * appropriately for conflicting path specifications.
     */
    Status parseElement(const BSONElement& elem, const FieldPath& pathToElem);

    /**
     * Traverses 'thisLevelSpec', parsing each element in turn.
     *
     * Sets _status appropriately if any paths conflict with each other or existing paths,
     * 'thisLevelSpec' contains a dotted path, or if 'thisLevelSpec' represents an invalid
     * expression.
     */
    Status parseNestedObject(const BSONObj& thisLevelSpec, const FieldPath& prefix);

    // The original object. Used to generate more helpful error messages.
    const BSONObj& _rawObj;

    // Tracks which paths we've seen to ensure no two paths conflict with each other.
    // Can be a vector since we iterate through it.
    std::vector<std::string> _seenPaths;
};

/**
 * A ParsedAggregationProjection is responsible for parsing and executing a $project. It
 * represents either an inclusion or exclusion projection. This is the common interface between the
 * two types of projections.
 */
class ParsedAggregationProjection
    : public DocumentSourceSingleDocumentTransformation::TransformerInterface {
public:
    /**
     * Main entry point for a ParsedAggregationProjection.
     *
     * Throws a UserException if 'spec' is an invalid projection specification.
     */
    static std::unique_ptr<ParsedAggregationProjection> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& spec);

    virtual ~ParsedAggregationProjection() = default;

    /**
     * Returns the type of projection represented by this ParsedAggregationProjection.
     */
    virtual ProjectionType getType() const = 0;

    /**
     * Parse the user-specified BSON object 'spec'. By the time this is called, 'spec' has already
     * been verified to not have any conflicting path specifications, and not to mix and match
     * inclusions and exclusions. 'variablesParseState' is used by any contained expressions to
     * track which variables are defined so that they can later be referenced at execution time.
     */
    virtual void parse(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const BSONObj& spec) = 0;

    /**
     * Optimize any expressions contained within this projection.
     */
    virtual void optimize() {}

    /**
     * Add any dependencies needed by this projection or any sub-expressions to 'deps'.
     */
    virtual DocumentSource::GetDepsReturn addDependencies(DepsTracker* deps) const {
        return DocumentSource::NOT_SUPPORTED;
    }

    /**
     * Apply the projection transformation.
     */
    Document applyTransformation(Document input) {
        return applyProjection(input);
    }

protected:
    ParsedAggregationProjection() = default;

    /**
     * Apply the projection to 'input'.
     */
    virtual Document applyProjection(Document input) const = 0;
};
}  // namespace parsed_aggregation_projection
}  // namespace mongo
