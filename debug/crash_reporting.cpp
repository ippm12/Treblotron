#include "debug/crash_reporting.hpp"
#include "debug/app_paths.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <csignal>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <fstream>
#include <filesystem>
#include <string>
#include "treblotron_version.hpp"

namespace {
struct CrashRequest {
    DWORD threadId;
    DWORD code;
    uintptr_t address;
    uintptr_t exceptionPointers;
    wchar_t path[2048];
    wchar_t executable[2048];
    char reason[512];
};
CrashRequest* request=nullptr;
HANDLE requested=nullptr, completed=nullptr, helperProcess=nullptr;
volatile LONG reporting=0;

LONG WINAPI handleException(EXCEPTION_POINTERS* exception)
{
    if(!request || InterlockedCompareExchange(&reporting,1,0)!=0) return EXCEPTION_CONTINUE_SEARCH;
    request->threadId=GetCurrentThreadId();
    request->exceptionPointers=reinterpret_cast<uintptr_t>(exception);
    request->code=exception->ExceptionRecord->ExceptionCode;
    request->address=reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    SetEvent(requested);
    HANDLE waits[]={completed,helperProcess};
    WaitForMultipleObjects(2,waits,FALSE,20000);
    return EXCEPTION_EXECUTE_HANDLER;
}
HANDLE parseHandle(const char* text)
{
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::strtoull(text,nullptr,10)));
}
}
bool runCrashDumpHelper(int argc,char** argv,int& exitCode)
{
    if(argc!=6 || std::strcmp(argv[1],"--internal-crash-helper")!=0) return false;
    exitCode=1;
    const HANDLE mapping=parseHandle(argv[2]), event=parseHandle(argv[3]);
    const HANDLE done=parseHandle(argv[4]), parent=parseHandle(argv[5]);
    auto* data=static_cast<CrashRequest*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(CrashRequest)));
    if(!data) return true;
    HANDLE waits[]={event,parent};
    if(WaitForMultipleObjects(2,waits,FALSE,INFINITE)==WAIT_OBJECT_0) {
        const std::filesystem::path path(data->path);
        std::ofstream report(path.string()+".txt");
        report << "Treblotron " << TREBLOTRON_VERSION_STRING << std::endl
               << "Reporter build: " << __DATE__ << " " << __TIME__ << std::endl
               << "Executable: " << std::filesystem::path(data->executable).string() << std::endl
               << "PID: " << GetProcessId(parent) << " Thread: " << data->threadId << std::endl
               << "Exception: 0x" << std::hex << data->code << std::endl
               << "Address: 0x" << data->address << std::dec << std::endl
               << "Reason: " << data->reason << std::endl;
        report.flush();
        HANDLE file=CreateFileW(data->path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        BOOL saved=FALSE;
        DWORD error=GetLastError();
        if(file!=INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{};
            info.ThreadId=data->threadId;
            info.ExceptionPointers=reinterpret_cast<EXCEPTION_POINTERS*>(data->exceptionPointers);
            info.ClientPointers=TRUE;
            saved=MiniDumpWriteDump(parent,GetProcessId(parent),file,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules|
                                          MiniDumpWithIndirectlyReferencedMemory),
                &info,nullptr,nullptr);
            error=saved ? ERROR_SUCCESS : GetLastError();
            FlushFileBuffers(file);
            CloseHandle(file);
        }
        report << "Dump saved: " << (saved ? "yes":"no") << std::endl
               << "Win32 error: " << error << std::endl;
        // Preserve the matching executable before subsequent rebuilds replace it.
        // In MinGW debug builds this also preserves embedded debugging symbols.
        if(saved) {
            const std::wstring binary=std::wstring(data->path)+L".exe";
            const BOOL copied=CopyFileW(data->executable,binary.c_str(),TRUE);
            report << "Matching executable saved: " << (copied ? "yes":"no") << std::endl;
        }
        report.flush();
        SetEvent(done);
        exitCode=saved ? 0:1;
    } else exitCode=0; // A normal parent exit needs no dump.
    UnmapViewOfFile(data);
    CloseHandle(mapping); CloseHandle(event); CloseHandle(done); CloseHandle(parent);
    return true;
}
bool initializeCrashReporting()
{
    if(request) return true;
    // Everything the exception handler needs is allocated while healthy.
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,&attributes,PAGE_READWRITE,0,sizeof(CrashRequest),nullptr);
    if(!mapping) return false;
    auto* data=static_cast<CrashRequest*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(CrashRequest)));
    HANDLE event=CreateEventW(&attributes,FALSE,FALSE,nullptr);
    HANDLE done=CreateEventW(&attributes,FALSE,FALSE,nullptr);
    HANDLE parent=nullptr;
    DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&parent,
                    PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,TRUE,0);
    if(!data || !event || !done || !parent) {
        if(data) UnmapViewOfFile(data);
        CloseHandle(mapping);
        if(event) CloseHandle(event);
        if(done) CloseHandle(done);
        if(parent) CloseHandle(parent);
        return false;
    }
    SYSTEMTIME time; GetSystemTime(&time);
    wchar_t name[160];
    swprintf(name,160,L"crash-%04u%02u%02u-%02u%02u%02u-%03u-%lu.dmp",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,GetCurrentProcessId());
    const auto path=std::filesystem::path(appDataPath("crashes/"))/name;
    wcsncpy(data->path,path.c_str(),2047);
    GetModuleFileNameW(nullptr,data->executable,2048);
    std::wstring command=L"""+std::wstring(data->executable)+L"" --internal-crash-helper "+
        std::to_wstring(reinterpret_cast<uintptr_t>(mapping))+L" "+
        std::to_wstring(reinterpret_cast<uintptr_t>(event))+L" "+
        std::to_wstring(reinterpret_cast<uintptr_t>(done))+L" "+
        std::to_wstring(reinterpret_cast<uintptr_t>(parent));
    STARTUPINFOW startup{}; startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL launched=CreateProcessW(data->executable,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,
                                     nullptr,nullptr,&startup,&process);
    CloseHandle(parent);
    if(!launched) {
        UnmapViewOfFile(data); CloseHandle(mapping); CloseHandle(event); CloseHandle(done);
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(mapping); // Mapping stays live via the view and helper handle.
    request=data; requested=event; completed=done; helperProcess=process.hProcess;
    ULONG stackBytes=64*1024; SetThreadStackGuarantee(&stackBytes);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(handleException);
    std::set_terminate([](){reportFatalCrash("Uncaught C++ exception / std::terminate");});
    std::signal(SIGABRT,[](int){reportFatalCrash("abort / assertion failure");});
    return true;
}
[[noreturn]] void reportFatalCrash(const char* reason)
{
    if(request) {
        std::strncpy(request->reason,reason,sizeof(request->reason)-1);
        CONTEXT context{}; RtlCaptureContext(&context);
        EXCEPTION_RECORD record{}; record.ExceptionCode=0xE0000001;
#ifdef _WIN64
        record.ExceptionAddress=reinterpret_cast<void*>(context.Rip);
#else
        record.ExceptionAddress=reinterpret_cast<void*>(context.Eip);
#endif
        EXCEPTION_POINTERS pointers{&record,&context};
        handleException(&pointers);
    }
    TerminateProcess(GetCurrentProcess(),0xE0000001);
    std::_Exit(1);
}
#else
bool runCrashDumpHelper(int,char**,int&) { return false; }
bool initializeCrashReporting() { return false; }
[[noreturn]] void reportFatalCrash(const char*) { std::abort(); }
#endif
