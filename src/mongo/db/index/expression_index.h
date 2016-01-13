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

#include "mongo/db/jsobj.h"
#include "mongo/db/hasher.h"
#include "mongo/db/query/index_bounds_builder.h"

namespace mongo {

    /**
     * Functions that compute expression index mappings.
     *
     * TODO: I think we could structure this more generally with respect to planning.
     */
    class ExpressionMapping {
    public:
        static BSONObj hash(const BSONElement& value) {
            BSONObjBuilder bob;
            bob.append("", BSONElementHasher::hash64(value, BSONElementHasher::DEFAULT_HASH_SEED));
            return bob.obj();
        }

        // TODO: what should we really pass in for indexInfoObj?
        static void cover2dsphere(const S2Region& region,
                                  const BSONObj& indexInfoObj,
                                  OrderedIntervalList* oilOut) {

            int coarsestIndexedLevel;
            BSONElement ce = indexInfoObj["coarsestIndexedLevel"];
            if (ce.isNumber()) {
                coarsestIndexedLevel = ce.numberInt();
            }
            else {
                coarsestIndexedLevel =
                    S2::kAvgEdge.GetClosestLevel(100 * 1000.0 / kRadiusOfEarthInMeters);
            }

            // The min level of our covering is the level whose cells are the closest match to the
            // *area* of the region (or the max indexed level, whichever is smaller) The max level
            // is 4 sizes larger.
            double edgeLen = sqrt(region.GetRectBound().Area());
            S2RegionCoverer coverer;
            coverer.set_min_level(min(coarsestIndexedLevel,
                                      2 + S2::kAvgEdge.GetClosestLevel(edgeLen)));
            coverer.set_max_level(4 + coverer.min_level());

            vector<S2CellId> cover;
            coverer.GetCovering(region, &cover);

            // Look at the cells we cover and all cells that are within our covering and finer.
            // Anything with our cover as a strict prefix is contained within the cover and should
            // be intersection tested.
            set<string> intervalSet;
            set<string> exactSet;
            for (size_t i = 0; i < cover.size(); ++i) {

                S2CellId coveredCell = cover[i];
                intervalSet.insert(coveredCell.toString());

                // Look at the cells that cover us.  We want to look at every cell that contains the
                // covering we would index on if we were to insert the query geometry.  We generate
                // the would-index-with-this-covering and find all the cells strictly containing the
                // cells in that set, until we hit the coarsest indexed cell.  We use equality, not
                // a prefix match.  Why not prefix?  Because we've already looked at everything
                // finer or as fine as our initial covering.
                //
                // Say we have a fine point with cell id 212121, we go up one, get 21212, we don't
                // want to look at cells 21212[not-1] because we know they're not going to intersect
                // with 212121, but entries inserted with cell value 21212 (no trailing digits) may.
                // And we've already looked at points with the cell id 211111 from the regex search
                // created above, so we only want things where the value of the last digit is not
                // stored (and therefore could be 1).

                while (coveredCell.level() > coarsestIndexedLevel) {

                    // Add the parent cell of the currently covered cell since we aren't at the
                    // coarsest level yet
                    // NOTE: Be careful not to generate cells strictly less than the
                    // coarsestIndexedLevel - this can result in S2 failures when level < 0.

                    coveredCell = coveredCell.parent();
                    exactSet.insert(coveredCell.toString());
                }
            }

            // We turned the cell IDs into strings which define point intervals or prefixes of
            // strings we want to look for.
            set<string>::iterator exactIt = exactSet.begin();
            set<string>::iterator intervalIt = intervalSet.begin();
            while (exactSet.end() != exactIt && intervalSet.end() != intervalIt) {
                const string& exact = *exactIt;
                const string& ival = *intervalIt;
                if (exact < ival) {
                    // add exact
                    oilOut->intervals.push_back(IndexBoundsBuilder::makePointInterval(exact));
                    exactIt++;
                }
                else {
                    string end = ival;
                    end[end.size() - 1]++;
                    oilOut->intervals.push_back(
                        IndexBoundsBuilder::makeRangeInterval(ival, end, true, false));
                    intervalIt++;
                }
            }

            if (exactSet.end() != exactIt) {
                verify(intervalSet.end() == intervalIt);
                do {
                    oilOut->intervals.push_back(IndexBoundsBuilder::makePointInterval(*exactIt));
                    exactIt++;
                } while (exactSet.end() != exactIt);
            }
            else if (intervalSet.end() != intervalIt) {
                verify(exactSet.end() == exactIt);
                do {
                    const string& ival = *intervalIt;
                    string end = ival;
                    end[end.size() - 1]++;
                    oilOut->intervals.push_back(
                        IndexBoundsBuilder::makeRangeInterval(ival, end, true, false));
                    intervalIt++;
                } while (intervalSet.end() != intervalIt);
            }

            // Make sure that our intervals don't overlap each other and are ordered correctly.
            // This perhaps should only be done in debug mode.
            if (!oilOut->isValidFor(1)) {
                cout << "check your assumptions! OIL = " << oilOut->toString() << endl;
                verify(0);
            }
        }
    };

}  // namespace mongo
