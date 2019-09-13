
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

#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_common.h"

/**
 * Examines all documents in a given radius of a given point.
 * Returns all documents that match a given search restriction.
 * See http://dochub.mongodb.org/core/haystackindexes
 *
 * Use when you want to look for restaurants within 25 miles with a certain name.
 * Don't use when you want to find the closest open restaurants.
 */
namespace mongo {

using std::string;
using std::vector;

class GeoHaystackSearchCommand : public ErrmsgCommandDeprecated {
public:
    GeoHaystackSearchCommand() : ErrmsgCommandDeprecated("geoSearch") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const final {
        return true;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        const NamespaceString nss = CommandHelpers::parseNsCollectionRequired(dbname, cmdObj);

        AutoGetCollectionForReadCommand ctx(opCtx, nss);

        // Check whether we are allowed to read from this node after acquiring our locks.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        uassertStatusOK(replCoord->checkCanServeReadsFor(
            opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

        Collection* collection = ctx.getCollection();
        if (!collection) {
            errmsg = "can't find ns";
            return false;
        }

        vector<IndexDescriptor*> idxs;
        collection->getIndexCatalog()->findIndexByType(opCtx, IndexNames::GEO_HAYSTACK, idxs);
        if (idxs.size() == 0) {
            errmsg = "no geoSearch index";
            return false;
        }
        if (idxs.size() > 1) {
            errmsg = "more than 1 geosearch index";
            return false;
        }

        BSONElement nearElt = cmdObj["near"];
        BSONElement maxDistance = cmdObj["maxDistance"];
        BSONElement search = cmdObj["search"];

        uassert(13318, "near needs to be an array", nearElt.isABSONObj());
        uassert(13319, "maxDistance needs a number", maxDistance.isNumber());
        uassert(13320, "search needs to be an object", search.type() == Object);

        unsigned limit = 50;
        if (cmdObj["limit"].isNumber())
            limit = static_cast<unsigned>(cmdObj["limit"].numberInt());

        IndexDescriptor* desc = idxs[0];
        HaystackAccessMethod* ham =
            static_cast<HaystackAccessMethod*>(collection->getIndexCatalog()->getIndex(desc));
        ham->searchCommand(opCtx,
                           collection,
                           nearElt.Obj(),
                           maxDistance.numberDouble(),
                           search.Obj(),
                           &result,
                           limit);
        return 1;
    }
} nameSearchCommand;

}  // namespace mongo
