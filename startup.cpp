/**
 * startup.cpp
 * 
 * The main function that runbs on startup
 */

#include <thread>
#include <chrono>
#include "common_inc.hpp"
#include "frame/frame.hpp"

int main()
{
    Status stat = initializeLoggingModule(LOGGING_LEVEL_INFO);
    if(IS_STATUS_NOT_OK(stat))
    {
        return -1;
    }

    stat = initializeFrameModule();
    if(IS_STATUS_NOT_OK(stat))
    {
        LOG_CRITICAL(MAIN_LOG_ID, "Failed Frame Module Init");
        return -1;
    }

    LOG_INFO(MAIN_LOG_ID, "Finished Initializing");

    while(pollFrames())
    {
        // Game logic here
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    shutdownFrameModule();
    shutdownLoggingModule();
    return 0;
}