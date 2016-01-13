/**
 *    Copyright (C) 2013 10gen Inc.
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

namespace mongo {

    /**
     * These map to implementations of the PlanStage interface, all of which live in db/exec/
     */
    enum StageType {
        STAGE_AND_HASH,
        STAGE_AND_SORTED,
        STAGE_COLLSCAN,

        // If we're running a .count(), the query is fully covered by one ixscan, and the ixscan is
        // from one key to another, we can just skip through the keys without bothering to examine
        // them.
        STAGE_COUNT,

        // If we're running a distinct, we only care about one value for each key.  The distinct
        // stage is an ixscan with some key-skipping behvaior that only distinct uses.
        STAGE_DISTINCT,

        // This is more of an "internal-only" stage where we try to keep docs that were mutated
        // during query execution.
        STAGE_KEEP_MUTATIONS,

        STAGE_FETCH,

        // TODO: This is secretly an expression index but we need geometry -> covering for our
        // geohash.
        STAGE_GEO_2D,

        // The two $geoNear impls imply a fetch+sort and must be stages.
        STAGE_GEO_NEAR_2D,
        STAGE_GEO_NEAR_2DSPHERE,

        STAGE_IXSCAN,
        STAGE_LIMIT,
        STAGE_OR,
        STAGE_PROJECTION,
        STAGE_SHARDING_FILTER,
        STAGE_SKIP,
        STAGE_SORT,
        STAGE_SORT_MERGE,
        STAGE_TEXT,
        STAGE_UNKNOWN,
    };

}  // namespace mongo
