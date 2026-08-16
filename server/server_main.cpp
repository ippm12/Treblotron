/**
 * server_main.cpp
 *
 * Entry point for the headless dart-inference server. Deliberately separate
 * from the game's startup.cpp: this binary links no SDL, no flecs, and none of
 * the game modules — only debug, dart, detect, net and server.
 */

#include "common_inc.hpp"
#include "server/server.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>


namespace
{
    std::atomic<bool> f_stop{false};

    extern "C" void onSignal(int) { f_stop.store(true, std::memory_order_release); }

    void printUsage(const char* argv0)
    {
        std::printf(
            "Usage: %s [options]\n"
            "\n"
            "  --port <n>            TCP port to listen on (default 9876)\n"
            "  --model-dir <path>    Directory holding the ONNX models\n"
            "                        (default ./detect/models)\n"
            "\n"
            "Detection tuning. These are defaults: a connected client sends its\n"
            "own from its Vision screen and those win for the session. Durations,\n"
            "not frame counts, because the cycle rate is a property of the backend\n"
            "and has ranged from 4 to 55 Hz.\n"
            "  --confirm <n>         Minimum consecutive detections for a dart\n"
            "                        (default 3)\n"
            "  --confirm-hold <ms>   A dart must hold position this long to count -\n"
            "                        what separates a landed dart from one still in\n"
            "                        flight (default 300)\n"
            "  --clear-hold <ms>     Board must be quiet this long before a turn can\n"
            "                        end - no hand, no leftover dart (default 1000)\n"
            "  --hand-enter <ms>     Hand present this long to start removing\n"
            "                        (default 100)\n"
            "  --no-hand-filter      Skip the palm/landmark stages\n"
            "\n"
            "Captures:\n"
            "  --capture-dir <path>  Where captures are written (default ./captures)\n"
            "  --capture-on-detect   Save the frames behind every confirmed dart\n"
            "\n"
            "Link:\n"
            "  --read-timeout <ms>   Drop a silent client after this long\n"
            "                        (default 20000)\n"
            "  --grace <ms>          Keep board state this long for a reconnect\n"
            "                        (default 60000)\n"
            "  --no-heatmap          Never send heatmap frames, even if asked\n"
            "\n"
            "Offline:\n"
            "  --selftest            Load the models, run one pass, and exit\n"
            "  --replay <dir>        Score saved {uuid}_camN.png triples and exit\n"
            "  --calibration <path>  Calibration for --replay\n"
            "                        (default ./config/wire_calibration.txt)\n"
            "  -h, --help            Show this message\n",
            argv0);
    }

    /** Read the value following `--flag`, or report the missing argument. */
    bool nextArg(int argc, char** argv, int& i, const char* flag, std::string& out)
    {
        if(i + 1 >= argc)
        {
            std::fprintf(stderr, "%s requires a value\n", flag);
            return false;
        }
        out = argv[++i];
        return true;
    }
}


int main(int argc, char** argv)
{
    DartServerConfig config;
    std::string replayDir;
    std::string calibrationPath = "./config/wire_calibration.txt";
    bool selfTest = false;

    for(int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        std::string value;

        if(std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0)
        {
            printUsage(argv[0]);
            return 0;
        }
        else if(std::strcmp(a, "--port") == 0)
        {
            if(!nextArg(argc, argv, i, a, value)) return -1;
            config.port = static_cast<uint16_t>(std::atoi(value.c_str()));
        }
        else if(std::strcmp(a, "--model-dir") == 0)
        {
            if(!nextArg(argc, argv, i, a, config.modelDir)) return -1;
        }
        else if(std::strcmp(a, "--confirm") == 0)
        {
            if(!nextArg(argc, argv, i, a, value)) return -1;
            config.settings.tuning.confirmFrames = std::atoi(value.c_str());
        }
        else if(std::strcmp(a, "--confirm-hold") == 0)
        {
            if(!nextArg(argc, argv, i, a, value)) return -1;
            config.settings.tuning.confirmHoldMs = std::atoi(value.c_str());
        }
        else if(std::strcmp(a, "--capture-on-detect") == 0)
        {
            config.settings.captureOnDetect = true;
        }
        else if(std::strcmp(a, "--capture-dir") == 0)
        {
            if(!nextArg(argc, argv, i, a, config.captureDir)) return -1;
        }
        else if(std::strcmp(a, "--clear-hold") == 0)
        {
            if(!nextArg(argc, argv, i, a, value)) return -1;
            config.settings.tuning.clearHoldMs = std::atoi(value.c_str());
        }
        else if(std::strcmp(a, "--hand-enter") == 0)
        {
            if(!nextArg(argc, argv, i, a, value)) return -1;
            config.settings.tuning.handEnterMs = std::atoi(value.c_str());
        }
        else if(std::strcmp(a, "--read-timeout") == 0)
        {
            if(!nextArg(argc, argv, i, a, value)) return -1;
            config.readTimeoutMs = std::atoi(value.c_str());
        }
        else if(std::strcmp(a, "--grace") == 0)
        {
            if(!nextArg(argc, argv, i, a, value)) return -1;
            config.reconnectGraceMs = std::atoi(value.c_str());
        }
        else if(std::strcmp(a, "--no-hand-filter") == 0)
        {
            config.enableHandFilter = false;
        }
        else if(std::strcmp(a, "--no-heatmap") == 0)
        {
            config.allowHeatmap = false;
        }
        else if(std::strcmp(a, "--selftest") == 0)
        {
            selfTest = true;
        }
        else if(std::strcmp(a, "--replay") == 0)
        {
            if(!nextArg(argc, argv, i, a, replayDir)) return -1;
        }
        else if(std::strcmp(a, "--calibration") == 0)
        {
            if(!nextArg(argc, argv, i, a, calibrationPath)) return -1;
        }
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n\n", a);
            printUsage(argv[0]);
            return -1;
        }
    }

    // Bounds live with the struct, so a bad --clear-hold is corrected the same
    // way a bad one off the network is.
    clampDartTuning(config.settings.tuning);

    if(IS_STATUS_NOT_OK(initializeLoggingModule(LOGGING_LEVEL_INFO)))
    {
        std::fprintf(stderr, "Failed to initialize logging\n");
        return -1;
    }
    setConsoleLog(SERVER_LOG_ID, LOGGING_LEVEL_INFO);
    // The detector's own log too. This is a headless tool with no other UI, and
    // the things it says are the things you need: which conditioning layout the
    // export declares, and why a model refused to load. Without it a mismatched
    // export surfaces on the console as a bare "models did not load".
    setConsoleLog(DETECT_LOG_ID, LOGGING_LEVEL_INFO);

    int exitCode = 0;

    if(selfTest)
    {
        exitCode = IS_STATUS_OK(runServerSelfTest(config)) ? 0 : -1;
    }
    else if(!replayDir.empty())
    {
        exitCode = IS_STATUS_OK(runServerReplay(config, replayDir, calibrationPath)) ? 0 : -1;
    }
    else
    {
        std::signal(SIGINT,  onSignal);
        std::signal(SIGTERM, onSignal);

        if(IS_STATUS_NOT_OK(initializeServerModule(config)))
        {
            shutdownLoggingModule();
            return -1;
        }

        LOG_INFO(SERVER_LOG_ID, "press Ctrl-C to stop");
        runServer(f_stop);
        shutdownServerModule();
    }

    shutdownLoggingModule();
    return exitCode;
}
