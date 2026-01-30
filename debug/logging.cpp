/**
 * logging.cpp
 * 
 * Functions to abstract logging functions
 */

#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "common_types.hpp"
#include "debug/common_logging.hpp"

#define LOG_ID_IS_VALID(logid) ((logid >= 0) && (logid < MAX_ASYNC_LOGS))

// Whether logging has been initialized or not
static bool f_loggingInitialized = false;

// A list of logs to write to.  Indexed by a LogLevel
static LoggerPtr f_loggers[MAX_ASYNC_LOGS];
static std::string f_loggerNames[MAX_ASYNC_LOGS] = {"Main"};

// The current console log, this defaults to the main log
static LogID f_consoleLogID = LOG_ID_INVALID;

typedef spdlog::level::level_enum SpdLogLevel;

/**
 * Convert common log level to SpdLog level
 */
static inline SpdLogLevel convertLogLevel(LogLevel input)
{
    SpdLogLevel newLevel;

    switch(input)
    {
        case LOGGING_LEVEL_VERBOSE:
            newLevel = spdlog::level::trace;
            break;
        case LOGGING_LEVEL_INFO:
            newLevel = spdlog::level::info;
            break;
        case LOGGING_LEVEL_WARNING:
            newLevel = spdlog::level::warn;
            break;
        case LOGGING_LEVEL_ERROR:
            newLevel = spdlog::level::err;
            break;
        case LOGGING_LEVEL_CRITICAL:
            newLevel = spdlog::level::critical;
            break;
        default:
            newLevel = spdlog::level::off;
            break;
    }

    return newLevel;
}


/**
 * Create a log with the given ID at the given level
 */
static Status createAsyncLog(LogID logID, LogLevel logLevel, std::string& name)
{
    try
    {
        // Create async logger that logs to both console and file
        std::string logger_name = std::to_string(logID) + "_" + name;
        std::string log_filename = "logs/dart_lens_" + logger_name + ".log";

        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_filename, true);
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        std::vector<spdlog::sink_ptr> sinks{file_sink, console_sink};

        auto logger = std::make_shared<spdlog::async_logger>(
            logger_name,
            sinks.begin(),
            sinks.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);

        // Set the log level
        SpdLogLevel spdLevel = convertLogLevel(logLevel);
        logger->set_level(spdLevel);

        // Set pattern for log messages
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

        // Store the logger in the array
        f_loggers[logID] = logger;

        // Register with spdlog
        spdlog::register_logger(logger);

        return STATUS_OK;
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        return STATUS_ERROR_GENERIC;
    }
}


Status initializeLoggingModule(LogLevel logLevel)
{
    if(f_loggingInitialized)
    {
        // We have already initialized the logger.  Log an error.
        LOG_ERROR(MAIN_LOG_ID, "Logging already initialized");
        return STATUS_ERROR_GENERIC;
    }

    // Initialize the thread pool
    spdlog::init_thread_pool(LOG_THREAD_POOL_Q_LEN, LOG_THREAD_POOL_NUM_THREADS);

    // Init logging array to nullptrs
    for(int i = 0; i < MAX_ASYNC_LOGS; i++)
    {
        // Create a new log at each index
        Status status = createAsyncLog(i, logLevel, f_loggerNames[i]);
        if(IS_STATUS_NOT_OK(status))
        {
            return STATUS_ERROR_GENERIC;
        }
    }

    // We start off with the console log attached to the main log
    spdlog::set_default_logger(f_loggers[MAIN_LOG_ID]);
    Status status = setConsoleLog(MAIN_LOG_ID, logLevel);
    if(IS_STATUS_NOT_OK(status))
    {
        return status;
    }

    f_loggingInitialized = true;

    // Log that initialization was successful
    LOG_INFO(MAIN_LOG_ID, "Logging module initialized successfully");

    return STATUS_OK;
}

Status setConsoleLog(LogID logID, LogLevel consoleLogLevel)
{
    if(!LOG_ID_IS_VALID(logID))
    {
        LOG_ERROR(MAIN_LOG_ID, "Unable to set console log to {}", logID);
        return STATUS_ERROR_INVALID_PARAM;
    }

    // Disable console from old log
    if(f_consoleLogID != LOG_ID_INVALID)
    {
        try
        {
            f_loggers[f_consoleLogID]->sinks().at(LOG_SINK_CONSOLE_INDEX)->set_level(spdlog::level::off);
        }
        catch(std::out_of_range const&)
        {
            LOG_ERROR(MAIN_LOG_ID, "Error disabling console sink at index {}", f_consoleLogID);
            return STATUS_ERROR_GENERIC;
        }
    }

    // Enable the new logger
    try
    {
        SpdLogLevel spdLevel = convertLogLevel(consoleLogLevel);
        f_loggers[logID]->sinks().at(LOG_SINK_CONSOLE_INDEX)->set_level(spdLevel);
    }
    catch(std::out_of_range const&)
    {
        LOG_ERROR(MAIN_LOG_ID, "Error enabling console sink at index {}", f_consoleLogID);
        return STATUS_ERROR_GENERIC;
    }

    return STATUS_OK;
}


void shutdownLoggingModule()
{
    spdlog::shutdown();
}


LoggerPtr getLogger(LogID logID)
{
    if (!LOG_ID_IS_VALID(logID))
    {
        return nullptr;
    }
    return f_loggers[logID];
}
