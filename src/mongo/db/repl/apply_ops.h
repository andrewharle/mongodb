
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

#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/apply_ops_gen.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"

namespace mongo {
class BSONObjBuilder;
class OperationContext;

namespace repl {
class ApplyOps {
public:
    static constexpr StringData kPreconditionFieldName = "preCondition"_sd;
    static constexpr StringData kOplogApplicationModeFieldName = "oplogApplicationMode"_sd;

    /**
     * Extracts CRUD operations from an atomic applyOps oplog entry.
     * Throws UserException on error.
     */
    static MultiApplier::Operations extractOperations(const OplogEntry& applyOpsOplogEntry);
};

/**
 * Holds information about an applyOps command object.
 */
class ApplyOpsCommandInfo : public ApplyOpsCommandInfoBase {
public:
    /**
     * Parses the object in the 'o' field of an applyOps command.
     * May throw UserException.
     */
    static ApplyOpsCommandInfo parse(const BSONObj& applyOpCmd);

    /**
     * Returns true if all operations described by this applyOps command are CRUD only.
     */
    bool areOpsCrudOnly() const;

    /**
     * Returns true if applyOps will try to process all operations in a single batch atomically.
     * Derived from getAllowAtomic() and areOpsCrudOnly().
     */
    bool isAtomic() const;

private:
    explicit ApplyOpsCommandInfo(const BSONObj& applyOpCmd);

    const bool _areOpsCrudOnly;
};

/**
 * Applies ops contained in 'applyOpCmd' and populates fields in 'result' to be returned to the
 * caller. The information contained in 'result' can be returned to the user if called as part
 * of the execution of an 'applyOps' command.
 *
 * The 'oplogApplicationMode' argument determines the semantics of the operations contained within
 * the given command object. This function may be called as part of a direct user invocation of the
 * 'applyOps' command, or as part of the application of an 'applyOps' oplog operation. In either
 * case, the mode can be set to determine how the internal ops are executed.
 */
Status applyOps(OperationContext* opCtx,
                const std::string& dbName,
                const BSONObj& applyOpCmd,
                repl::OplogApplication::Mode oplogApplicationMode,
                BSONObjBuilder* result);

}  // namespace repl
}  // namespace mongo
