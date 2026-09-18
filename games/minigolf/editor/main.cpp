#include "editor.hpp"
#include "debug/crash_reporting.hpp"
#include "common_inc.hpp"
#include <iostream>
int main(int argc,char** argv){
    int helperExit=0;
    if(runCrashDumpHelper(argc,argv,helperExit))return helperExit;
    initializeCrashReporting();
    // Keep the game's rotating log files untouched when both apps are open.
    // Editor startup errors go to stderr; fatal failures use the shared dump helper.
    if(initializeFrameModule()!=STATUS_OK){std::cerr<<SDL_GetError()<<std::endl;return 1;}
    FrameID frame=INVALID_FRAME_ID;
    if(createNewFrame("Treblotron Course Editor",1440,900,frame)!=STATUS_OK){std::cerr<<SDL_GetError()<<std::endl;shutdownFrameModule();
        return 1;
    }
    int result=0;
    try{GolfEditor::Editor editor(frame);
        result=editor.run(argc==3 && std::string(argv[1])=="--smoke" ? argv[2]:"");
    }
    catch(const std::exception& e){std::cerr<<e.what()<<std::endl;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Course editor",e.what(),nullptr);
        result=1;
    }
    deleteFrame(frame);
    shutdownFrameModule();
    return result;
}
