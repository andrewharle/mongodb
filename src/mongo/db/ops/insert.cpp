// insert.cpp


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

#include "mongo/db/ops/insert.h"

#include <vector>

#include "mongo/bson/bson_depth.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

using namespace mongoutils;

namespace {
/**
 * Validates the nesting depth of 'obj', returning a non-OK status if it exceeds the limit.
 */
Status validateDepth(const BSONObj& obj) {
    std::vector<BSONObjIterator> frames;
    frames.reserve(16);
    frames.emplace_back(obj);

    while (!frames.empty()) {
        const auto elem = frames.back().next();
        if (elem.type() == BSONType::Object || elem.type() == BSONType::Array) {
            if (MONGO_unlikely(frames.size() == BSONDepth::getMaxDepthForUserStorage())) {
                // We're exactly at the limit, so descending to the next level would exceed
                // the maximum depth.
                return {ErrorCodes::Overflow,
                        str::stream() << "cannot insert document because it exceeds "
                                      << BSONDepth::getMaxDepthForUserStorage()
                                      << " levels of nesting"};
            }
            frames.emplace_back(elem.embeddedObject());
        }

        if (!frames.back().more()) {
            frames.pop_back();
        }
    }

    return Status::OK();
}
}  // namespace

StatusWith<BSONObj> fixDocumentForInsert(ServiceContext* service, const BSONObj& doc) {
    if (doc.objsize() > BSONObjMaxUserSize)
        return StatusWith<BSONObj>(ErrorCodes::BadValue,
                                   str::stream() << "object to insert too large"
                                                 << ". size in bytes: "
                                                 << doc.objsize()
                                                 << ", max size: "
                                                 << BSONObjMaxUserSize);

    auto depthStatus = validateDepth(doc);
    if (!depthStatus.isOK()) {
        return depthStatus;
    }

    bool firstElementIsId = false;
    bool hasTimestampToFix = false;
    bool hadId = false;
    {
        BSONObjIterator i(doc);
        for (bool isFirstElement = true; i.more(); isFirstElement = false) {
            BSONElement e = i.next();

            if (e.type() == bsonTimestamp && e.timestampValue() == 0) {
                // we replace Timestamp(0,0) at the top level with a correct value
                // in the fast pass, we just mark that we want to swap
                hasTimestampToFix = true;
            }

            auto fieldName = e.fieldNameStringData();

            if (fieldName[0] == '$') {
                return StatusWith<BSONObj>(
                    ErrorCodes::BadValue,
                    str::stream() << "Document can't have $ prefixed field names: " << fieldName);
            }

            // check no regexp for _id (SERVER-9502)
            // also, disallow undefined and arrays
            // Make sure _id isn't duplicated (SERVER-19361).
            if (fieldName == "_id") {
                if (e.type() == RegEx) {
                    return StatusWith<BSONObj>(ErrorCodes::BadValue, "can't use a regex for _id");
                }
                if (e.type() == Undefined) {
                    return StatusWith<BSONObj>(ErrorCodes::BadValue,
                                               "can't use a undefined for _id");
                }
                if (e.type() == Array) {
                    return StatusWith<BSONObj>(ErrorCodes::BadValue, "can't use an array for _id");
                }
                if (e.type() == Object) {
                    BSONObj o = e.Obj();
                    Status s = o.storageValidEmbedded();
                    if (!s.isOK())
                        return StatusWith<BSONObj>(s);
                }
                if (hadId) {
                    return StatusWith<BSONObj>(ErrorCodes::BadValue,
                                               "can't have multiple _id fields in one document");
                } else {
                    hadId = true;
                    firstElementIsId = isFirstElement;
                }
            }
        }
    }

    if (firstElementIsId && !hasTimestampToFix)
        return StatusWith<BSONObj>(BSONObj());

    BSONObjIterator i(doc);

    BSONObjBuilder b(doc.objsize() + 16);
    if (firstElementIsId) {
        b.append(doc.firstElement());
        i.next();
    } else {
        BSONElement e = doc["_id"];
        if (e.type()) {
            b.append(e);
        } else {
            b.appendOID("_id", NULL, true);
        }
    }

    while (i.more()) {
        BSONElement e = i.next();
        if (hadId && e.fieldNameStringData() == "_id") {
            // no-op
        } else if (e.type() == bsonTimestamp && e.timestampValue() == 0) {
            auto nextTime = LogicalClock::get(service)->reserveTicks(1);
            b.append(e.fieldName(), nextTime.asTimestamp());
        } else {
            b.append(e);
        }
    }
    return StatusWith<BSONObj>(b.obj());
}

Status userAllowedWriteNS(StringData ns) {
    return userAllowedWriteNS(nsToDatabaseSubstring(ns), nsToCollectionSubstring(ns));
}

Status userAllowedWriteNS(const NamespaceString& ns) {
    return userAllowedWriteNS(ns.db(), ns.coll());
}

Status userAllowedWriteNS(StringData db, StringData coll) {
    if (coll == "system.profile") {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "cannot write to '" << db << ".system.profile'");
    }
    if (coll == "system.indexes") {
        return Status::OK();
    }
    return userAllowedCreateNS(db, coll);
}

Status userAllowedCreateNS(StringData db, StringData coll) {
    // validity checking

    if (db.size() == 0)
        return Status(ErrorCodes::InvalidNamespace, "db cannot be blank");

    if (!NamespaceString::validDBName(db, NamespaceString::DollarInDbNameBehavior::Allow))
        return Status(ErrorCodes::InvalidNamespace, "invalid db name");

    if (coll.size() == 0)
        return Status(ErrorCodes::InvalidNamespace, "collection cannot be blank");

    if (!NamespaceString::validCollectionName(coll))
        return Status(ErrorCodes::InvalidNamespace, "invalid collection name");

    if (db.size() + 1 /* dot */ + coll.size() > NamespaceString::MaxNsCollectionLen)
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "fully qualified namespace " << db << '.' << coll
                                    << " is too long "
                                    << "(max is "
                                    << NamespaceString::MaxNsCollectionLen
                                    << " bytes)");

    // check spceial areas

    if (db == "system")
        return Status(ErrorCodes::InvalidNamespace, "cannot use 'system' database");


    if (coll.startsWith("system.")) {
        if (coll == "system.js")
            return Status::OK();
        if (coll == "system.profile")
            return Status::OK();
        if (coll == "system.users")
            return Status::OK();
        if (coll == DurableViewCatalog::viewsCollectionName())
            return Status::OK();
        if (db == "admin") {
            if (coll == "system.version")
                return Status::OK();
            if (coll == "system.roles")
                return Status::OK();
            if (coll == "system.new_users")
                return Status::OK();
            if (coll == "system.backup_users")
                return Status::OK();
            if (coll == "system.keys")
                return Status::OK();
        }
        if (db == "config") {
            if (coll == "system.sessions")
                return Status::OK();
        }
        if (db == "local") {
            if (coll == "system.replset")
                return Status::OK();
            if (coll == "system.healthlog")
                return Status::OK();
        }
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "cannot write to '" << db << "." << coll << "'");
    }

    // some special rules

    if (coll.find(".system.") != string::npos) {
        // Writes are permitted to the persisted chunk metadata collections. These collections are
        // named based on the name of the sharded collection, e.g.
        // 'config.cache.chunks.dbname.collname'. Since there is a sharded collection
        // 'config.system.sessions', there will be a corresponding persisted chunk metadata
        // collection 'config.cache.chunks.config.system.sessions'. We wish to allow writes to this
        // collection.
        if (coll.find(".system.sessions") != string::npos) {
            return Status::OK();
        }

        // this matches old (2.4 and older) behavior, but I'm not sure its a good idea
        return Status(ErrorCodes::BadValue,
                      str::stream() << "cannot write to '" << db << "." << coll << "'");
    }

    return Status::OK();
}
}
