//@file update.h


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

#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"

namespace mongo {

class CanonicalQuery;
class Database;
class OperationContext;
class UpdateDriver;

/**
 * Utility method to execute an update described by "request".
 *
 * Caller must hold the appropriate database locks.
 */
UpdateResult update(OperationContext* opCtx, Database* db, const UpdateRequest& request);

/**
 * Takes the 'from' document and returns a new document after applying 'operators'. arrayFilters are
 * not supported.
 * e.g.
 *   applyUpdateOperators( BSON( "x" << 1 ) , BSON( "$inc" << BSON( "x" << 1 ) ) );
 *   returns: { x : 2 }
 */
BSONObj applyUpdateOperators(OperationContext* opCtx,
                             const BSONObj& from,
                             const BSONObj& operators);
}  // namespace mongo
