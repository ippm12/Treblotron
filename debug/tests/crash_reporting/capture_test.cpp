#include "debug/crash_reporting.hpp"
#include <windows.h>
#include "game_lib/box2d/physics_world.hpp"
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <thread>
int main(int argc,char** argv)
{
    int helperExit=0;
    if(runCrashDumpHelper(argc,argv,helperExit)) return helperExit;
    if(!initializeCrashReporting()) return 2;
    if(argc<2 || std::strcmp(argv[1],"normal")==0) return 0;
    if(std::strcmp(argv[1],"box2d")==0) {
        PhysicsWorld world;
        world.step(0.01f,0); // Box2D requires at least one sub-step.
        return 3;
    }
    if(std::strcmp(argv[1],"cpp")==0) throw std::runtime_error("crash capture regression");
    if(std::strcmp(argv[1],"abort")==0) std::abort();
    if(std::strcmp(argv[1],"thread")==0) {
        std::thread worker([](){ RaiseException(EXCEPTION_ACCESS_VIOLATION,EXCEPTION_NONCONTINUABLE,0,nullptr); });
        worker.join();
        return 3;
    }
    RaiseException(EXCEPTION_ACCESS_VIOLATION,EXCEPTION_NONCONTINUABLE,0,nullptr);
    return 3;
}
