
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

#include <boost/filesystem/convenience.hpp>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/platform/process_id.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

class Scope;

namespace shell_utils {

// Scoped management of mongo program instances.  Simple implementation:
// destructor kills all mongod instances created by the shell.
struct MongoProgramScope {
    MongoProgramScope() {}  // Avoid 'unused variable' warning.
    ~MongoProgramScope();
};
int KillMongoProgramInstances();

// Returns true if there are running child processes.
std::vector<ProcessId> getRunningMongoChildProcessIds();

void installShellUtilsLauncher(Scope& scope);

/** Record log lines from concurrent programs.  All public members are thread safe. */
class ProgramOutputMultiplexer {
public:
    void appendLine(int port, ProcessId pid, const std::string& name, const std::string& line);
    /** @return up to 100000 characters of the most recent log output. */
    std::string str() const;
    void clear();

private:
    std::stringstream _buffer;
};

/**
 * A registry of spawned programs that are identified by a bound port or else a system pid.
 * All public member functions are thread safe.
 */
class ProgramRegistry {
public:
    bool isPortRegistered(int port) const;
    /** @return pid for a registered port. */
    ProcessId pidForPort(int port) const;
    /** @return port (-1 if doesn't exist) for a registered pid. */
    int portForPid(ProcessId pid) const;
    /** Register an unregistered program. */
    void registerProgram(ProcessId pid, int port = -1);
    /** Registers the reader thread for the PID. Must be called before `joinReaderThread`. */
    void registerReaderThread(ProcessId pid, stdx::thread reader);
    /** Closes the registered program's write pipe and waits for all of the written output to be
     * consumed by the reader thread, then removes the program from the registry */
    void unregisterProgram(ProcessId pid);

    bool isPidRegistered(ProcessId pid) const;
    void getRegisteredPorts(std::vector<int>& ports);
    void getRegisteredPids(std::vector<ProcessId>& pids);

private:
    stdx::unordered_set<ProcessId> _registeredPids;
    stdx::unordered_map<int, ProcessId> _portToPidMap;
    stdx::unordered_map<ProcessId, stdx::thread> _outputReaderThreads;
    mutable stdx::recursive_mutex _mutex;

#ifdef _WIN32
private:
    std::map<ProcessId, HANDLE> _handles;

public:
    HANDLE getHandleForPid(ProcessId pid);
    void eraseHandleForPid(ProcessId pid);
    std::size_t countHandleForPid(ProcessId pid);
    void insertHandleForPid(ProcessId pid, HANDLE handle);

#endif
};

/** Helper class for launching a program and logging its output. */
class ProgramRunner {
public:
    /** @param args The program's arguments, including the program name.
     *  @param env Environment to run the program with, which will override any set by the local
     *             environment
     * @param isMongo Indicator variable, true if runs as a mongo process.
     */
    ProgramRunner(const BSONObj& args, const BSONObj& env, bool isMongo);
    /** Launch the program. */
    void start();
    /** Continuously read the program's output, generally from a special purpose thread. */
    void operator()();
    ProcessId pid() const {
        return _pid;
    }
    int port() const {
        return _port;
    }

private:
    boost::filesystem::path findProgram(const std::string& prog);
    void launchProcess(int child_stdout);

    std::vector<std::string> _argv;
    std::map<std::string, std::string> _envp;
    int _port;
    int _pipe;
    ProcessId _pid;
    std::string _name;
};
}  // namespace shell_utils
}  // namespace mongo
