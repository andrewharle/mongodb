// mongo/shell/shell_utils_launcher.cpp
/*
 *    Copyright 2010 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/shell/shell_utils_launcher.h"

#include <algorithm>
#include <array>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/iostreams/tee.hpp>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <map>
#include <signal.h>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define SIGKILL 9
#else
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "mongo/client/dbclientinterface.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_win32.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/text.h"

#ifndef _WIN32
extern char** environ;
#endif

namespace mongo {

using std::unique_ptr;
using std::cout;
using std::endl;
using std::make_pair;
using std::map;
using std::pair;
using std::string;
using std::stringstream;
using std::vector;

#ifdef _WIN32
inline int close(int fd) {
    return _close(fd);
}
inline int read(int fd, void* buf, size_t size) {
    return _read(fd, buf, size);
}
inline int pipe(int fds[2]) {
    return _pipe(fds, 4096, _O_TEXT | _O_NOINHERIT);
}
#endif

/**
 * These utilities are thread safe but do not provide mutually exclusive access to resources
 * identified by the caller.  Resources identified by a pid or port should not be accessed
 * by different threads.  Dependent filesystem paths should not be accessed by different
 * threads.
 */
namespace shell_utils {

namespace {
void safeClose(int fd) {
#ifndef _WIN32
    struct ScopedSignalBlocker {
        ScopedSignalBlocker() {
            sigset_t mask;
            sigfillset(&mask);
            pthread_sigmask(SIG_SETMASK, &mask, &_oldMask);
        }

        ~ScopedSignalBlocker() {
            pthread_sigmask(SIG_SETMASK, &_oldMask, NULL);
        }

    private:
        sigset_t _oldMask;
    };
    const ScopedSignalBlocker block;
#endif
    if (close(fd) != 0) {
        const auto ewd = errnoWithDescription();
        error() << "failed to close fd " << fd << ": " << ewd;
        fassertFailed(40318);
    }
}

stdx::mutex _createProcessMtx;
}  // namespace

ProgramOutputMultiplexer programOutputLogger;

bool ProgramRegistry::isPortRegistered(int port) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    return _portToPidMap.count(port) == 1;
}

ProcessId ProgramRegistry::pidForPort(int port) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    invariant(isPortRegistered(port));
    return _portToPidMap.find(port)->second;
}

int ProgramRegistry::portForPid(ProcessId pid) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    for (const auto& portPid : _portToPidMap) {
        if (portPid.second == pid)
            return portPid.first;
    }
    return -1;
}

void ProgramRegistry::registerProgram(ProcessId pid, int port) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    invariant(!isPidRegistered(pid));
    _registeredPids.emplace(pid);
    if (port != -1) {
        _portToPidMap.emplace(port, pid);
    }
}

void ProgramRegistry::unregisterProgram(ProcessId pid) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    invariant(isPidRegistered(pid));

    _outputReaderThreads[pid].join();

    // Remove the PID from the registry.
    _outputReaderThreads.erase(pid);
    _portToPidMap.erase(portForPid(pid));
    _registeredPids.erase(pid);
}

void ProgramRegistry::registerReaderThread(ProcessId pid, stdx::thread reader) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    invariant(isPidRegistered(pid));
    invariant(_outputReaderThreads.count(pid) == 0);
    _outputReaderThreads.emplace(pid, std::move(reader));
}

void ProgramRegistry::getRegisteredPorts(vector<int>& ports) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    for (const auto& portPid : _portToPidMap) {
        ports.push_back(portPid.first);
    }
}

bool ProgramRegistry::isPidRegistered(ProcessId pid) const {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    return _registeredPids.count(pid) == 1;
}

void ProgramRegistry::getRegisteredPids(vector<ProcessId>& pids) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
    for (const auto& pid : _registeredPids) {
        pids.emplace_back(pid);
    }
}

#ifdef _WIN32
HANDLE ProgramRegistry::getHandleForPid(ProcessId pid) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);

    return _handles[pid];
}

void ProgramRegistry::eraseHandleForPid(ProcessId pid) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);

    _handles.erase(pid);
}

std::size_t ProgramRegistry::countHandleForPid(ProcessId pid) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);

    return _handles.count(pid);
}

void ProgramRegistry::insertHandleForPid(ProcessId pid, HANDLE handle) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);

    _handles.insert(make_pair(pid, handle));
}

#endif

ProgramRegistry& registry = *(new ProgramRegistry());

void ProgramOutputMultiplexer::appendLine(int port,
                                          ProcessId pid,
                                          const std::string& name,
                                          const std::string& line) {
    stdx::lock_guard<stdx::mutex> lk(mongoProgramOutputMutex);
    boost::iostreams::tee_device<std::ostream, std::stringstream> teeDevice(cout, _buffer);
    boost::iostreams::stream<decltype(teeDevice)> teeStream(teeDevice);
    if (port > 0) {
        teeStream << name << port << "| " << line << endl;
    } else {
        teeStream << name << pid << "| " << line << endl;
    }
}

string ProgramOutputMultiplexer::str() const {
    stdx::lock_guard<stdx::mutex> lk(mongoProgramOutputMutex);
    return _buffer.str();
}

void ProgramOutputMultiplexer::clear() {
    stdx::lock_guard<stdx::mutex> lk(mongoProgramOutputMutex);
    _buffer.str("");
}

ProgramRunner::ProgramRunner(const BSONObj& args, const BSONObj& env) {
    verify(!args.isEmpty());

    string program(args.firstElement().valuestrsafe());
    verify(!program.empty());
    boost::filesystem::path programPath = findProgram(program);

    string prefix("mongod-");
    bool isMongodProgram =
        string("mongod") == program || program.compare(0, prefix.size(), prefix) == 0;
    prefix = "mongos-";
    bool isMongosProgram =
        string("mongos") == program || program.compare(0, prefix.size(), prefix) == 0;

    if (isMongodProgram) {
        _name = "d";
    } else if (isMongosProgram) {
        _name = "s";
    } else if (program == "mongobridge") {
        _name = "b";
    } else {
        _name = "sh";
    }

    _argv.push_back(programPath.string());

    _port = -1;

    // Parse individual arguments into _argv
    BSONObjIterator j(args);
    j.next();  // skip program name (handled above)
    while (j.more()) {
        BSONElement e = j.next();
        string str;
        if (e.isNumber()) {
            stringstream ss;
            ss << e.number();
            str = ss.str();
        } else {
            verify(e.type() == mongo::String);
            str = e.valuestr();
        }
        if (str == "--port") {
            _port = -2;
        } else if (_port == -2) {
            _port = strtol(str.c_str(), 0, 10);
        } else if (isMongodProgram && str == "--configsvr") {
            _name = "c";
        }
        _argv.push_back(str);
    }

    // Load explicitly set environment key value pairs into _envp.
    for (const BSONElement& e : env) {
        // Environment variable values must be strings
        verify(e.type() == mongo::String);

        _envp.emplace(std::string(e.fieldName()), std::string(e.valuestr()));
    }

// Import this process' environment into _envp, for all keys that have not already been set.
// We need to do this so that the child process has all the PATH and locale variables, unless
// we explicitly override them.
#ifdef _WIN32
    wchar_t* processEnv = GetEnvironmentStringsW();
    ON_BLOCK_EXIT(
        [](wchar_t* toFree) {
            if (toFree)
                FreeEnvironmentStringsW(toFree);
        },
        processEnv);

    // Windows' GetEnvironmentStringsW returns a NULL terminated array of NULL separated
    // <key>=<value> pairs.
    while (processEnv && *processEnv) {
        std::wstring envKeyValue(processEnv);
        size_t splitPoint = envKeyValue.find('=');
        invariant(splitPoint != std::wstring::npos);
        std::string envKey = toUtf8String(envKeyValue.substr(0, splitPoint));
        std::string envValue = toUtf8String(envKeyValue.substr(splitPoint + 1));
        _envp.emplace(std::move(envKey), std::move(envValue));
        processEnv += envKeyValue.size() + 1;
    }
#else
    // environ is a POSIX defined array of char*s. Each char* in the array is a <key>=<value>\0
    // pair.
    char** environEntry = environ;
    while (*environEntry) {
        std::string envKeyValue(*environEntry);
        size_t splitPoint = envKeyValue.find('=');
        invariant(splitPoint != std::string::npos);
        std::string envKey = envKeyValue.substr(0, splitPoint);
        std::string envValue = envKeyValue.substr(splitPoint + 1);
        _envp.emplace(std::move(envKey), std::move(envValue));
        ++environEntry;
    }
#endif
    bool needsPort = isMongodProgram || isMongosProgram || (program == "mongobridge");
    if (!needsPort) {
        _port = -1;
    }
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "a port number is expected when running " << program
                          << " from the shell",
            !needsPort || _port >= 0);

    uassert(ErrorCodes::BadValue,
            str::stream() << "can't start " << program << ", port " << _port << " already in use",
            _port < 0 || !registry.isPortRegistered(_port));
}

void ProgramRunner::start() {
    int pipeEnds[2];

    {
        // NOTE(JCAREY):
        //
        // We take this lock from before our call to pipe until after we close the write side (in
        // the parent) to avoid leaking fds from threads racing around fork().  I.e.
        //
        // Thread A: calls pipe()
        // Thread B: calls fork()
        // A: sets cloexec on read and write sides
        // B: has a forked child with open fds
        // A: spawns a child thread to read it's child process's stdout
        // A: A's child process exits
        // A: wait's on A's reader thread in de-register
        // A: deadlocks forever (because the child reader thread stays in read() because of the open
        //    fd in B)
        //
        // Holding the lock for the duration of those events prevents the leaks and thus the
        // associated deadlocks.
        stdx::lock_guard<stdx::mutex> lk(_createProcessMtx);
        int status = pipe(pipeEnds);
        if (status != 0) {
            const auto ewd = errnoWithDescription();
            error() << "failed to create pipe: " << ewd;
            fassertFailed(16701);
        }
#ifndef _WIN32
        // The calls to fcntl to set CLOEXEC ensure that processes started by the process we are
        // about to fork do *not* inherit the file descriptors for the pipe. If grandchild processes
        // could inherit the FD for the pipe, than the pipe wouldn't close on child process exit. On
        // windows, instead the handle inherit flag is turned off after the call to CreateProcess.
        status = fcntl(pipeEnds[0], F_SETFD, FD_CLOEXEC);
        if (status != 0) {
            const auto ewd = errnoWithDescription();
            error() << "failed to set FD_CLOEXEC on pipe end 0: " << ewd;
            fassertFailed(40308);
        }
        status = fcntl(pipeEnds[1], F_SETFD, FD_CLOEXEC);
        if (status != 0) {
            const auto ewd = errnoWithDescription();
            error() << "failed to set FD_CLOEXEC on pipe end 1: " << ewd;
            fassertFailed(40317);
        }
#endif

        fflush(0);

        launchProcess(pipeEnds[1]);  // sets _pid

        // Close the write end of the pipe.
        safeClose(pipeEnds[1]);
    }

    if (_port >= 0) {
        registry.registerProgram(_pid, _port);
    } else {
        registry.registerProgram(_pid);
    }

    _pipe = pipeEnds[0];

    {
        stringstream ss;
        ss << "shell: started program (sh" << _pid << "): ";
        for (unsigned i = 0; i < _argv.size(); i++) {
            ss << " " << _argv[i];
        }
        log() << ss.str();
    }
}

void ProgramRunner::operator()() {
    // Send the never_close_handle flag so that we can handle closing the fd below with safeClose.
    boost::iostreams::stream_buffer<boost::iostreams::file_descriptor_source> fdBuf(
        _pipe, boost::iostreams::file_descriptor_flags::never_close_handle);
    std::istream fdStream(&fdBuf);

    std::string line;
    while (std::getline(fdStream, line)) {
        if (line.find('\0') != std::string::npos) {
            programOutputLogger.appendLine(
                _port, _pid, _name, "WARNING: mongod wrote null bytes to output");
        }
        programOutputLogger.appendLine(_port, _pid, _name, line);
    }

    // Close the read end of the pipe.
    safeClose(_pipe);
}

boost::filesystem::path ProgramRunner::findProgram(const string& prog) {
    boost::filesystem::path p = prog;

#ifdef _WIN32
    // The system programs either come versioned in the form of <utility>-<major.minor>
    // (e.g., mongorestore-2.4) or just <utility>. For windows, the appropriate extension
    // needs to be appended.
    //
    if (p.extension() != ".exe") {
        p = prog + ".exe";
    }
#endif

    // Check if the binary exists in the current working directory
    boost::filesystem::path t = boost::filesystem::current_path() / p;
    if (boost::filesystem::exists(t)) {
        return t;
    }

#ifndef _WIN32
    // On POSIX, we need to manually resolve the $PATH variable, to try and find the binary in the
    // filesystem.
    const char* cpath = getenv("PATH");
    if (!cpath) {
        // PATH was unset, so path search is implementation defined
        return t;
    }

    std::string path(cpath);
    std::vector<std::string> pathEntries;

    // PATH entries are separated by colons. Per POSIX 2013, there is no way to escape a colon in
    // an entry.
    splitStringDelim(path, &pathEntries, ':');

    for (const std::string& pathEntry : pathEntries) {
        boost::filesystem::path potentialBinary = boost::filesystem::path(pathEntry) / p;
        if (boost::filesystem::exists(potentialBinary) &&
            boost::filesystem::is_regular_file(potentialBinary) &&
            access(potentialBinary.c_str(), X_OK) == 0) {
            return potentialBinary;
        }
    }
#endif

    return p;
}

void ProgramRunner::launchProcess(int child_stdout) {
    std::vector<std::string> envStrings;
    for (const auto& envKeyValue : _envp) {
        envStrings.emplace_back(envKeyValue.first + '=' + envKeyValue.second);
    }

#ifdef _WIN32
    stringstream ss;
    for (unsigned i = 0; i < _argv.size(); i++) {
        if (i)
            ss << ' ';
        if (_argv[i].find(' ') == string::npos)
            ss << _argv[i];
        else {
            ss << '"';
            // escape all embedded quotes
            for (size_t j = 0; j < _argv[i].size(); ++j) {
                if (_argv[i][j] == '"')
                    ss << '\\';
                ss << _argv[i][j];
            }
            ss << '"';
        }
    }

    std::wstring args = toNativeString(ss.str().c_str());

    // Construct the environment block which the new process will use.
    // An environment block is a NULL terminated array of NULL terminated WCHAR strings. The
    // strings are of the form "name=value\0". Because the strings are variable length, we must
    // precompute the size of the array before we may allocate it.
    size_t environmentBlockSize = 0;
    std::vector<std::wstring> nativeEnvStrings;

    // Compute the size of the environment block, in characters. Note that we have to count
    // wchar_t characters, which we'll actually be storing in the block later, rather than UTF8
    // characters we have in _envp and need to convert.
    for (const std::string& envKeyValue : envStrings) {
        std::wstring nativeKeyValue = toNativeString(envKeyValue.c_str());
        environmentBlockSize += (nativeKeyValue.size() + 1);
        nativeEnvStrings.emplace_back(std::move(nativeKeyValue));
    }

    // Reserve space for the final NULL character which terminates the environment block
    environmentBlockSize += 1;

    auto lpEnvironment = stdx::make_unique<wchar_t[]>(environmentBlockSize);
    size_t environmentOffset = 0;
    for (const std::wstring& envKeyValue : nativeEnvStrings) {
        // Ensure there is enough room to write the string, the string's NULL byte, and the block's
        // NULL byte
        invariant(environmentOffset + envKeyValue.size() + 1 + 1 <= environmentBlockSize);
        wcscpy_s(
            lpEnvironment.get() + environmentOffset, envKeyValue.size() + 1, envKeyValue.c_str());
        environmentOffset += envKeyValue.size();
        std::memset(lpEnvironment.get() + environmentOffset, 0, sizeof(wchar_t));
        environmentOffset += 1;
    }
    std::memset(lpEnvironment.get() + environmentOffset, 0, sizeof(wchar_t));

    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(child_stdout));
    invariant(h != INVALID_HANDLE_VALUE);
    invariant(SetHandleInformation(h, HANDLE_FLAG_INHERIT, 1));

    STARTUPINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = h;
    si.hStdOutput = h;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    DWORD dwCreationFlags = 0;
    dwCreationFlags |= CREATE_UNICODE_ENVIRONMENT;

    bool success = CreateProcessW(nullptr,
                                  const_cast<LPWSTR>(args.c_str()),
                                  nullptr,
                                  nullptr,
                                  true,
                                  dwCreationFlags,
                                  lpEnvironment.get(),
                                  nullptr,
                                  &si,
                                  &pi) != 0;
    if (!success) {
        const auto ewd = errnoWithDescription();
        ss << "couldn't start process " << _argv[0] << "; " << ewd;
        uasserted(14042, ss.str());
    }

    CloseHandle(pi.hThread);
    invariant(SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0));

    _pid = ProcessId::fromNative(pi.dwProcessId);
    registry.insertHandleForPid(_pid, pi.hProcess);
#else

    std::string execErrMsg = str::stream() << "Unable to start program " << _argv[0];
    auto constCharStorageMaker = [](const std::vector<std::string>& in) {
        std::vector<const char*> out;
        std::transform(in.begin(), in.end(), std::back_inserter(out), [](const std::string& x) {
            return x.c_str();
        });
        out.push_back(nullptr);
        return out;
    };

    std::vector<const char*> argvStorage = constCharStorageMaker(_argv);
    std::vector<const char*> envpStorage = constCharStorageMaker(envStrings);

    pid_t nativePid = fork();
    _pid = ProcessId::fromNative(nativePid);
    // Async signal unsafe functions should not be called in the child process.

    if (nativePid == -1) {
        // Fork failed so it is time for the process to exit
        const auto ewd = errnoWithDescription();
        cout << "ProgramRunner is unable to fork child process: " << ewd << endl;
        fassertFailed(34363);
    }

    if (nativePid == 0) {
        // DON'T ASSERT IN THIS BLOCK - very bad things will happen
        //
        // Also, deliberately call _exit instead of quickExit. We intended to
        // fork() and exec() here, so we never want to run any form of cleanup.
        // This includes things that quickExit calls, such as atexit leak
        // checks.

        if (dup2(child_stdout, STDOUT_FILENO) == -1 || dup2(child_stdout, STDERR_FILENO) == -1) {
            // Async signal unsafe code reporting a terminal error condition.
            perror("Unable to dup2 child output: ");
            _exit(-1);  // do not pass go, do not call atexit handlers
        }

        execve(argvStorage[0],
               const_cast<char**>(argvStorage.data()),
               const_cast<char**>(envpStorage.data()));

        // Async signal unsafe code reporting a terminal error condition.
        perror(execErrMsg.c_str());

        _exit(-1);
    }

#endif
}

// returns true if process exited
// If this function returns true, it will always call `registry.unregisterProgram(pid);`
// If block is true, this will throw if it cannot wait for the processes to exit.
bool wait_for_pid(ProcessId pid, bool block = true, int* exit_code = NULL) {
#ifdef _WIN32
    verify(registry.countHandleForPid(pid));
    HANDLE h = registry.getHandleForPid(pid);

    // wait until the process object is signaled before getting its
    // exit code. do this even when block is false to ensure that all
    // file handles open in the process have been closed.

    DWORD ret = WaitForSingleObject(h, (block ? INFINITE : 0));
    if (ret == WAIT_TIMEOUT) {
        return false;
    } else if (ret != WAIT_OBJECT_0) {
        const auto ewd = errnoWithDescription();
        log() << "wait_for_pid: WaitForSingleObject failed: " << ewd;
    }

    DWORD tmp;
    if (GetExitCodeProcess(h, &tmp)) {
        if (tmp == STILL_ACTIVE) {
            uassert(
                ErrorCodes::UnknownError, "Process is STILL_ACTIVE even after blocking", !block);
            return false;
        }
        CloseHandle(h);
        registry.eraseHandleForPid(pid);
        if (exit_code)
            *exit_code = tmp;

        registry.unregisterProgram(pid);
        return true;
    } else {
        const auto ewd = errnoWithDescription();
        log() << "GetExitCodeProcess failed: " << ewd;
        return false;
    }
#else
    int tmp;
    int ret;
    do {
        ret = waitpid(pid.toNative(), &tmp, (block ? 0 : WNOHANG));
    } while (ret == -1 && errno == EINTR);
    if (ret && exit_code) {
        if (WIFEXITED(tmp)) {
            *exit_code = WEXITSTATUS(tmp);
        } else if (WIFSIGNALED(tmp)) {
            *exit_code = -WTERMSIG(tmp);
        } else {
            MONGO_UNREACHABLE;
        }
    }
    if (ret) {
        registry.unregisterProgram(pid);
    } else if (block) {
        uasserted(ErrorCodes::UnknownError, "Process did not exit after blocking");
    }
    return ret == pid.toNative();
#endif
}

BSONObj RawMongoProgramOutput(const BSONObj& args, void* data) {
    return BSON("" << programOutputLogger.str());
}

BSONObj ClearRawMongoProgramOutput(const BSONObj& args, void* data) {
    programOutputLogger.clear();
    return undefinedReturn;
}

BSONObj CheckProgram(const BSONObj& args, void* data) {
    ProcessId pid = ProcessId::fromNative(singleArg(args).numberInt());
    bool isDead = wait_for_pid(pid, false);
    return BSON(string("") << (!isDead));
}

BSONObj WaitProgram(const BSONObj& a, void* data) {
    ProcessId pid = ProcessId::fromNative(singleArg(a).numberInt());
    int exit_code = -123456;  // sentinel value
    wait_for_pid(pid, true, &exit_code);
    return BSON(string("") << exit_code);
}

// This function starts a program. In its input array it accepts either all commandline tokens
// which will be executed, or a single Object which must have a field named "args" which contains
// an array with all commandline tokens. The Object may have a field named "env" which contains an
// object of Key Value pairs which will be loaded into the environment of the spawned process.
BSONObj StartMongoProgram(const BSONObj& a, void* data) {
    _nokillop = true;
    BSONObj args = a;
    BSONObj env{};
    BSONElement firstElement = args.firstElement();

    if (firstElement.ok() && firstElement.isABSONObj()) {
        BSONObj subobj = firstElement.Obj();
        BSONElement argsElem = subobj["args"];
        BSONElement envElem = subobj["env"];
        uassert(40098,
                "If StartMongoProgram is called with a BSONObj, "
                "it must contain an 'args' subobject." +
                    args.toString(),
                argsElem.ok() && argsElem.isABSONObj());

        args = argsElem.Obj();
        if (envElem.ok() && envElem.isABSONObj()) {
            env = envElem.Obj();
        }
    }

    ProgramRunner r(args, env);
    r.start();
    invariant(registry.isPidRegistered(r.pid()));
    stdx::thread t(r);
    registry.registerReaderThread(r.pid(), std::move(t));
    return BSON(string("") << r.pid().asLongLong());
}

BSONObj RunMongoProgram(const BSONObj& a, void* data) {
    BSONObj env{};
    ProgramRunner r(a, env);
    r.start();
    invariant(registry.isPidRegistered(r.pid()));
    stdx::thread t(r);
    registry.registerReaderThread(r.pid(), std::move(t));
    int exit_code = -123456;  // sentinel value
    wait_for_pid(r.pid(), true, &exit_code);
    return BSON(string("") << exit_code);
}

BSONObj ResetDbpath(const BSONObj& a, void* data) {
    verify(a.nFields() == 1);
    string path = a.firstElement().valuestrsafe();
    verify(!path.empty());
    if (boost::filesystem::exists(path))
        boost::filesystem::remove_all(path);
    boost::filesystem::create_directory(path);
    return undefinedReturn;
}

BSONObj PathExists(const BSONObj& a, void* data) {
    verify(a.nFields() == 1);
    string path = a.firstElement().valuestrsafe();
    verify(!path.empty());
    bool exists = boost::filesystem::exists(path);
    return BSON(string("") << exists);
}

void copyDir(const boost::filesystem::path& from, const boost::filesystem::path& to) {
    boost::filesystem::directory_iterator end;
    boost::filesystem::directory_iterator i(from);
    while (i != end) {
        boost::filesystem::path p = *i;
        if (p.leaf() == "metrics.interim" || p.leaf() == "metrics.interim.temp") {
            // Ignore any errors for metrics.interim* files as these may disappear during copy
            boost::system::error_code ec;
            boost::filesystem::copy_file(p, to / p.leaf(), ec);
            if (ec) {
                log() << "Skipping copying of file from '" << p.generic_string() << "' to '"
                      << (to / p.leaf()).generic_string() << "' due to: " << ec.message();
            }
        } else if (p.leaf() != "mongod.lock" && p.leaf() != "WiredTiger.lock") {
            if (boost::filesystem::is_directory(p)) {
                boost::filesystem::path newDir = to / p.leaf();
                boost::filesystem::create_directory(newDir);
                copyDir(p, newDir);
            } else {
                boost::filesystem::copy_file(p, to / p.leaf());
            }
        }
        ++i;
    }
}

// NOTE target dbpath will be cleared first
BSONObj CopyDbpath(const BSONObj& a, void* data) {
    verify(a.nFields() == 2);
    BSONObjIterator i(a);
    string from = i.next().str();
    string to = i.next().str();
    verify(!from.empty());
    verify(!to.empty());
    if (boost::filesystem::exists(to))
        boost::filesystem::remove_all(to);
    boost::filesystem::create_directory(to);
    copyDir(from, to);
    return undefinedReturn;
}

inline void kill_wrapper(ProcessId pid, int sig, int port, const BSONObj& opt) {
#ifdef _WIN32
    if (sig == SIGKILL || port == 0) {
        verify(registry.countHandleForPid(pid));
        TerminateProcess(registry.getHandleForPid(pid),
                         1);  // returns failure for "zombie" processes.
        return;
    }

    std::string eventName = getShutdownSignalName(pid.asUInt32());

    HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (event == NULL) {
        int gle = GetLastError();
        if (gle != ERROR_FILE_NOT_FOUND) {
            const auto ewd = errnoWithDescription();
            warning() << "kill_wrapper OpenEvent failed: " << ewd;
        } else {
            log() << "kill_wrapper OpenEvent failed to open event to the process " << pid.asUInt32()
                  << ". It has likely died already or server is running an older version."
                  << " Attempting to shutdown through admin command.";

            // Back-off to the old way of shutting down the server on Windows, in case we
            // are managing a pre-2.6.0rc0 service, which did not have the event.
            //
            try {
                DBClientConnection conn;
                conn.connect(HostAndPort{"127.0.0.1:" + BSONObjBuilder::numStr(port)},
                             "MongoDB Shell");

                BSONElement authObj = opt["auth"];

                if (!authObj.eoo()) {
                    string errMsg;
                    conn.auth("admin", authObj["user"].String(), authObj["pwd"].String(), errMsg);

                    if (!errMsg.empty()) {
                        cout << "Failed to authenticate before shutdown: " << errMsg << endl;
                    }
                }

                BSONObj info;
                BSONObjBuilder b;
                b.append("shutdown", 1);
                b.append("force", 1);
                conn.runCommand("admin", b.done(), info);
            } catch (...) {
                // Do nothing. This command never returns data to the client and the driver
                // doesn't like that.
                //
            }
        }
        return;
    }

    ON_BLOCK_EXIT(CloseHandle, event);

    bool result = SetEvent(event);
    if (!result) {
        const auto ewd = errnoWithDescription();
        error() << "kill_wrapper SetEvent failed: " << ewd;
        return;
    }
#else
    int x = kill(pid.toNative(), sig);
    if (x) {
        if (errno == ESRCH) {
        } else {
            const auto ewd = errnoWithDescription();
            log() << "killFailed: " << ewd;
            verify(x == 0);
        }
    }

#endif
}

int killDb(int port, ProcessId _pid, int signal, const BSONObj& opt) {
    ProcessId pid;
    if (port > 0) {
        if (!registry.isPortRegistered(port)) {
            log() << "No db started on port: " << port;
            return 0;
        }
        pid = registry.pidForPort(port);
    } else {
        pid = _pid;
    }

    kill_wrapper(pid, signal, port, opt);

    int exitCode = EXIT_FAILURE;
    try {
        wait_for_pid(pid, true, &exitCode);
    } catch (...) {
        warning() << "process " << pid << " failed to terminate.";
        return EXIT_FAILURE;
    }

    if (signal == SIGKILL) {
        sleepmillis(4000);  // allow operating system to reclaim resources
    }

    return exitCode;
}

int killDb(int port, ProcessId _pid, int signal) {
    BSONObj dummyOpt;
    return killDb(port, _pid, signal, dummyOpt);
}

int getSignal(const BSONObj& a) {
    int ret = SIGTERM;
    if (a.nFields() >= 2) {
        BSONObjIterator i(a);
        i.next();
        BSONElement e = i.next();
        verify(e.isNumber());
        ret = int(e.number());
    }
    return ret;
}

BSONObj getStopMongodOpts(const BSONObj& a) {
    if (a.nFields() == 3) {
        BSONObjIterator i(a);
        i.next();
        i.next();
        BSONElement e = i.next();

        if (e.isABSONObj()) {
            return e.embeddedObject();
        }
    }

    return BSONObj();
}

/** stopMongoProgram(port[, signal]) */
BSONObj StopMongoProgram(const BSONObj& a, void* data) {
    int nFields = a.nFields();
    verify(nFields >= 1 && nFields <= 3);
    uassert(15853, "stopMongo needs a number", a.firstElement().isNumber());
    int port = int(a.firstElement().number());
    int code = killDb(port, ProcessId::fromNative(0), getSignal(a), getStopMongodOpts(a));
    log() << "shell: stopped mongo program on port " << port;
    return BSON("" << (double)code);
}

BSONObj StopMongoProgramByPid(const BSONObj& a, void* data) {
    verify(a.nFields() == 1 || a.nFields() == 2);
    uassert(15852, "stopMongoByPid needs a number", a.firstElement().isNumber());
    ProcessId pid = ProcessId::fromNative(int(a.firstElement().number()));
    int code = killDb(0, pid, getSignal(a));
    log() << "shell: stopped mongo program on pid " << pid;
    return BSON("" << (double)code);
}

int KillMongoProgramInstances() {
    vector<ProcessId> pids;
    registry.getRegisteredPids(pids);
    int returnCode = EXIT_SUCCESS;
    for (auto&& pid : pids) {
        int port = registry.portForPid(pid);
        int code = killDb(port != -1 ? port : 0, pid, SIGTERM);
        if (code != EXIT_SUCCESS) {
            returnCode = code;
        }
    }
    return returnCode;
}

MongoProgramScope::~MongoProgramScope() {
    DESTRUCTOR_GUARD(KillMongoProgramInstances(); ClearRawMongoProgramOutput(BSONObj(), 0);)
}

void installShellUtilsLauncher(Scope& scope) {
    scope.injectNative("_startMongoProgram", StartMongoProgram);
    scope.injectNative("runProgram", RunMongoProgram);
    scope.injectNative("run", RunMongoProgram);
    scope.injectNative("_runMongoProgram", RunMongoProgram);
    scope.injectNative("_stopMongoProgram", StopMongoProgram);
    scope.injectNative("stopMongoProgramByPid", StopMongoProgramByPid);
    scope.injectNative("rawMongoProgramOutput", RawMongoProgramOutput);
    scope.injectNative("clearRawMongoProgramOutput", ClearRawMongoProgramOutput);
    scope.injectNative("waitProgram", WaitProgram);
    scope.injectNative("checkProgram", CheckProgram);
    scope.injectNative("resetDbpath", ResetDbpath);
    scope.injectNative("pathExists", PathExists);
    scope.injectNative("copyDbpath", CopyDbpath);
}
}
}
