/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

//
// ServerStatus metric cursor counts.
//

class ClusterCursorStats final : public ServerStatusMetric {
public:
    ClusterCursorStats() : ServerStatusMetric("cursor.open") {}

    void appendAtLeaf(BSONObjBuilder& b) const final {
        BSONObjBuilder openBob(b.subobjStart(_leafName));
        auto stats = grid.getCursorManager()->stats();

        openBob.append("multiTarget", static_cast<long long>(stats.cursorsSharded));
        openBob.append("singleTarget", static_cast<long long>(stats.cursorsNotSharded));
        openBob.append("pinned", static_cast<long long>(stats.cursorsPinned));
        openBob.append("total",
                       static_cast<long long>(stats.cursorsSharded + stats.cursorsNotSharded));
        openBob.done();
    }
} clusterCursorStats;

}  // namespace
}  // namespace mongo
