#include "debug/common_logging.hpp"
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

int main(int argc,char** argv)
{
    if(initializeLoggingModule(LOGGING_LEVEL_VERBOSE)!=STATUS_OK) return 2;
    setConsoleLog(MAIN_LOG_ID,LOGGING_LEVEL_NONE);
    const std::string tag=argc>1 ? argv[1] : "default";
    std::vector<std::thread> writers;
    for(int worker=0;worker<4;++worker) writers.emplace_back([worker,&tag]() {
        for(int i=0;i<2500;++i) LOG_INFO(MAIN_LOG_ID,"{} worker={} entry={}",tag,worker,i);
    });
    for(auto& writer:writers) writer.join();
    LOG_VERBOSE(FRAME_LOG_ID,"{} LAST_VERBOSE",tag);
    LOG_INFO(GAME_MANAGER_LOG_ID,"{} LAST_INFO",tag);
    LOG_CRITICAL(MAIN_LOG_ID,"{} LAST_CRITICAL",tag);
    if(tag=="lifecycle") {
        shutdownLoggingModule();
        if(getLogger(MAIN_LOG_ID)) return 3;
        if(initializeLoggingModule(LOGGING_LEVEL_INFO)!=STATUS_OK) return 4;
        LOG_INFO(MAIN_LOG_ID,"REINITIALIZED");
        shutdownLoggingModule();
        return 0;
    }
    // No destructors, atexit handlers, logging shutdown or CRT stream flushing.
    std::_Exit(73);
}
