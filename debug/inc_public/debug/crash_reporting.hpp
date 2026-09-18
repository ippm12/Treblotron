#pragma once
// Called before logging or application initialization. Returns true in the
// helper process, which should exit immediately using helperExitCode.
bool runCrashDumpHelper(int argc,char** argv,int& helperExitCode);
bool initializeCrashReporting();
[[noreturn]] void reportFatalCrash(const char* reason);
