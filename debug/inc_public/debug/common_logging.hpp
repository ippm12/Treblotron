/**
 * common_logging.hpp
 * 
 * A set of common logging definitions
 */

#ifndef COMMON_LOGGING_HPP
#define COMMON_LOGGING_HPP

#include <memory>
#include <string>
#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "common_types.hpp"

typedef spdlog::details::async_logger_ptr LoggerPtr;

// Log ID definitions
typedef ssize_t LogID;
#define LOG_ID_INVALID  (-1)

// Thread pool settings
#define LOG_THREAD_POOL_Q_LEN           1024
#define LOG_THREAD_POOL_NUM_THREADS     1

// IDs for each sink
#define LOG_SINK_FILE_INDEX         0
#define LOG_SINK_CONSOLE_INDEX      1

// Log IDs should all be specified here
#define MAX_ASYNC_LOGS  4
#define MAIN_LOG_ID     0 // The default log
#define FRAME_LOG_ID    1 // The log for frame events
#define GAME_MANAGER_LOG_ID  2 // The log for game manager events
#define VISION_LOG_ID   3 // The log for vision module events

// LogLevel definitions
typedef ssize_t LogLevel;
#define LOGGING_LEVEL_NONE          0
#define LOGGING_LEVEL_VERBOSE       1
#define LOGGING_LEVEL_INFO          2
#define LOGGING_LEVEL_WARNING       3
#define LOGGING_LEVEL_ERROR         4
#define LOGGING_LEVEL_CRITICAL      5

// definitions for doing the actual logging
#define LOG_VERBOSE(logID, ...)     logAsyncMessage(logID, LOGGING_LEVEL_VERBOSE, __VA_ARGS__)
#define LOG_INFO(logID, ...)        logAsyncMessage(logID, LOGGING_LEVEL_INFO, __VA_ARGS__)
#define LOG_WARNING(logID, ...)     logAsyncMessage(logID, LOGGING_LEVEL_WARNING, __VA_ARGS__)
#define LOG_ERROR(logID, ...)       logAsyncMessage(logID, LOGGING_LEVEL_ERROR, __VA_ARGS__)
#define LOG_CRITICAL(logID, ...)    logAsyncMessage(logID, LOGGING_LEVEL_CRITICAL, __VA_ARGS__)

/**
 * Initializes logging module as well as a main asynchronous log.
 */
Status initializeLoggingModule(LogLevel logLevel);

/**
 * Shuts down logging module and clears list of async queues
 */
void shutdownLoggingModule();

/**
 * Set the async log that is attached to the console
 */
Status setConsoleLog(LogID logID, LogLevel consoleLogLevel);

/**
 * Get a logger by ID (returns nullptr if invalid)
 */
LoggerPtr getLogger(LogID logID);

/**
 * Logs a message with the given logging level, string should use {} formatting notation
 */
template<typename... Args>
void logAsyncMessage(LogID asyncLogID, LogLevel level, spdlog::format_string_t<Args...> fmt, Args&&... args)
{
    auto logger = getLogger(asyncLogID);
    if (!logger) return;

    spdlog::level::level_enum spdLevel;
    switch(level)
    {
        case LOGGING_LEVEL_VERBOSE:  spdLevel = spdlog::level::trace; break;
        case LOGGING_LEVEL_INFO:     spdLevel = spdlog::level::info; break;
        case LOGGING_LEVEL_WARNING:  spdLevel = spdlog::level::warn; break;
        case LOGGING_LEVEL_ERROR:    spdLevel = spdlog::level::err; break;
        case LOGGING_LEVEL_CRITICAL: spdLevel = spdlog::level::critical; break;
        default: return;
    }

    logger->log(spdLevel, fmt, std::forward<Args>(args)...);
}

#endif // COMMON_LOGGING_HPP
