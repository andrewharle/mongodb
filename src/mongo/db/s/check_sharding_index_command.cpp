
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::unique_ptr;

namespace dps = ::mongo::dotted_path_support;

namespace {

class CheckShardingIndex : public ErrmsgCommandDeprecated {
public:
    CheckShardingIndex() : ErrmsgCommandDeprecated("checkShardingIndex") {}

    std::string help() const override {
        return "Internal command.\n";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& jsobj,
                   std::string& errmsg,
                   BSONObjBuilder& result) {
        const NamespaceString nss = NamespaceString(parseNs(dbname, jsobj));

        BSONObj keyPattern = jsobj.getObjectField("keyPattern");
        if (keyPattern.isEmpty()) {
            errmsg = "no key pattern found in checkShardingindex";
            return false;
        }

        if (keyPattern.nFields() == 1 && str::equals("_id", keyPattern.firstElementFieldName())) {
            result.appendBool("idskip", true);
            return true;
        }

        BSONObj min = jsobj.getObjectField("min");
        BSONObj max = jsobj.getObjectField("max");
        if (min.isEmpty() != max.isEmpty()) {
            errmsg = "either provide both min and max or leave both empty";
            return false;
        }

        AutoGetCollection autoColl(opCtx, nss, MODE_IS);

        Collection* const collection = autoColl.getCollection();
        if (!collection) {
            errmsg = "ns not found";
            return false;
        }

        IndexDescriptor* idx =
            collection->getIndexCatalog()->findShardKeyPrefixedIndex(opCtx,
                                                                     keyPattern,
                                                                     true);  // requireSingleKey
        if (idx == NULL) {
            errmsg = "couldn't find valid index for shard key";
            return false;
        }
        // extend min to get (min, MinKey, MinKey, ....)
        KeyPattern kp(idx->keyPattern());
        min = Helpers::toKeyFormat(kp.extendRangeBound(min, false));
        if (max.isEmpty()) {
            // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
            max = Helpers::toKeyFormat(kp.extendRangeBound(max, true));
        } else {
            // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
            max = Helpers::toKeyFormat(kp.extendRangeBound(max, false));
        }

        auto exec = InternalPlanner::indexScan(opCtx,
                                               collection,
                                               idx,
                                               min,
                                               max,
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               PlanExecutor::YIELD_AUTO,
                                               InternalPlanner::FORWARD);

        // Find the 'missingField' value used to represent a missing document field in a key of
        // this index.
        // NOTE A local copy of 'missingField' is made because indices may be
        // invalidated during a db lock yield.
        BSONObj missingFieldObj = IndexLegacy::getMissingField(opCtx, collection, idx->infoObj());
        BSONElement missingField = missingFieldObj.firstElement();

        // for now, the only check is that all shard keys are filled
        // a 'missingField' valued index key is ok if the field is present in the document,
        // TODO if $exist for nulls were picking the index, it could be used instead efficiently
        int keyPatternLength = keyPattern.nFields();

        RecordId loc;
        BSONObj currKey;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&currKey, &loc))) {
            // check that current key contains non missing elements for all fields in keyPattern
            BSONObjIterator i(currKey);
            for (int k = 0; k < keyPatternLength; k++) {
                if (!i.more()) {
                    errmsg = str::stream() << "index key " << currKey << " too short for pattern "
                                           << keyPattern;
                    return false;
                }
                BSONElement currKeyElt = i.next();

                const StringData::ComparatorInterface* stringComparator = nullptr;
                BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore,
                                             stringComparator);
                if (!currKeyElt.eoo() && eltCmp.evaluate(currKeyElt != missingField))
                    continue;

                // This is a fetch, but it's OK.  The underlying code won't throw a page fault
                // exception.
                BSONObj obj = collection->docFor(opCtx, loc).value();
                BSONObjIterator j(keyPattern);
                BSONElement real;
                for (int x = 0; x <= k; x++)
                    real = j.next();

                real = dps::extractElementAtPath(obj, real.fieldName());

                if (real.type())
                    continue;

                const string msg = str::stream()
                    << "There are documents which have missing or incomplete shard key fields ("
                    << redact(currKey) << "). Please ensure that all documents in the collection "
                                          "include all fields from the shard key.";
                log() << "checkShardingIndex for '" << nss.toString() << "' failed: " << msg;

                errmsg = msg;
                return false;
            }
        }

        if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
            uassertStatusOK(WorkingSetCommon::getMemberObjectStatus(currKey).withContext(
                "Executor error while checking sharding index"));
        }

        return true;
    }

} cmdCheckShardingIndex;

}  // namespace
}  // namespace mongo
