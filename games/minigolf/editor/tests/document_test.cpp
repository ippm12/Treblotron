#include "../document.hpp"
#include "course_preview.hpp"
#include "frame/render_queue.hpp"
#include "debug/crash_reporting.hpp"
#include "debug/app_paths.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace GolfEditor;
int main(int argc,char** argv) {
    int helperExit=0;if(runCrashDumpHelper(argc,argv,helperExit))return helperExit;
    initializeCrashReporting();
    Document doc;doc.create();validateCourse(doc.course);
    const auto path=std::filesystem::current_path()/"saved-course";
    doc.save(path);assert(!doc.dirty());
    doc.edit([&]{doc.course.theme="double-dunes";});assert(doc.dirty());
    doc.undo();assert(doc.course.theme=="classic");doc.redo();assert(doc.course.theme=="double-dunes");
    doc.save(path);Document themed;themed.load(path);assert(themed.course.theme=="double-dunes");
    doc.add(Kind::Wall,{400,400},{40,40});doc.add(Kind::Wall,{440,400},{40,40});
    doc.edit([&]{doc.current().walls[0].group=doc.current().walls[1].group="barrier";});
    doc.select({Kind::Wall,0});assert(doc.selection.size()==2);
    doc.edit([&]{doc.rotate(45);});
    assert(std::abs(doc.current().walls[1].centerX-(400+40/std::sqrt(2.0)))<0.01);
    assert(std::abs(doc.current().walls[1].centerY-(400+40/std::sqrt(2.0)))<0.01);
    assert(doc.current().walls[0].angleDegrees==45 && doc.current().walls[1].angleDegrees==45);
    doc.undo();doc.select({Kind::Wall,0});
    doc.edit([&]{doc.translate({80,20});});assert(doc.current().walls[1].centerX==520);
    doc.undo();assert(doc.current().walls[1].centerX==440);doc.redo();assert(doc.current().walls[1].centerX==520);
    doc.select({Kind::Wall,0});doc.duplicate();assert(doc.current().walls.size()==4);assert(doc.current().walls[2].group==doc.current().walls[3].group);assert(doc.current().walls[2].group!="barrier");
    doc.erase();assert(doc.current().walls.size()==2);
    auto before=doc.revision;bool rejected=false;
    try{doc.edit([&]{doc.current().walls[0].width=-10;});}catch(const std::exception&){rejected=true;}
    assert(rejected && doc.current().walls[0].width==40 && doc.revision==before);
    doc.portals({300,300},{900,300});doc.duplicate();assert(doc.current().portals.size()==4);validateCourse(doc.course);
    doc.select({Kind::Portal,0});doc.erase();assert(doc.current().portals.size()==2);doc.undo();assert(doc.current().portals.size()==4);
    doc.edit([&]{for(int i=0;i<2;++i)doc.current().rails.push_back({{{{0,0},0,100},{{100,0},1,50}},RailMode::PingPong});doc.attach({Kind::Wall,0},1);doc.attach({Kind::Cup,0},0);});
    doc.edit([&]{doc.removeRail(0);});assert(doc.current().cupRail==-1 && doc.current().walls[0].rail==0 && doc.current().walls[1].rail==0);
    doc.add(Kind::Surface,{700,500},{100,100},SurfaceKind::Ice);doc.attach(doc.selection[0],0);assert(doc.rail(doc.selection[0])==-1);
    doc.save(path);Document reloaded;reloaded.load(path);assert(reloaded.current().walls[0].id==doc.current().walls[0].id);
    doc.edit([&]{doc.current().par=4;});doc.undo();assert(!doc.dirty());
    Document bundled;bundled.load(appAssetPath("assets/games/minigolf/courses/obstacle-sampler"));assert(bundled.saveDirectory==std::filesystem::path(appDataPath("courses"))/"obstacle-sampler");
    assert(initializeFrameModule()==STATUS_OK);FrameID frame;assert(createNewFrame("Editor tests",1440,900,frame)==STATUS_OK);
    {
        CoursePreview preview(frame);auto hole=blankCourse().holes[0];preview.setHole(hole,true);assert(preview.ready());preview.putt({0,-400});
        for(int i=0;i<60;++i)preview.advance(1.0f/120);
        assert(preview.ballPosition().y<hole.startPos.y-50);
        preview.resetBall(hole.cupPos);for(int i=0;i<120;++i)preview.advance(1.0f/120);assert(preview.holed());
        preview.setHole(hole,false,"double-dunes");preview.seek(3);assert(preview.time()==3);preview.draw();
        assert(renderQueueDrawFlush(frame)==STATUS_OK);
        hole.surfaces={{{705,500},{1000000,1000000},SurfaceKind::Sand}};
        preview.setHole(hole,false);preview.view({705,500},1);preview.draw();
        assert(renderQueueDrawFlush(frame)==STATUS_OK);
    }
    deleteFrame(frame);shutdownFrameModule();std::cout<<"PASS editor document, safe saves, history, groups, portal pairs, rail references and shared physics"<<std::endl;
}
