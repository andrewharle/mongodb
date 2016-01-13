// processinfo_win32.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/pch.h"

#include <iostream>
#include <psapi.h>

#include "mongo/util/processinfo.h"

using namespace std;

namespace mongo {

    // dynamically link to psapi.dll (in case this version of Windows
    // does not support what we need)
    struct PsApiInit {
        bool supported;
        typedef BOOL (WINAPI *pQueryWorkingSetEx)(HANDLE hProcess,
                                                  PVOID pv,
                                                  DWORD cb);
        pQueryWorkingSetEx QueryWSEx;

        PsApiInit() {
            HINSTANCE psapiLib = LoadLibrary( TEXT("psapi.dll") );
            if (psapiLib) {
                QueryWSEx = reinterpret_cast<pQueryWorkingSetEx>
                    ( GetProcAddress( psapiLib, "QueryWorkingSetEx" ) );
                if (QueryWSEx) {
                    supported = true;
                    return;
                }
            }
            supported = false;
        }
    };

    static PsApiInit* psapiGlobal = NULL;

    int _wconvertmtos( SIZE_T s ) {
        return (int)( s / ( 1024 * 1024 ) );
    }

    ProcessInfo::ProcessInfo( ProcessId pid ) {
    }

    ProcessInfo::~ProcessInfo() {
    }

    bool ProcessInfo::supported() {
        return true;
    }

    int ProcessInfo::getVirtualMemorySize() {
        MEMORYSTATUSEX mse;
        mse.dwLength = sizeof(mse);
        verify( GlobalMemoryStatusEx( &mse ) );
        DWORDLONG x = (mse.ullTotalVirtual - mse.ullAvailVirtual) / (1024 * 1024) ;
        verify( x <= 0x7fffffff );
        return (int) x;
    }

    int ProcessInfo::getResidentSize() {
        PROCESS_MEMORY_COUNTERS pmc;
        verify( GetProcessMemoryInfo( GetCurrentProcess() , &pmc, sizeof(pmc) ) );
        return _wconvertmtos( pmc.WorkingSetSize );
    }

    void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
        MEMORYSTATUSEX mse;
        mse.dwLength = sizeof(mse);
        PROCESS_MEMORY_COUNTERS pmc;
        if( GetProcessMemoryInfo( GetCurrentProcess() , &pmc, sizeof(pmc) ) ) {
            info.append("page_faults", static_cast<int>(pmc.PageFaultCount));
            info.append("usagePageFileMB", static_cast<int>(pmc.PagefileUsage / 1024 / 1024));
        }
        if( GlobalMemoryStatusEx( &mse ) ) {
            info.append("totalPageFileMB", static_cast<int>(mse.ullTotalPageFile / 1024 / 1024));
            info.append("availPageFileMB", static_cast<int>(mse.ullAvailPageFile / 1024 / 1024));
            info.append("ramMB", static_cast<int>(mse.ullTotalPhys / 1024 / 1024));
        }
    }

    bool getFileVersion(const char *filePath, DWORD &fileVersionMS, DWORD &fileVersionLS) {
        DWORD verSize = GetFileVersionInfoSizeA(filePath, NULL);
        if (verSize == 0) {
            DWORD gle = GetLastError();
            warning() << "GetFileVersionInfoSizeA on " << filePath << " failed with " << errnoWithDescription(gle);
            return false;
        }

        boost::scoped_array<char> verData(new char[verSize]);
        if (GetFileVersionInfoA(filePath, NULL, verSize, verData.get()) == 0) {
            DWORD gle = GetLastError();
            warning() << "GetFileVersionInfoSizeA on " << filePath << " failed with " << errnoWithDescription(gle);
            return false;
        }

        UINT size;
        VS_FIXEDFILEINFO *verInfo;
        if (VerQueryValueA(verData.get(), "\\", (LPVOID *)&verInfo, &size) == 0) {
            DWORD gle = GetLastError();
            warning() << "VerQueryValueA on " << filePath << " failed with " << errnoWithDescription(gle);
            return false;
        }

        if (size != sizeof(VS_FIXEDFILEINFO)) {
            warning() << "VerQueryValueA on " << filePath << " returned structure with unexpected size";
            return false;
        }

        fileVersionMS = verInfo->dwFileVersionMS;
        fileVersionLS = verInfo->dwFileVersionLS;
        return true;
    }

    // If the version of the ntfs.sys driver shows that the KB2731284 hotfix or a later update
    // is installed, zeroing out data files is unnecessary. The file version numbers used below
    // are taken from the Hotfix File Information at http://support.microsoft.com/kb/2731284.
    bool isKB2731284OrLaterUpdateInstalled() {
        UINT pathBufferSize = GetSystemDirectoryA(NULL, 0);
        if (pathBufferSize == 0) {
            DWORD gle = GetLastError();
            warning() << "GetSystemDirectoryA failed with " << errnoWithDescription(gle);
            return false;
        }

        boost::scoped_array<char> systemDirectory(new char[pathBufferSize]);
        UINT systemDirectoryPathLen;
        systemDirectoryPathLen = GetSystemDirectoryA(systemDirectory.get(), pathBufferSize);
        if (systemDirectoryPathLen == 0) {
            DWORD gle = GetLastError();
            warning() << "GetSystemDirectoryA failed with " << errnoWithDescription(gle);
            return false;
        }

        if (systemDirectoryPathLen != pathBufferSize - 1) {
            warning() << "GetSystemDirectoryA returned unexpected path length";
            return false;
        }

        string ntfsDotSysPath = systemDirectory.get();
        if (ntfsDotSysPath.back() != '\\') {
            ntfsDotSysPath.append("\\");
        }
        ntfsDotSysPath.append("drivers\\ntfs.sys");
        DWORD fileVersionMS;
        DWORD fileVersionLS;
        if (getFileVersion(ntfsDotSysPath.c_str(), fileVersionMS, fileVersionLS)) {
            WORD fileVersionFirstNumber = HIWORD(fileVersionMS);
            WORD fileVersionSecondNumber = LOWORD(fileVersionMS);
            WORD fileVersionThirdNumber = HIWORD(fileVersionLS);
            WORD fileVersionFourthNumber = LOWORD(fileVersionLS);

            if (fileVersionFirstNumber == 6 && fileVersionSecondNumber == 1 && fileVersionThirdNumber == 7600 &&
                    fileVersionFourthNumber >= 21296 && fileVersionFourthNumber <= 21999) {
                return true;
            } else if (fileVersionFirstNumber == 6 && fileVersionSecondNumber == 1 && fileVersionThirdNumber == 7601 &&
                    fileVersionFourthNumber >= 22083 && fileVersionFourthNumber <= 22999) {
                return true;
            }
        }

        return false;
    }

    void ProcessInfo::SystemInfo::collectSystemInfo() {
        BSONObjBuilder bExtra;
        stringstream verstr;
        OSVERSIONINFOEX osvi;   // os version
        MEMORYSTATUSEX mse;     // memory stats
        SYSTEM_INFO ntsysinfo;  //system stats

        // get basic processor properties
        GetNativeSystemInfo( &ntsysinfo );
        addrSize = (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? 64 : 32);
        numCores = ntsysinfo.dwNumberOfProcessors;
        pageSize = static_cast<unsigned long long>(ntsysinfo.dwPageSize);
        bExtra.append("pageSize", static_cast<long long>(pageSize));

        // get memory info
        mse.dwLength = sizeof( mse );
        if ( GlobalMemoryStatusEx( &mse ) ) {
            memSize = mse.ullTotalPhys;
        }

        // get OS version info
        ZeroMemory( &osvi, sizeof( osvi ) );
        osvi.dwOSVersionInfoSize = sizeof( osvi );
        if ( GetVersionEx( (OSVERSIONINFO*)&osvi ) ) {

            verstr << osvi.dwMajorVersion << "." << osvi.dwMinorVersion;
            if ( osvi.wServicePackMajor )
                verstr << " SP" << osvi.wServicePackMajor;
            verstr << " (build " << osvi.dwBuildNumber << ")";

            osName = "Microsoft ";
            switch ( osvi.dwMajorVersion ) {
            case 6:
                switch ( osvi.dwMinorVersion ) {
                    case 3:
                        if ( osvi.wProductType == VER_NT_WORKSTATION )
                            osName += "Windows 8.1";
                        else
                            osName += "Windows Server 2012 R2";
                        break;
                    case 2:
                        if ( osvi.wProductType == VER_NT_WORKSTATION )
                            osName += "Windows 8";
                        else
                            osName += "Windows Server 2012";
                        break;
                    case 1:
                        if ( osvi.wProductType == VER_NT_WORKSTATION )
                            osName += "Windows 7";
                        else
                            osName += "Windows Server 2008 R2";

                        // Windows 6.1 is either Windows 7 or Windows 2008 R2. There is no SP2 for
                        // either of these two operating systems, but the check will hold if one
                        // were released. This code assumes that SP2 will include fix for
                        // http://support.microsoft.com/kb/2731284.
                        //
                        if ((osvi.wServicePackMajor >= 0) && (osvi.wServicePackMajor < 2)) {
                              if (isKB2731284OrLaterUpdateInstalled()) {
                                  log() << "Hotfix KB2731284 or later update is installed, no need to zero-out data files";
                                  fileZeroNeeded = false;
                              } else {
                                  log() << "Hotfix KB2731284 or later update is not installed, will zero-out data files";
                                  fileZeroNeeded = true;
                              }
                        }
                        break;
                    case 0:
                        if ( osvi.wProductType == VER_NT_WORKSTATION )
                            osName += "Windows Vista";
                        else
                            osName += "Windows Server 2008";
                        break;
                    default:
                        osName += "Windows NT version ";
                        osName += verstr.str();
                        break;
                }
                break;
            case 5:
                switch ( osvi.dwMinorVersion ) {
                    case 2:
                        osName += "Windows Server 2003";
                        break;
                    case 1:
                        osName += "Windows XP";
                        break;
                    case 0:
                        if ( osvi.wProductType == VER_NT_WORKSTATION )
                            osName += "Windows 2000 Professional";
                        else
                            osName += "Windows 2000 Server";
                        break;
                    default:
                        osName += "Windows NT version ";
                        osName += verstr.str();
                        break;
                }
                break;
            }
        }
        else {
            // unable to get any version data
            osName += "Windows NT";
        }

        if ( ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ) { cpuArch = "x86_64"; }
        else if ( ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ) { cpuArch = "x86"; }
        else if ( ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64 ) { cpuArch = "ia64"; }
        else { cpuArch = "unknown"; }

        osType = "Windows";
        osVersion = verstr.str();
        hasNuma = checkNumaEnabled();
        _extraStats = bExtra.obj();
        if (psapiGlobal == NULL) {
            psapiGlobal = new PsApiInit();
        }

    }

    bool ProcessInfo::checkNumaEnabled() {
        typedef BOOL(WINAPI *LPFN_GLPI)(
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION,
            PDWORD);

        DWORD returnLength = 0;
        DWORD numaNodeCount = 0;
        scoped_array<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer;

        LPFN_GLPI glpi(reinterpret_cast<LPFN_GLPI>(GetProcAddress(
            GetModuleHandleW(L"kernel32"),
            "GetLogicalProcessorInformation")));
        if (glpi == NULL) {
            return false;
        }

        DWORD returnCode = 0;
        do {
            returnCode = glpi(buffer.get(), &returnLength);

            if (returnCode == FALSE) {
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                    buffer.reset(reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(
                        new BYTE[returnLength]));
                }
                else {
                    DWORD gle = GetLastError();
                    warning() << "GetLogicalProcessorInformation failed with "
                        << errnoWithDescription(gle);
                    return false;
                }
            }
        } while (returnCode == FALSE);

        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = buffer.get();

        unsigned int byteOffset = 0;
        while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength) {
            if (ptr->Relationship == RelationNumaNode) {
                // Non-NUMA systems report a single record of this type.
                numaNodeCount++;
            }

            byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            ptr++;
        }

        // For non-NUMA machines, the count is 1
        return numaNodeCount > 1;
    }

    bool ProcessInfo::blockCheckSupported() {
        return psapiGlobal->supported;
    }

    bool ProcessInfo::blockInMemory(const void* start) {
#if 0
        // code for printing out page fault addresses and pc's --
        // this could be useful for targetting heavy pagefault locations in the code
        static BOOL bstat = InitializeProcessForWsWatch( GetCurrentProcess() );
        PSAPI_WS_WATCH_INFORMATION_EX wiex[30];
        DWORD bufsize =  sizeof(wiex);
        bstat = GetWsChangesEx( GetCurrentProcess(), &wiex[0], &bufsize );
        if (bstat) {
            for (int i=0; i<30; i++) {
                if (wiex[i].BasicInfo.FaultingPc == 0) break;
                cout << "faulting pc = " << wiex[i].BasicInfo.FaultingPc << " address = " << wiex[i].BasicInfo.FaultingVa << " thread id = " << wiex[i].FaultingThreadId << endl;
            }
        }
#endif
        PSAPI_WORKING_SET_EX_INFORMATION wsinfo;
        wsinfo.VirtualAddress = const_cast<void*>(start);
        BOOL result = psapiGlobal->QueryWSEx( GetCurrentProcess(), &wsinfo, sizeof(wsinfo) );
        if ( result )
            if ( wsinfo.VirtualAttributes.Valid )
                return true;
        return false;
    }

    bool ProcessInfo::pagesInMemory(const void* start, size_t numPages, vector<char>* out) {
        out->resize(numPages);
        scoped_array<PSAPI_WORKING_SET_EX_INFORMATION> wsinfo(
                new PSAPI_WORKING_SET_EX_INFORMATION[numPages]);

        const void* startOfFirstPage = alignToStartOfPage(start);
        for (size_t i = 0; i < numPages; i++) {
            wsinfo[i].VirtualAddress = reinterpret_cast<void*>(
                    reinterpret_cast<unsigned long long>(startOfFirstPage) + i * getPageSize());
        }

        BOOL result = psapiGlobal->QueryWSEx(GetCurrentProcess(),
                                            wsinfo.get(),
                                            sizeof(PSAPI_WORKING_SET_EX_INFORMATION) * numPages);

        if (!result) return false;
        for (size_t i = 0; i < numPages; ++i) {
            (*out)[i] = wsinfo[i].VirtualAttributes.Valid ? 1 : 0;
        }
        return true;
    }

}
