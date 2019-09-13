
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

#include "mongo/db/update/modifier_table.h"

#include <string>
#include <utility>

#include "mongo/base/init.h"
#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/base/status.h"
#include "mongo/db/update/addtoset_node.h"
#include "mongo/db/update/arithmetic_node.h"
#include "mongo/db/update/bit_node.h"
#include "mongo/db/update/compare_node.h"
#include "mongo/db/update/conflict_placeholder_node.h"
#include "mongo/db/update/current_date_node.h"
#include "mongo/db/update/pop_node.h"
#include "mongo/db/update/pull_node.h"
#include "mongo/db/update/pullall_node.h"
#include "mongo/db/update/push_node.h"
#include "mongo/db/update/rename_node.h"
#include "mongo/db/update/set_node.h"
#include "mongo/db/update/unset_node.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::make_pair;
using std::string;

namespace modifiertable {

namespace {

struct ModifierEntry {
    string name;
    ModifierType type;

    ModifierEntry(StringData name, ModifierType type) : name(name.toString()), type(type) {}
};

typedef StringDataUnorderedMap<ModifierEntry*> NameMap;

NameMap* MODIFIER_NAME_MAP;

void init(NameMap* nameMap) {
    ModifierEntry* entryAddToSet = new ModifierEntry("$addToSet", MOD_ADD_TO_SET);
    nameMap->insert(make_pair(StringData(entryAddToSet->name), entryAddToSet));

    ModifierEntry* entryBit = new ModifierEntry("$bit", MOD_BIT);
    nameMap->insert(make_pair(StringData(entryBit->name), entryBit));

    ModifierEntry* entryCurrentDate = new ModifierEntry("$currentDate", MOD_CURRENTDATE);
    nameMap->insert(make_pair(StringData(entryCurrentDate->name), entryCurrentDate));

    ModifierEntry* entryInc = new ModifierEntry("$inc", MOD_INC);
    nameMap->insert(make_pair(StringData(entryInc->name), entryInc));

    ModifierEntry* entryMax = new ModifierEntry("$max", MOD_MAX);
    nameMap->insert(make_pair(StringData(entryMax->name), entryMax));

    ModifierEntry* entryMin = new ModifierEntry("$min", MOD_MIN);
    nameMap->insert(make_pair(StringData(entryMin->name), entryMin));

    ModifierEntry* entryMul = new ModifierEntry("$mul", MOD_MUL);
    nameMap->insert(make_pair(StringData(entryMul->name), entryMul));

    ModifierEntry* entryPop = new ModifierEntry("$pop", MOD_POP);
    nameMap->insert(make_pair(StringData(entryPop->name), entryPop));

    ModifierEntry* entryPull = new ModifierEntry("$pull", MOD_PULL);
    nameMap->insert(make_pair(StringData(entryPull->name), entryPull));

    ModifierEntry* entryPullAll = new ModifierEntry("$pullAll", MOD_PULL_ALL);
    nameMap->insert(make_pair(StringData(entryPullAll->name), entryPullAll));

    ModifierEntry* entryPush = new ModifierEntry("$push", MOD_PUSH);
    nameMap->insert(make_pair(StringData(entryPush->name), entryPush));

    ModifierEntry* entrySet = new ModifierEntry("$set", MOD_SET);
    nameMap->insert(make_pair(StringData(entrySet->name), entrySet));

    ModifierEntry* entrySetOnInsert = new ModifierEntry("$setOnInsert", MOD_SET_ON_INSERT);
    nameMap->insert(make_pair(StringData(entrySetOnInsert->name), entrySetOnInsert));

    ModifierEntry* entryRename = new ModifierEntry("$rename", MOD_RENAME);
    nameMap->insert(make_pair(StringData(entryRename->name), entryRename));

    ModifierEntry* entryUnset = new ModifierEntry("$unset", MOD_UNSET);
    nameMap->insert(make_pair(StringData(entryUnset->name), entryUnset));
}

}  // unnamed namespace

MONGO_INITIALIZER(ModifierTable)(InitializerContext* context) {
    MODIFIER_NAME_MAP = new NameMap(
        SimpleStringDataComparator::kInstance.makeStringDataUnorderedMap<ModifierEntry*>());
    init(MODIFIER_NAME_MAP);

    return Status::OK();
}

ModifierType getType(StringData typeStr) {
    NameMap::const_iterator it = MODIFIER_NAME_MAP->find(typeStr);
    if (it == MODIFIER_NAME_MAP->end()) {
        return MOD_UNKNOWN;
    }
    return it->second->type;
}

std::unique_ptr<UpdateLeafNode> makeUpdateLeafNode(ModifierType modType) {
    switch (modType) {
        case MOD_ADD_TO_SET:
            return stdx::make_unique<AddToSetNode>();
        case MOD_BIT:
            return stdx::make_unique<BitNode>();
        case MOD_CONFLICT_PLACEHOLDER:
            return stdx::make_unique<ConflictPlaceholderNode>();
        case MOD_CURRENTDATE:
            return stdx::make_unique<CurrentDateNode>();
        case MOD_INC:
            return stdx::make_unique<ArithmeticNode>(ArithmeticNode::ArithmeticOp::kAdd);
        case MOD_MAX:
            return stdx::make_unique<CompareNode>(CompareNode::CompareMode::kMax);
        case MOD_MIN:
            return stdx::make_unique<CompareNode>(CompareNode::CompareMode::kMin);
        case MOD_MUL:
            return stdx::make_unique<ArithmeticNode>(ArithmeticNode::ArithmeticOp::kMultiply);
        case MOD_POP:
            return stdx::make_unique<PopNode>();
        case MOD_PULL:
            return stdx::make_unique<PullNode>();
        case MOD_PULL_ALL:
            return stdx::make_unique<PullAllNode>();
        case MOD_PUSH:
            return stdx::make_unique<PushNode>();
        case MOD_RENAME:
            return stdx::make_unique<RenameNode>();
        case MOD_SET:
            return stdx::make_unique<SetNode>();
        case MOD_SET_ON_INSERT:
            return stdx::make_unique<SetNode>(UpdateNode::Context::kInsertOnly);
        case MOD_UNSET:
            return stdx::make_unique<UnsetNode>();
        default:
            return nullptr;
    }
}

}  // namespace modifiertable
}  // namespace mongo
