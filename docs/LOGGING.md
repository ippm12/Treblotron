# Logging and crash diagnostics

Logging is synchronous: accepted messages are written and flushed before the
logging call returns. There is no asynchronous queue to overflow or lose on
process exit. This costs file I/O on logging threads, so avoid high-frequency
verbose logging in production.

Each channel writes to `logs/treblotron_<id>_<name>.log` under the application
data directory (beside the executable in portable builds). Files rotate on
restart and at 5 MiB, retaining three backups (`.1.log` is newest). Copy logs
soon after a failure; retention is bounded.

Flushing protects completed log calls against abrupt process termination.
It does not guarantee a message whose call was interrupted, preserve data
through power/storage failure, or itself produce a crash stack trace. Windows crash capture is described below.
No logging mutexes are taken from crash/signal handlers.

Stop logging producer threads before `shutdownLoggingModule()`. Shutdown
flushes and releases all loggers, and initialization can then run again.

Run `python tests/run.py debug/logging --build-dir build` with a configured
GCC/Ninja application build. It uses isolated logs under
`build/tests/debug/logging`, verifies 10,000 concurrent messages and final
verbose/info/critical records after immediate process exit without cleanup,
then checks restart retention and shutdown/reinitialization.


## Windows crash dumps

The app and server start a hidden helper process before logging or application
initialization. Unhandled Windows exceptions, C++ termination, aborts, and
Box2D assertions signal this helper. The failing thread remains alive while
the helper writes the dump, with a 20-second upper wait bound. The crash
handler does not call the logger or allocate heap memory.

Files are stored in `crashes/` under the same application data directory as
logs. Portable builds use `build/bin/crashes/`. Each run gets a unique UTC
timestamp and PID filename; restarting does not truncate existing dumps.

- `.dmp`: thread stacks, modules, exception context, and referenced memory.
- `.dmp.txt`: exception code/address, thread/process, executable and build
  identification, dump success/failure, and any assertion reason.
- `.dmp.exe`: matching executable preserved on successful capture. MinGW
  debug executables include their debugging symbols; these copies can be large.

Capture failures are recorded in the sidecar report when possible. A helper
startup failure is reported as a warning in the ordinary logs. No files are
uploaded. A clean exit creates no dump. Forced process kills, power loss,
fail-fast mechanisms that bypass handlers, and failures before main are not
covered by these application handlers. An attached debugger may intercept an
exception before the unhandled-exception handler runs.

Run `python tests/run.py debug/crash_reporting --build-dir build` to exercise
Windows exceptions, an uncaught C++ exception, abort, and a background-thread
exception. The test inspects the minidump signature and exception stream and
uses `build/tests/debug/crash_reporting`, leaving real game crash records untouched.
