
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

#pragma once

#include <memory>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/db/query/canonical_query.h"

namespace mongo {

class BSONObj;
class ExtensionsCallback;
class NamespaceString;
class OperationContext;

/**
 * The parsed form of the distinct command request.
 */
class ParsedDistinct {
public:
    static const char kKeyField[];
    static const char kQueryField[];
    static const char kCollationField[];
    static const char kCommentField[];

    ParsedDistinct(std::unique_ptr<CanonicalQuery> query, const std::string key)
        : _query(std::move(query)), _key(std::move(key)) {}

    const CanonicalQuery* getQuery() const {
        return _query.get();
    }

    /**
     * Releases ownership of the canonical query to the caller.
     */
    std::unique_ptr<CanonicalQuery> releaseQuery() {
        invariant(_query.get());
        return std::move(_query);
    }

    const std::string& getKey() const {
        return _key;
    }

    /**
     * Convert this ParsedDistinct into an aggregation command object.
     */
    StatusWith<BSONObj> asAggregationCommand() const;

    /**
     * 'extensionsCallback' allows for additional mongod parsing. If called from mongos, an
     * ExtensionsCallbackNoop object should be passed to skip this parsing.
     */
    static StatusWith<ParsedDistinct> parse(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const BSONObj& cmdObj,
                                            const ExtensionsCallback& extensionsCallback,
                                            bool isExplain);

private:
    std::unique_ptr<CanonicalQuery> _query;

    // The field for which we are getting distinct values.
    const std::string _key;
};

}  // namespace mongo
