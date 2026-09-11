/**
 * logging.cpp
 * 
 * Functions to abstract logging functions
 */

#include <memory>
#include <vector>
#include <string>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "common_types.hpp"
#include "debug/app_paths.hpp"
#include "debug/common_logging.hpp"

#define LOG_ID_IS_VALID(logid) ((logid >= 0) && (logid < MAX_ASYNC_LOGS))

// Whether logging has been initialized or not
static bool f_loggingInitialized = false;

// A list of logs to write to.  Indexed by a LogLevel
static LoggerPtr f_loggers[MAX_ASYNC_LOGS];
static std::string f_loggerNames[MAX_ASYNC_LOGS] = {"Main", "Frame", "GameManager", "Vision"};

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
static Status createLog(LogID logID, LogLevel logLevel, std::string& name)
{
    try
    {
        // Synchronous delivery: no queued messages are lost on process failure.
        std::string logger_name = std::to_string(logID) + "_" + name;
        // Preserve previous runs, with bounded retention (three 5 MiB backups).
        std::string log_filename =
            appDataPath("logs/treblotron_" + logger_name + ".log");

        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_filename, 5 * 1024 * 1024, 3, true);
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        std::vector<spdlog::sink_ptr> sinks{file_sink, console_sink};

        auto logger = std::make_shared<spdlog::logger>(
            logger_name, sinks.begin(), sinks.end());
        // fflush every accepted message before its logging call returns.
        // This survives abrupt process exit, not power loss or an interrupted write.
        logger->flush_on(spdlog::level::trace);

        // Set the log level
        SpdLogLevel spdLevel = convertLogLevel(logLevel);
        logger->set_level(spdLevel);
        console_sink->set_level(spdlog::level::off);

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

    // Init logging array to nullptrs
    for(int i = 0; i < MAX_ASYNC_LOGS; i++)
    {
        // Create a new log at each index
        Status status = createLog(i, logLevel, f_loggerNames[i]);
        if(IS_STATUS_NOT_OK(status))
        {
            shutdownLoggingModule();
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

    f_consoleLogID=logID;
    return STATUS_OK;
}


void shutdownLoggingModule()
{
    for(auto& logger:f_loggers) if(logger) logger->flush();
    spdlog::shutdown();
    for(auto& logger:f_loggers) logger.reset();
    f_consoleLogID=LOG_ID_INVALID;
    f_loggingInitialized=false;
}


LoggerPtr getLogger(LogID logID)
{
    if (!LOG_ID_IS_VALID(logID))
    {
        return nullptr;
    }
    return f_loggers[logID];
}
