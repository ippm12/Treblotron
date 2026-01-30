/**
 * startup.cpp
 * 
 * The main function that runbs on startup
 */

#include "common_inc.hpp"

int main()
{
    Status stat = initializeLoggingModule(LOGGING_LEVEL_INFO);
    if(IS_STATUS_NOT_OK(stat))
    {
        return -1;
    }

    LOG_INFO(MAIN_LOG_ID, "Finished Initializing");

    shutdownLoggingModule();
    return 0;
}