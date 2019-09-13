// @file cpuprofile.cpp


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

/**
 * This module provides commands for starting and stopping the Google perftools
 * cpu profiler linked into mongod.
 *
 * The following command enables the not-currently-enabled profiler, and writes
 * the profile data to the specified "profileFilename."
 *     { _cpuProfilerStart: { profileFilename: '/path/on/mongod-host.prof' } }
 *
 * The following command disables the already-enabled profiler:
 *     { _cpuProfilerStop: 1}
 *
 * The commands defined here, and profiling, are only available when enabled at
 * build-time with the "--use-cpu-profiler" argument to scons.
 *
 * Example SCons command line:
 *
 *     scons --release --use-cpu-profiler
 */

#include "gperftools/profiler.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"

namespace mongo {

namespace {

/**
 * Common code for the implementation of cpu profiler commands.
 */
class CpuProfilerCommand : public ErrmsgCommandDeprecated {
public:
    CpuProfilerCommand(char const* name) : ErrmsgCommandDeprecated(name) {}
    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool adminOnly() const override {
        return true;
    }
    bool localHostOnlyIfNoAuth() const override {
        return true;
    }
    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::cpuProfiler);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    // This is an abuse of the global dbmutex.  We only really need to
    // ensure that only one cpuprofiler command runs at once; it would
    // be fine for it to run concurrently with other operations.
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
};

/**
 * Class providing implementation of the _cpuProfilerStart command.
 */
class CpuProfilerStartCommand : public CpuProfilerCommand {
public:
    CpuProfilerStartCommand() : CpuProfilerCommand(commandName) {}

    virtual bool errmsgRun(OperationContext* opCtx,
                           std::string const& db,
                           const BSONObj& cmdObj,
                           std::string& errmsg,
                           BSONObjBuilder& result);

    static char const* const commandName;
} cpuProfilerStartCommandInstance;

/**
 * Class providing implementation of the _cpuProfilerStop command.
 */
class CpuProfilerStopCommand : public CpuProfilerCommand {
public:
    CpuProfilerStopCommand() : CpuProfilerCommand(commandName) {}

    virtual bool errmsgRun(OperationContext* opCtx,
                           std::string const& db,
                           const BSONObj& cmdObj,
                           std::string& errmsg,
                           BSONObjBuilder& result);

    static char const* const commandName;
} cpuProfilerStopCommandInstance;

char const* const CpuProfilerStartCommand::commandName = "_cpuProfilerStart";
char const* const CpuProfilerStopCommand::commandName = "_cpuProfilerStop";

bool CpuProfilerStartCommand::errmsgRun(OperationContext* opCtx,
                                        std::string const& db,
                                        const BSONObj& cmdObj,
                                        std::string& errmsg,
                                        BSONObjBuilder& result) {
    // The DB lock here is just so we have IX on the global lock in order to prevent shutdown
    Lock::DBLock dbXLock(opCtx, db, MODE_X);
    OldClientContext ctx(opCtx, db, false /* no shard version checking */);

    std::string profileFilename = cmdObj[commandName]["profileFilename"].String();
    if (!::ProfilerStart(profileFilename.c_str())) {
        errmsg = "Failed to start profiler";
        return false;
    }
    return true;
}

bool CpuProfilerStopCommand::errmsgRun(OperationContext* opCtx,
                                       std::string const& db,
                                       const BSONObj& cmdObj,
                                       std::string& errmsg,
                                       BSONObjBuilder& result) {
    // The DB lock here is just so we have IX on the global lock in order to prevent shutdown
    Lock::DBLock dbXLock(opCtx, db, MODE_X);
    OldClientContext ctx(opCtx, db, false /* no shard version checking */);

    ::ProfilerStop();
    return true;
}

}  // namespace

}  // namespace mongo
