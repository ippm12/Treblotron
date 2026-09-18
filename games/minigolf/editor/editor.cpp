#include "course_themes.hpp"
#include "course_motion.hpp"
#include "editor.hpp"
#include "debug/app_paths.hpp"
#include "frame/render_queue.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
namespace GolfEditor {
namespace {
constexpr SDL_Color bg{17,23,30,255},panel{27,35,45,255},accent{60,159,198,255},muted{144,165,180,255};
bool inside(SDL_FRect r,float x,float y){return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h;
}
std::string numeric(float n){std::ostringstream s;
    s<<std::setprecision(7)<<n;
    return s.str();
}
const char* names[]={"Tee","Extra tee","Cup","Wall","Surface","Bumper","Laser","Portal"};
bool movable(Object o){return o.kind==Kind::Cup || o.kind==Kind::Wall || o.kind==Kind::Bumper || o.kind==Kind::Laser || o.kind==Kind::Portal;}
}
Editor::Editor(FrameID f):frame(f),renderer(getFrameRenderer(f)),window(SDL_GetRenderWindow(renderer)),preview(f) {
    SDL_SetWindowResizable(window,true);
    SDL_SetWindowMinimumSize(window,1100,720);
    SDL_SetRenderLogicalPresentation(renderer,0,0,SDL_LOGICAL_PRESENTATION_DISABLED);
    font=TTF_OpenFont(appAssetPath("assets/fonts/Roboto-Regular.ttf").c_str(),17);
    if(!font) throw std::runtime_error(SDL_GetError());
    scene=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,1920,1080);
    if(!scene) throw std::runtime_error(SDL_GetError());
    SDL_SetTextureScaleMode(scene,SDL_SCALEMODE_LINEAR);
    doc.load(defaultCourseDirectory());
    fit();
    resetPreview();
    updateTitle();
}
Editor::~Editor(){for(auto& [key,t]:textCache){(void)key;
        SDL_DestroyTexture(t);
    }
    SDL_DestroyTexture(scene);
    TTF_CloseFont(font);
}
void Editor::text(float x,float y,const std::string& value,SDL_Color color) {
    if(value.empty()) return;
    auto key=std::to_string(color.r)+","+std::to_string(color.g)+","+std::to_string(color.b)+value;
    if(textCache.size()>1024){for(auto& [k,t]:textCache){(void)k;
            SDL_DestroyTexture(t);
        }
        textCache.clear();
    }
    auto& texture=textCache[key];
    if(!texture){auto* surface=TTF_RenderText_Blended(font,value.c_str(),value.size(),color);
        if(!surface)return;
        texture=SDL_CreateTextureFromSurface(renderer,surface);
        SDL_DestroySurface(surface);
    }
    float w=0,h=0;
    SDL_GetTextureSize(texture,&w,&h);
    SDL_FRect r{x,y,w,h};
    SDL_RenderTexture(renderer,texture,nullptr,&r);
}
void Editor::box(SDL_FRect r,SDL_Color c,bool fill){SDL_SetRenderDrawColor(renderer,c.r,c.g,c.b,c.a);
    if(fill) SDL_RenderFillRect(renderer,&r);
    else SDL_RenderRect(renderer,&r);
}
void Editor::button(SDL_FRect r,const std::string& label,std::function<void()> callback,bool active,bool enabled){
    box(r,active?accent:(enabled?SDL_Color{43,55,69,255}:SDL_Color{30,38,47,255}));
    SDL_Rect clip{int(r.x+4),int(r.y),int(r.w-8),int(r.h)};
    SDL_SetRenderClipRect(renderer,&clip);
    text(r.x+8,r.y+(r.h-21)*0.5f,label,enabled?SDL_Color{238,244,248,255}:muted);
    SDL_SetRenderClipRect(renderer,nullptr);
    if(enabled) widgets.push_back({r,"",label,std::move(callback),{}});
}
void Editor::field(float x,float y,float width,const std::string& key,const std::string& label,const std::string& value,std::function<void(const std::string&)> setter){
    text(x,y+5,label,muted);
    SDL_FRect r{x+126,y,width-126,29};
    box(r,focus==key?SDL_Color{50,81,99,255}:SDL_Color{17,25,34,255});
    SDL_Rect clip{int(r.x+5),int(r.y),int(r.w-10),int(r.h)};
    SDL_SetRenderClipRect(renderer,&clip);
    text(r.x+6,y+5,focus==key?buffer:value);
    SDL_SetRenderClipRect(renderer,nullptr);
    widgets.push_back({r,key,value,{},std::move(setter)});
}
void Editor::number(float y,const std::string& label,float value,std::function<void(float)> setter){
    int w,h;
    SDL_GetWindowSize(window,&w,&h);
    if(y<74 || y+30>h-40)return;
    field(float(w-300),y,284,label,label,numeric(value),[setter](const std::string& text){size_t used=0;
        float n=std::stof(text,&used);
        if(used!=text.size() || !std::isfinite(n))throw std::runtime_error("Enter a finite number");
        setter(n);
    });
}
void Editor::action(const std::function<void()>& f){try{f();
        updateTitle();
    }
    catch(const std::exception& e){status=e.what();
    }}
void Editor::commit(){
    if(focus.empty()) return;
    auto value=buffer;
    auto setter=std::move(focusSetter);
    focus.clear();
    SDL_StopTextInput(window);
    if(setter) action([&]{setter(value);
        resetPreview();
    });
}
void Editor::updateTitle(){SDL_SetWindowTitle(window,(doc.course.name+(doc.dirty()?" *":"")+" - Treblotron Course Editor").c_str());
}
void Editor::fit(){const auto& h=doc.current();
    center={(h.areaTopLeft.x+h.areaBottomRight.x)/2,(h.areaTopLeft.y+h.areaBottomRight.y)/2};
    const float margin=doc.course.theme=="classic" ? 100.0f:200.0f;
    zoom=std::min(COURSE_VIEW_W/(h.areaBottomRight.x-h.areaTopLeft.x+margin),COURSE_VIEW_H/(h.areaBottomRight.y-h.areaTopLeft.y+margin));
}
void Editor::resetPreview(){preview.setHole(doc.current(),testing,doc.course.theme);
    preview.view(center,zoom);
}
Vec2 Editor::world(float x,float y)const{return {center.x+(x-canvas.x-canvas.w/2)/(zoom*canvas.w/COURSE_VIEW_W),center.y+(y-canvas.y-canvas.h/2)/(zoom*canvas.w/COURSE_VIEW_W)};
}
Vec2 Editor::screen(Vec2 p)const{float s=zoom*canvas.w/COURSE_VIEW_W;
    return {canvas.x+canvas.w/2+(p.x-center.x)*s,canvas.y+canvas.h/2+(p.y-center.y)*s};
}
Vec2 Editor::snap(Vec2 p)const{if(!grid)return p;
    return {std::round(p.x/20)*20,std::round(p.y/20)*20};
}
Vec2 Editor::displayedPosition(Object o)const{
    return attachedPosition(doc.current(),doc.position(o),doc.rail(o),preview.time());
}
std::vector<Object> Editor::objects()const{
    const auto& h=doc.current();
    std::vector<Object> result;
    auto add=[&](Kind k,size_t n){for(size_t i=0;
        i<n;
        ++i)result.push_back({k,i});
    };
    if(!hideSurfaces)add(Kind::Surface,h.surfaces.size());
    add(Kind::Wall,h.walls.size());
    add(Kind::Bumper,h.bumpers.size());
    add(Kind::Laser,h.lasers.size());
    add(Kind::Portal,h.portals.size());
    result.push_back({Kind::Cup,0});
    result.push_back({Kind::Tee,0});
    add(Kind::ExtraTee,h.spawnPositions.size());
    return result;
}
std::optional<Object> Editor::hit(Vec2 p)const{
    auto list=objects();
    for(auto i=list.rbegin();
    i!=list.rend();
    ++i){if(lockedSurfaces && i->kind==Kind::Surface)continue;
        auto c=displayedPosition(*i),s=doc.extent(*i);
        float margin=7/(zoom*canvas.w/COURSE_VIEW_W);
        if(insidePatch(p,c,{s.x+2*margin,s.y+2*margin},0,doc.angle(*i)))return *i;
    }
    return {};
}
bool Editor::selectRail(float x,float y){
    if(hideRails)return false;
    auto list=objects();
    // Prefer the selected object's copy of a shared rail when paths overlap.
    for(auto o:doc.selection)list.push_back(o);
    for(bool handles:{true,false})for(auto it=list.rbegin();it!=list.rend();++it){
        auto o=*it;
        int r=doc.rail(o);
        if(r<0)continue;
        auto base=doc.position(o);
        const auto& rail=doc.current().rails[r];
        auto position=[&](size_t i){auto offset=rail.stops[i].offset;return screen({base.x+offset.x,base.y+offset.y});};
        for(size_t i=0;i<rail.stops.size();++i){
            auto a=position(i);
            bool found=false;
            if(handles)found=std::hypot(x-a.x,y-a.y)<=12;
            else if(i+1<rail.stops.size() || rail.mode==RailMode::Loop){
                auto b=position((i+1)%rail.stops.size());
                float dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;
                float t=length>0?std::clamp(((x-a.x)*dx+(y-a.y)*dy)/length,0.0f,1.0f):0;
                found=std::hypot(x-a.x-t*dx,y-a.y-t*dy)<=7;
            }
            if(!found)continue;
            doc.select(o,false);
            pointIndex=int(i);
            inspectorScroll=0;
            if(handles){dragPoint=int(i);dragging=true;dragChanged=false;last=snap(world(x,y));}
            status=handles?"Drag this waypoint. Shared rails update all attached objects.":"Rail selected. Drag a numbered handle, or enable Add points in the inspector.";
            return true;
        }
    }
    return false;
}
void Editor::fileDialog(int kind){
    if(dialogOpen)return;
    dialogOpen=true;
    struct Request{Editor* editor;
        int kind;
    };
    auto* request=new Request{this,kind};
    auto callback=[](void* data,const char* const* files,int){std::unique_ptr<Request> r(static_cast<Request*>(data));
        std::lock_guard lock(r->editor->dialogMutex);
        r->editor->dialogResult=std::pair{r->kind,files && files[0]?std::string(files[0]):std::string{}};
    };
    if(kind==0){static const SDL_DialogFileFilter filter{"Course manifest","json"};
        SDL_ShowOpenFileDialog(callback,request,window,&filter,1,appDataPath("courses").c_str(),false);
    }
    else SDL_ShowOpenFolderDialog(callback,request,window,doc.saveDirectory.string().c_str(),false);
}
bool Editor::confirmReplace(const std::filesystem::path& directory) {
    if(!std::filesystem::exists(directory/"course.json") ||
    (!doc.sourceDirectory.empty() && std::filesystem::weakly_canonical(directory)==doc.sourceDirectory)) return true;
    const SDL_MessageBoxButtonData buttons[]={{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,0,"Cancel"},{0,1,"Replace course"}};
    SDL_MessageBoxData msg{SDL_MESSAGEBOX_WARNING,window,"Replace course?","This folder already contains another course copy. Replace its files?",2,buttons,nullptr};
    int chosen=0;
    SDL_ShowMessageBox(&msg,&chosen);
    return chosen==1;
}
bool Editor::discardAllowed(){
    if(!doc.dirty())return true;
    const SDL_MessageBoxButtonData buttons[]={{0,0,"Cancel"},{0,1,"Discard"},{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,2,"Save"}};
    SDL_MessageBoxData msg{SDL_MESSAGEBOX_WARNING,window,"Unsaved course","Save your course changes before continuing?",3,buttons,nullptr};
    int chosen=0;
    SDL_ShowMessageBox(&msg,&chosen);
    if(chosen==2){save();
        return !doc.dirty();
    }
    return chosen==1;
}
void Editor::save(bool as){
    commit();
    if(as){fileDialog(1);
        return;
    }
    action([&]{if(!confirmReplace(doc.saveDirectory)) return;
        doc.save(doc.saveDirectory);
        status="Saved: "+doc.saveDirectory.string();
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(appDataPath("course-editor/recovery"))/"course.json",ec);
    });
}
void Editor::open(const std::filesystem::path& path){if(!discardAllowed())return;
    doc.load(path);
    testing=false;
    animated=false;
    doc.selection.clear();
    firstPortal.reset();
    inspectorScroll=0;
    fit();
    resetPreview();
    status="Opened: "+path.string();
    updateTitle();
}
void Editor::recovery(){const auto path=std::filesystem::path(appDataPath("course-editor/recovery"));
    if(!std::filesystem::exists(path/"course.json")){status="No autosave to recover.";
        return;
    }
    if(!discardAllowed())return;
    doc.load(path);
    doc.saveDirectory=std::filesystem::path(appDataPath("courses"))/doc.course.id;
    doc.checkpoint();
    testing=false;
    fit();
    resetPreview();
    status="Recovered autosave. Save to keep it as a local course.";
}
void Editor::tile(Vec2 p){
    p={std::floor(p.x/40)*40+20,std::floor(p.y/40)*40+20};
    auto& h=doc.current();
    for(const auto& w:h.walls)if(w.centerX==p.x && w.centerY==p.y && w.width==40 && w.height==40)return;
    if(!dragChanged){doc.checkpoint();
        dragChanged=true;
    }
    bool showRail=std::none_of(h.walls.begin(),h.walls.end(),[&](const auto& w){return w.group==paintGroup;
    });
    h.walls.push_back({p.x,p.y,40,40,-1,showRail,newId("wall"),paintGroup});
    doc.selection={{Kind::Wall,h.walls.size()-1}};
    resetPreview();
}
void Editor::canvasDown(const SDL_MouseButtonEvent& e){
    if(!inside(canvas,e.x,e.y))return;
    Vec2 p=world(e.x,e.y);
    if(e.button==SDL_BUTTON_MIDDLE){panning=true;
        last={e.x,e.y};
        return;
    }
    if(testing){if(e.button==SDL_BUTTON_RIGHT){preview.resetBall(p);
            status="Ball placed here for testing.";
        }
        else if(preview.ready()){shooting=true;
            shotStart=p;
        }
        return;
    }
    if(e.button==SDL_BUTTON_RIGHT){firstPortal.reset();
        addingWaypoints=false;
        tool=0;
        return;
    }
    if(e.button!=SDL_BUTTON_LEFT)return;
    if(animated) {status="Pause motion before editing objects.";
        return;
    }
    if(tool==0){
        if(!doc.selection.empty()){
            auto o=doc.selection.front();
            if(o.kind==Kind::Wall || o.kind==Kind::Surface || o.kind==Kind::Laser){
                auto c=displayedPosition(o),size=doc.extent(o);
                float scale=zoom*canvas.w/COURSE_VIEW_W;
                auto d=rotateVector({0,-size.y/2-28/scale},doc.angle(o));
                auto handle=screen({c.x+d.x,c.y+d.y});
                if(std::hypot(e.x-handle.x,e.y-handle.y)<=12){
                    dragPoint=-5;dragging=true;dragChanged=false;last=p;return;
                }
                if(o.kind==Kind::Laser)for(int end=0;end<2;++end){
                    auto h=screen(laserEnd(doc.current().lasers[o.index],c,end));
                    if(std::hypot(e.x-h.x,e.y-h.y)<=12){
                        dragPoint=-3-end;dragging=true;dragChanged=false;last=snap(p);return;
                    }
                }
            }
        }
        if(doc.selection.size()==1) {
            auto o=doc.selection.front();
            if(o.kind==Kind::Wall || o.kind==Kind::Surface) {
                auto pos=displayedPosition(o),size=doc.extent(o);
                auto corner=rotateVector({size.x/2,size.y/2},doc.angle(o));
                auto handle=screen({pos.x+corner.x,pos.y+corner.y});
                if(std::hypot(e.x-handle.x,e.y-handle.y)<12) {
                    dragPoint=-2;
                    dragging=true;
                    dragChanged=false;
                    dragStart={pos.x-corner.x,pos.y-corner.y};
                    last=snap(p);
                    return;
                }
            }
        }

        auto found=hit(p);
        if((!found || found->kind==Kind::Surface) && selectRail(e.x,e.y))return;
        if(!found){doc.selection.clear();
            return;
        }
        doc.select(*found,(SDL_GetModState()&SDL_KMOD_SHIFT)!=0);
        dragging=true;
        dragChanged=false;
        last=snap(p);
        pointIndex=0;
        inspectorScroll=0;
    }
    else if(tool==1){dragging=true;
        dragChanged=false;
        paintGroup=newId("barrier");
        tile(p);
        last=p;
    }
    else if(tool>=2 && tool<=6){dragging=true;
        dragStart=snap(p);
        last=dragStart;
        dragChanged=false;
    }
    else if(tool==7){doc.add(Kind::Bumper,snap(p));
        resetPreview();
    }
    else if(tool==8){if(!firstPortal)firstPortal=snap(p);
        else{doc.portals(*firstPortal,snap(p));
            firstPortal.reset();
            resetPreview();
        }}
    else if(tool==9){doc.add(Kind::Tee,snap(p));
        resetPreview();
    }
    else if(tool==10){doc.add(Kind::Cup,snap(p));
        resetPreview();
    }
    else if(tool==11){doc.add(Kind::ExtraTee,snap(p));
        resetPreview();
    }
    else if(tool==12){
        if(selectRail(e.x,e.y))return;
        auto clicked=hit(p);
        if(clicked && movable(*clicked)){
            doc.select(*clicked,false);
            pointIndex=0;
            inspectorScroll=0;
            addingWaypoints=doc.rail(*clicked)<0;
            status=addingWaypoints?"Click to place the next waypoint of a new rail.":"Object selected. Drag rail handles, or enable Add points in the inspector.";
            return;
        }
        if(doc.selection.empty() || !movable(doc.selection.front())){
            auto found=hit(p);
            if(found && movable(*found)){
                doc.select(*found,false);
                inspectorScroll=0;
                status="Object selected. Click to place its next rail waypoint.";
            } else status="Select a wall, cup, bumper, laser or portal. Surfaces and tees cannot move.";
            return;
        }
        auto o=doc.selection.front();
        int rail=doc.rail(o);
        if(rail>=0 && !addingWaypoints){
            status="Drag a rail handle to move it. Enable Add points in the inspector to extend the path.";
            return;
        }
        auto base=doc.position(o);
        auto target=snap(p);
        if(rail<0 && std::hypot(target.x-base.x,target.y-base.y)<1){
            status="Click away from the object to place the next waypoint.";
            return;
        }
        doc.edit([&]{
            if(rail<0){
                Rail path;
                path.id=newId("rail");
                path.stops.push_back({{0,0},0,100,newId("stop")});
                rail=int(doc.current().rails.size());
                doc.current().rails.push_back(path);
                for(auto s:doc.selection)if(movable(s))doc.attach(s,rail);
            }
            doc.current().rails[rail].stops.push_back({{target.x-base.x,target.y-base.y},0,100,newId("stop")});
        });
        pointIndex=int(doc.current().rails[rail].stops.size()-1);
        hideRails=false;
        addingWaypoints=true;
        resetPreview();
        status="Waypoint added. Click for more; Select / move to drag points. Edit speed and pauses in the inspector.";
    }
}
void Editor::canvasMove(float x,float y){
    if(panning){float s=zoom*canvas.w/COURSE_VIEW_W;
        center.x-=(x-last.x)/s;
        center.y-=(y-last.y)/s;
        last={x,y};
        return;
    }
    if(!dragging || testing)return;
    auto p=snap(world(x,y));
    if(tool==1){
        if(inside(canvas,x,y)) {
            auto end=world(x,y);
            auto start=last;
            int steps=std::max(1,int(std::ceil(std::hypot(end.x-start.x,end.y-start.y)/20)));
            for(int i=1;
            i<=steps;
            ++i) tile({start.x+(end.x-start.x)*i/steps,start.y+(end.y-start.y)*i/steps});
            last=end;
        }
        return;
    }
    if(tool!=0 && !(tool==12 && dragPoint>=0)){last=p;
        return;
    }
    if(dragPoint==-5 && !doc.selection.empty()){
        auto o=doc.selection.front();
        auto c=displayedPosition(o),mouse=world(x,y);
        if(std::hypot(mouse.x-c.x,mouse.y-c.y)<1)return;
        if(!dragChanged){doc.checkpoint();dragChanged=true;}
        float desired=std::atan2(mouse.y-c.y,mouse.x-c.x)*57.295779513f+90;
        doc.rotate(std::remainder(desired-doc.angle(o),360.0f));
        float t=preview.time();resetPreview();preview.seek(t);return;
    }
    Vec2 delta{p.x-last.x,p.y-last.y};
    if(delta.x==0 && delta.y==0)return;
    if(!dragChanged){doc.checkpoint();
        dragChanged=true;
    }
    if((dragPoint==-3 || dragPoint==-4) && !doc.selection.empty()){
        auto o=doc.selection.front();
        auto& laser=doc.current().lasers.at(o.index);
        int end=-dragPoint-3;
        auto c=displayedPosition(o),opposite=laserEnd(laser,laser.center,1-end);
        Vec2 target{p.x-(c.x-laser.center.x),p.y-(c.y-laser.center.y)};
        if(end==0)setLaserEnds(laser,target,opposite);else setLaserEnds(laser,opposite,target);
    }
    else if(dragPoint==-2 && doc.selection.size()==1) {
        auto o=doc.selection.front();
        auto local=rotateVector({p.x-dragStart.x,p.y-dragStart.y},-doc.angle(o));
        Vec2 size{std::max(4.0f,local.x),std::max(4.0f,local.y)};
        auto previous=displayedPosition(o);
        auto half=rotateVector({size.x/2,size.y/2},doc.angle(o));
        doc.move(o,{dragStart.x+half.x-previous.x,dragStart.y+half.y-previous.y});
        auto& h=doc.current();
        if(o.kind==Kind::Wall){h.walls[o.index].width=size.x;
            h.walls[o.index].height=size.y;
        }
        else if(o.kind==Kind::Surface)h.surfaces[o.index].size=size;
        else h.lasers[o.index].size=size;
    }
    else if(dragPoint>=0 && !doc.selection.empty()){auto o=doc.selection.front();
        auto& stop=doc.current().rails.at(doc.rail(o)).stops.at(dragPoint);
        stop.offset.x+=delta.x;
        stop.offset.y+=delta.y;
    }
    else doc.translate(delta);
    last=p;
    float time=preview.time();
    resetPreview();
    preview.seek(time);
}
void Editor::canvasUp(){
    if(shooting){auto p=world(mouseX,mouseY);
        preview.putt({(shotStart.x-p.x)*3,(shotStart.y-p.y)*3});
        shooting=false;
    }
    if(dragging && tool>=2 && tool<=6){Vec2 p{(dragStart.x+last.x)/2,(dragStart.y+last.y)/2},s{std::max(20.0f,std::abs(last.x-dragStart.x)),std::max(20.0f,std::abs(last.y-dragStart.y))};
        if(tool==6){
            if(std::hypot(last.x-dragStart.x,last.y-dragStart.y)>=4){
                doc.edit([&]{Laser laser;setLaserEnds(laser,dragStart,last);doc.current().lasers.push_back(laser);});
                doc.select({Kind::Laser,doc.current().lasers.size()-1});
            } else status="Drag between two different points to place a laser beam.";
        }
        else doc.add(Kind::Surface,p,s,static_cast<SurfaceKind>(tool==2?1:tool==3?2:tool==4?0:3));
        resetPreview();
    }
    if(dragging && tool==1 && !doc.selection.empty()) doc.select(doc.selection.front());
    dragging=false;
    panning=false;
    dragPoint=-1;
    updateTitle();
}
void Editor::inspector(){
    int w,height;
    SDL_GetWindowSize(window,&w,&height);
    float x=float(w-300),y=88-inspectorScroll;
    inspectorBottom=88;
    auto& h=doc.current();
    auto input=[&](const std::string& label,float value,std::function<void(float)> set){number(y,label,value,[this,set](float n){doc.edit([&]{set(n);
            });
        });
        y+=35;
        inspectorBottom=y+inspectorScroll;
    };
    auto prop=[&](const std::string& label,std::function<float&(CourseHole&)> access){float value=access(h);
        input(label,value,[this,access](float n){access(doc.current())=n;
        });
    };
    auto label=[&](const std::string& value){if(y>=74 && y<height-65)text(x,y,value,accent);
        y+=30;
        inspectorBottom=y+inspectorScroll;
    };
    auto btn=[&](const std::string& caption,std::function<void()> f){if(y>=74 && y<height-75)button({x,y,284,28},caption,[this,f]{f();
            resetPreview();
        });
        y+=34;
        inspectorBottom=y+inspectorScroll;
    };
    auto stringField=[&](const std::string& key,const std::string& title,const std::string& value,std::function<void(const std::string&)> set){if(y>=74 && y<height-75)field(x,y,284,key,title,value,[this,set](const std::string& v){doc.edit([&]{set(v);
            });
        });
        y+=35;
        inspectorBottom=y+inspectorScroll;
    };
    if(testing){label("PLAYTEST");
        label(preview.holed()?"In the cup!":"Drag backward to putt.");
        label("Right-click: place ball");
        label("Reset returns to the tee.");
        return;
    }
    if(doc.selection.empty()){
        label("COURSE & HOLE");
        btn(std::string("Theme: ")+courseTheme(doc.course.theme).name,[this]{doc.edit([&]{
            for(size_t i=0;i<COURSE_THEMES.size();++i) if(doc.course.theme==COURSE_THEMES[i].id) {
                doc.course.theme=COURSE_THEMES[(i+1)%COURSE_THEMES.size()].id; break;
            }
        });fit();});
        stringField("course-name","Course name",doc.course.name,[this](const auto& s){doc.course.name=s;
        });
        stringField("hole-name","Hole name",h.name,[this](const auto& s){doc.current().name=s;
        });
        input("Par",float(h.par),[this](float n){if(n!=std::floor(n))throw std::runtime_error("Par must be an integer");
            doc.current().par=int(n);
        });
        prop("Left edge",[](auto& a)->float&{return a.areaTopLeft.x;
        });
        prop("Top edge",[](auto& a)->float&{return a.areaTopLeft.y;
        });
        prop("Right edge",[](auto& a)->float&{return a.areaBottomRight.x;
        });
        prop("Bottom edge",[](auto& a)->float&{return a.areaBottomRight.y;
        });
        label("HOLE MANAGEMENT");
        btn("Move hole earlier",[this]{if(doc.hole>0){doc.edit([&]{std::swap(doc.course.holes[doc.hole],doc.course.holes[doc.hole-1]);
                });
                --doc.hole;
                fit();
            }});
        btn("Move hole later",[this]{if(doc.hole+1<9){doc.edit([&]{std::swap(doc.course.holes[doc.hole],doc.course.holes[doc.hole+1]);
                });
                ++doc.hole;
                fit();
            }});
        btn("Copy hole into next slot",[this]{if(doc.hole==8){status="Select a hole before the last slot.";
                return;
            }
            doc.edit([&]{auto copy=doc.current();
                copy.id=newId("hole");
                copy.name+=" copy";
                doc.course.holes[doc.hole+1]=copy;
            });
            status="Copied into hole "+std::to_string(doc.hole+2)+". Undo restores its previous layout.";
        });
        btn("Clear this hole",[this]{doc.edit([&]{auto blank=blankCourse().holes[0];
                blank.id=doc.current().id;
                blank.name=doc.current().name;
                doc.current()=blank;
            });
            fit();
        });
        return;
    }
    Object o=doc.selection.front();
    const auto pos=doc.position(o);
    label(std::string(names[int(o.kind)])+(doc.selection.size()>1?" (group / selection)":""));
    input("Position X",pos.x,[this,o](float n){doc.translate({n-doc.position(o).x,0});
    });
    input("Position Y",pos.y,[this,o](float n){doc.translate({0,n-doc.position(o).y});
    });
    if(o.kind==Kind::Wall || o.kind==Kind::Surface || o.kind==Kind::Laser)
        input("Angle (degrees)",doc.angle(o),[this,o](float n){doc.rotate(n-doc.angle(o));});
    if(o.kind==Kind::Wall){
        prop("Width",[o](auto& a)->float&{return a.walls.at(o.index).width;
        });
        prop("Height",[o](auto& a)->float&{return a.walls.at(o.index).height;
        });
        btn("Group selected walls",[this]{doc.edit([&]{auto group=newId("barrier");
                int r=doc.rail(doc.selection.front());
                bool first=true;
                for(auto s:doc.selection)if(s.kind==Kind::Wall){auto& wall=doc.current().walls[s.index];
                    wall.group=group;
                    wall.rail=r;
                    wall.showRail=first;
                    first=false;
                }});
        });
        btn("Ungroup walls",[this]{doc.edit([&]{for(auto s:doc.selection)if(s.kind==Kind::Wall){doc.current().walls[s.index].group.clear();
                    doc.current().walls[s.index].showRail=true;
                }});
        });
    }
    if(o.kind==Kind::Surface){prop("Width",[o](auto& a)->float&{return a.surfaces.at(o.index).size.x;
        });
        prop("Height",[o](auto& a)->float&{return a.surfaces.at(o.index).size.y;
        });
        const char* kinds[]={"Rough","Sand","Ice","Water"};
        btn(std::string("Surface: ")+kinds[int(h.surfaces[o.index].kind)],[this,o]{doc.edit([&]{auto& k=doc.current().surfaces[o.index].kind;
                k=SurfaceKind((int(k)+1)%4);
            });
        });
    }
    if(o.kind==Kind::Cup || o.kind==Kind::Portal)prop("Cup/portal radius",[](auto& a)->float&{return a.cupRadius;
    });
    if(o.kind==Kind::Bumper){prop("Radius",[o](auto& a)->float&{return a.bumpers.at(o.index).radius;
        });
        prop("Speed boost",[o](auto& a)->float&{return a.bumpers.at(o.index).kickSpeed;
        });
    }
    if(o.kind==Kind::Laser){prop("Length",[o](auto& a)->float&{auto& s=a.lasers.at(o.index).size;return s.x>=s.y?s.x:s.y;
        });
        prop("Beam thickness",[o](auto& a)->float&{auto& s=a.lasers.at(o.index).size;return s.x>=s.y?s.y:s.x;
        });
        prop("On seconds",[o](auto& a)->float&{return a.lasers.at(o.index).onSeconds;
        });
        prop("Off seconds",[o](auto& a)->float&{return a.lasers.at(o.index).offSeconds;
        });
        prop("Laser phase",[o](auto& a)->float&{return a.lasers.at(o.index).phaseSeconds;
        });
    }
    if(o.kind==Kind::Portal){auto color=h.portals[o.index].color;
        int pair=h.portals[o.index].pair;
        for(int channel=0;
        channel<3;
        ++channel)input(channel==0?"Red":channel==1?"Green":"Blue",channel==0?color.r:channel==1?color.g:color.b,[this,pair,channel](float n){if(n<0 || n>255 || n!=std::floor(n))throw std::runtime_error("Color must be an integer from 0 to 255");
            for(auto& p:doc.current().portals)if(p.pair==pair){if(channel==0)p.color.r=uint8_t(n);
                else if(channel==1)p.color.g=uint8_t(n);
                else p.color.b=uint8_t(n);
            }});
    }
    if(o.kind==Kind::Tee || o.kind==Kind::ExtraTee || o.kind==Kind::Surface)return;
    label("MOVEMENT RAIL");
    int r=doc.rail(o);
    btn(r<0?"Attach existing rail...":"Attached: rail "+std::to_string(r+1)+" (change...)",[this]{choosingRail=!choosingRail;
    });
    if(choosingRail){
        if(h.rails.empty())label("No existing rails. Create one below.");
        for(size_t i=0;i<h.rails.size();++i)btn("Attach rail "+std::to_string(i+1)+" ("+std::to_string(h.rails[i].stops.size())+" points)",[this,i]{
            doc.edit([&]{for(auto s:doc.selection)if(movable(s))doc.attach(s,int(i));});
            pointIndex=0;choosingRail=false;addingWaypoints=false;hideRails=false;
            status="Rail attached. Its offsets apply relative to each object's original position.";
        });
    }
    if(r>=0)btn("Detach from rail",[this]{
        doc.edit([&]{for(auto s:doc.selection)doc.attach(s,-1);});
        pointIndex=0;addingWaypoints=false;
    });
    btn("Create and attach new rail",[this]{doc.edit([&]{Rail rail;
            rail.id=newId("rail");
            rail.stops={{{0,0},0,100,newId("stop")},{{160,0},0,100,newId("stop")}};
            doc.current().rails.push_back(rail);
            int i=int(doc.current().rails.size()-1);
            for(auto s:doc.selection)doc.attach(s,i);
        });
        pointIndex=0;
        tool=12;addingWaypoints=false;hideRails=false;animated=false;choosingRail=false;
        status="Rail created. Drag numbered handles; enable Add points to extend it.";
    });
    if(r<0)return;
    btn(addingWaypoints && tool==12?"Finish adding points":"Add points on canvas",[this]{
        addingWaypoints=!(addingWaypoints && tool==12);
        tool=12;
        hideRails=false;
        animated=false;
        preview.seek(0);
        status=addingWaypoints?"Click empty space to add points. Drag handles to move them. Right-click to finish.":"Drag handles to move waypoints; click a path or object to select it.";
    });
    auto& rail=h.rails.at(r);
    pointIndex=std::clamp(pointIndex,0,int(rail.stops.size())-1);
    int point=pointIndex;
    btn(rail.mode==RailMode::Loop?"Mode: loop":"Mode: back and forth",[this,r]{doc.edit([&]{auto& mode=doc.current().rails[r].mode;
            mode=mode==RailMode::Loop?RailMode::PingPong:RailMode::Loop;
        });
    });
    prop("Rail phase",[r](auto& a)->float&{return a.rails.at(r).phaseSeconds;
    });
    btn("Waypoint "+std::to_string(point+1)+" / "+std::to_string(rail.stops.size())+" (next)",[this,r]{pointIndex=(pointIndex+1)%int(doc.current().rails[r].stops.size());
    });
    prop("Offset X",[r,point](auto& a)->float&{return a.rails.at(r).stops.at(point).offset.x;
    });
    prop("Offset Y",[r,point](auto& a)->float&{return a.rails.at(r).stops.at(point).offset.y;
    });
    prop("Pause seconds",[r,point](auto& a)->float&{return a.rails.at(r).stops.at(point).pauseSeconds;
    });
    prop("Leaving speed",[r,point](auto& a)->float&{return a.rails.at(r).stops.at(point).speed;
    });
    btn("Remove this waypoint",[this,r,point]{if(doc.current().rails[r].stops.size()<=2){status="A rail needs at least two waypoints.";
            return;
        }
        doc.edit([&]{auto& v=doc.current().rails[r].stops;
            v.erase(v.begin()+point);
        });
        pointIndex=0;
    });
    btn("Delete rail / detach all users",[this,r]{doc.edit([&]{doc.removeRail(r);
        });
    });
}
void Editor::paint(){
    widgets.clear();
    int w,h;
    SDL_GetWindowSize(window,&w,&h);
    SDL_SetRenderTarget(renderer,nullptr);
    SDL_SetRenderClipRect(renderer,nullptr);
    SDL_SetRenderDrawColor(renderer,bg.r,bg.g,bg.b,255);
    SDL_RenderClear(renderer);
    float availableW=float(w-510),availableH=float(h-205);
    float scale=std::min(availableW/COURSE_VIEW_W,availableH/COURSE_VIEW_H);
    canvas={190+(availableW-COURSE_VIEW_W*scale)/2,80+(availableH-COURSE_VIEW_H*scale)/2,COURSE_VIEW_W*scale,COURSE_VIEW_H*scale};
    preview.view(center,zoom);
    SDL_SetRenderTarget(renderer,scene);
    renderQueueClearFrame(frame,22,32,28);
    preview.draw(!hideSurfaces || testing,!hideRails || testing);
    renderQueueDrawFlush(frame);
    SDL_SetRenderTarget(renderer,nullptr);
    SDL_FRect source{COURSE_VIEW_X,COURSE_VIEW_Y,COURSE_VIEW_W,COURSE_VIEW_H};
    SDL_RenderTexture(renderer,scene,&source,&canvas);
    SDL_Rect clip{int(canvas.x),int(canvas.y),int(canvas.w),int(canvas.h)};
    SDL_SetRenderClipRect(renderer,&clip);
    if(!testing){
        if(grid && zoom*scale>0.25f){SDL_SetRenderDrawColor(renderer,120,160,130,45);
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
            auto min=world(canvas.x,canvas.y),max=world(canvas.x+canvas.w,canvas.y+canvas.h);
            for(float x=std::ceil(min.x/40)*40;
            x<max.x;
            x+=40)for(float y=std::ceil(min.y/40)*40;
            y<max.y;
            y+=40){auto p=screen({x,y});
                SDL_RenderPoint(renderer,p.x,p.y);
            }
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
        }
        auto tee=[&](Vec2 p,const std::string& name){auto s=screen(p);
            box({s.x-8,s.y-8,16,16},{220,245,220,255},false);
            text(s.x+11,s.y-10,name);
        };
        tee(doc.current().startPos,"Tee");
        for(size_t i=0;
        i<doc.current().spawnPositions.size();
        ++i)tee(doc.current().spawnPositions[i],"T"+std::to_string(i+2));
        for(auto o:doc.selection){
            auto c=displayedPosition(o),size=doc.extent(o);
            SDL_FPoint corners[5];
            for(int i=0;i<4;++i){
                auto d=rotateVector({(i==0 || i==3?-1.0f:1.0f)*size.x/2,(i<2?-1.0f:1.0f)*size.y/2},doc.angle(o));
                auto p=screen({c.x+d.x,c.y+d.y});corners[i]={p.x,p.y};
            }
            corners[4]=corners[0];SDL_SetRenderDrawColor(renderer,255,220,90,255);SDL_RenderLines(renderer,corners,5);
        }
        if(!doc.selection.empty() && tool==0){
            auto o=doc.selection.front();auto c=displayedPosition(o),size=doc.extent(o);
            auto point=[&](Vec2 d){auto r=rotateVector(d,doc.angle(o));return screen({c.x+r.x,c.y+r.y});};
            if(o.kind==Kind::Wall || o.kind==Kind::Surface || o.kind==Kind::Laser){
                auto stem=point({0,-size.y/2}),handle=point({0,-size.y/2-28/(scale*zoom)});
                SDL_SetRenderDrawColor(renderer,255,220,90,255);SDL_RenderLine(renderer,stem.x,stem.y,handle.x,handle.y);
                box({handle.x-6,handle.y-6,12,12},{255,220,90,255},false);
                text(handle.x+10,handle.y-10,"Rotate");
                if(o.kind==Kind::Laser){
                    for(int end=0;end<2;++end){auto p=screen(laserEnd(doc.current().lasers[o.index],c,end));box({p.x-6,p.y-6,12,12},accent);}
                }else if(doc.selection.size()==1){auto p=point({size.x/2,size.y/2});box({p.x-5,p.y-5,10,10},accent);}
            }
        }
        if(!hideRails)for(auto o:objects()){
            bool selected=std::find(doc.selection.begin(),doc.selection.end(),o)!=doc.selection.end();
            if(tool!=12 && !selected)continue;
            int r=doc.rail(o);
            if(r>=0){auto base=doc.position(o);
                const auto& rail=doc.current().rails[r];
                for(size_t i=0;
                i<rail.stops.size();
                ++i){auto p=screen({base.x+rail.stops[i].offset.x,base.y+rail.stops[i].offset.y});
                    box({p.x-7,p.y-7,14,14},selected && int(i)==pointIndex?SDL_Color{255,210,80,255}:accent,selected);
                    text(p.x+10,p.y-13,std::to_string(i+1),selected?SDL_Color{255,220,90,255}:muted);
                }}}
        if(firstPortal){auto p=screen(*firstPortal);
            box({p.x-15,p.y-15,30,30},accent,false);
        }
        if(dragging && tool>=2 && tool<=6){auto a=screen(dragStart),b=screen(last);
            if(tool==6){SDL_SetRenderDrawColor(renderer,245,70,80,255);SDL_RenderLine(renderer,a.x,a.y,b.x,b.y);
                box({a.x-5,a.y-5,10,10},accent);box({b.x-5,b.y-5,10,10},accent);
            }else box({std::min(a.x,b.x),std::min(a.y,b.y),std::abs(a.x-b.x),std::abs(a.y-b.y)},accent,false);
        }
    }
    if(shooting){auto p=screen(preview.ballPosition());
        SDL_SetRenderDrawColor(renderer,255,245,160,255);
        SDL_RenderLine(renderer,p.x,p.y,p.x+(shotStart.x-world(mouseX,mouseY).x)*zoom*scale,p.y+(shotStart.y-world(mouseX,mouseY).y)*zoom*scale);
    }
    SDL_SetRenderClipRect(renderer,nullptr);
    box({0,0,float(w),64},panel);
    box({0,64,178,float(h-64)},panel);
    box({float(w-316),64,316,float(h-64)},panel);
    button({12,16,62,32},"New",[this]{if(discardAllowed()){doc.create();
            testing=false;
            fit();
            resetPreview();
        }});
    button({80,16,70,32},"Open",[this]{fileDialog(0);
    });
    button({156,16,64,32},"Save",[this]{save();
    });
    button({226,16,83,32},"Save As",[this]{save(true);
    });
    button({315,16,67,32},"Undo",[this]{doc.undo();
        resetPreview();
    },false,!doc.undoStack.empty() && !testing);
    button({388,16,67,32},"Redo",[this]{doc.redo();
        resetPreview();
    },false,!doc.redoStack.empty() && !testing);
    button({467,16,110,32},testing?"Stop test":"Playtest",[this]{testing=!testing;
        firstPortal.reset();
        resetPreview();
        status=testing?"Drag backward and release to putt. Right-click places the ball.":"Editing resumed; test movement was not saved.";
    },testing);
    button({583,16,78,32},testing?"Reset":"Fit",[this]{if(testing)preview.resetBall(doc.current().startPos);
        else fit();
    });
    button({667,16,91,32},"Recover",[this]{recovery();
    });
    button({764,16,97,32},"Validate",[this]{validateCourse(doc.course);
        status="File validation passed. Playtest to check paths and obstacle clearance.";
    });
    text(16,80,"PLACE OBJECTS",accent);
    const char* tools[]={"Select / move","Paint wall tiles","Sand rectangle","Ice rectangle","Rough rectangle","Water rectangle","Laser beam","Bumper","Portal pair","Primary tee","Cup","Extra tee","Rail / waypoints"};
    for(int i=0;
    i<13;
    ++i)button({12,float(110+i*31),154,27},tools[i],[this,i]{tool=i;
        addingWaypoints=false;
        firstPortal.reset();
        animated=false;
        preview.seek(0);
        if(i==12){hideRails=false;
            status="Click an object or path to select it. Drag numbered handles. Add points using the inspector.";
        }
    },tool==i,!testing);
    button({12,526,154,27},grid?"Grid / snap: on":"Grid / snap: off",[this]{grid=!grid;
    },grid);
    button({12,560,154,27},lockedSurfaces?"Surfaces: locked":"Surfaces: unlocked",[this]{lockedSurfaces=!lockedSurfaces;
    },lockedSurfaces);
    button({12,594,154,27},hideSurfaces?"Show surfaces":"Hide surfaces",[this]{hideSurfaces=!hideSurfaces;
    });
    button({12,628,154,27},hideRails?"Show rails":"Hide rails",[this]{hideRails=!hideRails;
    });
    inspectorScroll=std::clamp(inspectorScroll,0.0f,std::max(0.0f,inspectorBottom-h+55));
    inspector();
    const float bottom=float(h-106);
    button({190,bottom,100,29},animated?"Pause motion":"Run motion",[this]{animated=!animated;
    },animated,!testing);
    button({296,bottom,61,29},"0 sec",[this]{animated=false;
        preview.seek(0);
    },false,!testing);
    button({363,bottom,74,29},"+0.5 sec",[this]{animated=false;
        preview.seek(preview.time()+0.5f);
    },false,!testing);
    text(448,bottom+5,numeric(preview.time())+" s",muted);
    const float hw=std::min(74.0f,float(w-520)/9);
    for(size_t i=0;
    i<9;
    ++i)button({190+i*(hw+3),float(h-65),hw,29},"Hole "+std::to_string(i+1),[this,i]{doc.hole=i;
        doc.selection.clear();
        firstPortal.reset();
        inspectorScroll=0;
        fit();
        resetPreview();
    },doc.hole==i);
    SDL_Rect statusClip{190,h-28,w-510,25};
    SDL_SetRenderClipRect(renderer,&statusClip);
    text(190,float(h-26),status,muted);
    SDL_SetRenderClipRect(renderer,nullptr);
    text(float(w-300),float(h-30),"Scroll panel for more properties",muted);
}
void Editor::event(const SDL_Event& e){
    if(e.type==SDL_EVENT_QUIT || e.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED){commit();
        if(!dialogOpen && discardAllowed())running=false;
        return;
    }
    if(e.type==SDL_EVENT_MOUSE_MOTION){mouseX=e.motion.x;
        mouseY=e.motion.y;
        action([&]{canvasMove(mouseX,mouseY);
        });
        return;
    }
    if(e.type==SDL_EVENT_MOUSE_WHEEL){int w,h;
        SDL_GetWindowSize(window,&w,&h);
        (void)h;
        if(mouseX>w-316)inspectorScroll=std::clamp(inspectorScroll-e.wheel.y*40,0.0f,std::max(0.0f,inspectorBottom-h+55));
        else if(inside(canvas,mouseX,mouseY)){zoom=std::clamp(zoom*std::pow(1.12f,e.wheel.y),0.1f,8.0f);
        }
        return;
    }
    if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){mouseX=e.button.x;
        mouseY=e.button.y;
        commit();
        for(const auto& widget:widgets)if(inside(widget.rect,mouseX,mouseY)){if(widget.setter){focus=widget.key;
                focusSetter=widget.setter;
                buffer=widget.value;
                selectAll=true;
                SDL_StartTextInput(window);
            }
            else if(widget.action)action(widget.action);
            return;
        }
        action([&]{canvasDown(e.button);
        });
        return;
    }
    if(e.type==SDL_EVENT_MOUSE_BUTTON_UP){mouseX=e.button.x;
        mouseY=e.button.y;
        action([&]{canvasUp();
        });
        return;
    }
    if(e.type==SDL_EVENT_TEXT_INPUT && !focus.empty()){if(selectAll){buffer.clear();
            selectAll=false;
        }
        buffer+=e.text.text;
        return;
    }
    if(e.type!=SDL_EVENT_KEY_DOWN)return;
    auto key=e.key.key;
    const bool ctrl=(e.key.mod&SDL_KMOD_CTRL)!=0;
    if(!focus.empty()){
        if(key==SDLK_RETURN || key==SDLK_KP_ENTER)commit();
        else if(key==SDLK_ESCAPE){focus.clear();
            SDL_StopTextInput(window);
        }
        else if(key==SDLK_A && ctrl)selectAll=true;
        else if(key==SDLK_BACKSPACE){if(selectAll)buffer.clear();
            else if(!buffer.empty()){
                size_t start=buffer.size()-1;
                while(start>0 && (static_cast<unsigned char>(buffer[start])&0xc0)==0x80) --start;
                buffer.resize(start);
            }
            selectAll=false;
        }
        return;
    }
    action([&]{
        if(ctrl && key==SDLK_S)save((e.key.mod&SDL_KMOD_SHIFT)!=0);
        else if(ctrl && key==SDLK_O)fileDialog(0);
        else if(ctrl && key==SDLK_Z && !testing){doc.undo();
            resetPreview();
        }
        else if(ctrl && key==SDLK_Y && !testing){doc.redo();
            resetPreview();
        }
        else if(ctrl && key==SDLK_D && !testing){doc.duplicate();
            resetPreview();
        }
        else if(key==SDLK_DELETE && !testing && !doc.selection.empty()){doc.erase();
            resetPreview();
        }
        else if(key==SDLK_ESCAPE){canvasUp();
            firstPortal.reset();
            tool=0;
            doc.selection.clear();
            inspectorScroll=0;
            if(testing){testing=false;
                resetPreview();
            }}
        else if(key==SDLK_F)fit();
        else if(key==SDLK_SPACE && !testing)animated=!animated;
        else if((key==SDLK_LEFT || key==SDLK_RIGHT || key==SDLK_UP || key==SDLK_DOWN) && !testing && !doc.selection.empty()){float step=grid?20:1;
            doc.edit([&]{doc.translate({key==SDLK_LEFT?-step:key==SDLK_RIGHT?step:0,key==SDLK_UP?-step:key==SDLK_DOWN?step:0});
            });
            resetPreview();
        }
    });
}
int Editor::run(const std::string& smokeOutput){
    uint64_t previous=SDL_GetTicks();
    paint();
    SDL_RenderPresent(renderer);
    if(!smokeOutput.empty()) {
        auto check=[](bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);
        };
        auto buttonEvent=[&](Uint32 type,Vec2 p){SDL_Event e{};
            e.type=type;
            e.button.button=SDL_BUTTON_LEFT;
            e.button.x=p.x;
            e.button.y=p.y;
            event(e);
            paint();
        };
        auto click=[&](Vec2 p){buttonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN,p);
            buttonEvent(SDL_EVENT_MOUSE_BUTTON_UP,p);
        };
        auto drag=[&](Vec2 from,Vec2 to){buttonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN,from);
            SDL_Event e{};
            e.type=SDL_EVENT_MOUSE_MOTION;
            e.motion.x=to.x;
            e.motion.y=to.y;
            event(e);
            paint();
            buttonEvent(SDL_EVENT_MOUSE_BUTTON_UP,to);
        };
        auto undo=[&]{SDL_Event e{};
            e.type=SDL_EVENT_KEY_DOWN;
            e.key.key=SDLK_Z;
            e.key.mod=SDL_KMOD_CTRL;
            event(e);
            paint();
        };
        auto original=doc;
        doc.create();
        fit();
        resetPreview();
        paint();
        click({80,309});
        drag(screen({400,280}),screen({640,400}));
        check(doc.current().lasers.size()==1,"Laser beam placement failed");
        check(doc.current().lasers[0].size.y==8 && doc.current().lasers[0].angleDegrees>20,"Laser drag created a rectangle instead of an angled beam");
        click({80,123});
        auto laser=doc.current().lasers[0];
        auto endpoint=laserEnd(laser,laser.center,1);
        drag(screen(endpoint),screen({640,280}));
        check(std::abs(doc.current().lasers[0].angleDegrees)<0.01f,"Laser endpoint drag did not aim the beam");
        auto fixed=laserEnd(doc.current().lasers[0],doc.current().lasers[0].center,0);
        check(std::hypot(fixed.x-400,fixed.y-280)<1,"Laser endpoint drag moved the opposite end");
        undo();undo();
        check(doc.current().lasers.empty(),"Laser placement was not one undo step");
        click({80,185});
        drag(screen({400,400}),screen({660,520}));
        check(doc.current().surfaces.size()==1,"Mouse surface placement failed");
        click({80,123});
        click(screen(doc.current().surfaces[0].center));
        auto patch=doc.current().surfaces[0];
        Vec2 corner{patch.center.x+patch.size.x/2,patch.center.y+patch.size.y/2};
        drag(screen(corner),screen({corner.x+80,corner.y+40}));
        check(doc.current().surfaces[0].size.x>patch.size.x,"Mouse resize failed");
        undo();
        check(doc.current().surfaces[0].size.x==patch.size.x,"Resize undo failed");
        click(screen(patch.center));
        float handleDistance=patch.size.y/2+28/(zoom*canvas.w/COURSE_VIEW_W);
        auto angledHandle=rotateVector({0,-handleDistance},45);
        drag(screen({patch.center.x,patch.center.y-handleDistance}),screen({patch.center.x+angledHandle.x,patch.center.y+angledHandle.y}));
        check(std::abs(doc.current().surfaces[0].angleDegrees-45)<0.01f,"Rotation handle did not rotate the surface");
        auto size=doc.current().surfaces[0].size;
        auto rotatedCorner=rotateVector({size.x/2,size.y/2},45);
        auto enlargedCorner=rotateVector({size.x/2+80,size.y/2+40},45);
        bool oldGrid=grid;grid=false;
        drag(screen({patch.center.x+rotatedCorner.x,patch.center.y+rotatedCorner.y}),screen({patch.center.x+enlargedCorner.x,patch.center.y+enlargedCorner.y}));
        grid=oldGrid;
        check(std::abs(doc.current().surfaces[0].size.x-size.x-80)<0.1f,"Rotated surface resize did not follow its local axes");
        undo();undo();
        click({80,155});
        drag(screen({200,600}),screen({500,600}));
        check(doc.current().walls.size()>=7,"Continuous tile painting left gaps");
        undo();
        check(doc.current().walls.empty(),"Wall stroke was not a single undo step");
        click({80,371});
        click(screen({850,400}));
        click(screen({1100,400}));
        check(doc.current().portals.size()==2,"Portal pair placement failed");
        doc.selection.clear();
        hideRails=true;
        click({80,495});
        check(tool==12 && !hideRails,"Rail tool did not reveal paths");
        click(screen({850,400}));
        check(doc.selection.size()==1 && doc.current().rails.empty(),"Rail tool did not select an object first");
        click(screen({850,560}));
        check(doc.current().rails.size()==1 && doc.rail(doc.selection.front())==0,"Rail tool did not create and attach a path");
        check(doc.current().rails[0].stops.size()==2,"New rail must start at the object and end at the click");
        auto offset=doc.current().rails[0].stops[1].offset;
        auto origin=doc.position(doc.selection.front());
        auto target=snap({850,560});
        check(std::hypot(origin.x+offset.x-target.x,origin.y+offset.y-target.y)<1,"Rail waypoint offset is wrong");
        click(screen({1000,560}));
        check(doc.current().rails[0].stops.size()==3,"Rail tool did not extend the existing path");
        drag(screen(target),screen({target.x+60,target.y+60}));
        check(doc.current().rails[0].stops.size()==3,"Dragging a handle accidentally appended a waypoint");
        check(std::abs(doc.current().rails[0].stops[1].offset.x-offset.x-60)<1,"Rail tool did not drag a waypoint");
        undo();
        doc.selection.clear();
        click({80,495});
        auto nearHandle=screen(target);nearHandle.x+=10;
        click(nearHandle);
        check(doc.selection.size()==1 && pointIndex==1,"Visible waypoint was not clickable without selecting its owner");
        float originalZoom=zoom;
        for(float factor:{0.6f,1.4f}){
            zoom=originalZoom*factor;paint();doc.selection.clear();
            auto handle=screen(target);handle.x+=10;
            click(handle);
            check(doc.selection.size()==1 && pointIndex==1,"Waypoint hit area changed with zoom");
        }
        zoom=originalZoom;paint();
        click(screen({1200,700}));
        check(doc.current().rails[0].stops.size()==3,"Rail editing unexpectedly added a point");
        doc.selection.clear();
        click(screen({(origin.x+target.x)/2,(origin.y+target.y)/2}));
        check(doc.selection.size()==1,"Clicking a rail segment did not select the rail");
        click({80,123});
        drag(screen(origin),screen({origin.x+40,origin.y}));
        check(std::abs(doc.current().portals[0].center.x-origin.x-40)<1,"First waypoint intercepted object movement");
        check(doc.current().rails[0].stops[0].offset.x==0,"Moving the object changed its rail's first offset");
        undo();
        // Selection and movement follow the sampled rail position, not the base.
        preview.seek(0.6f);paint();
        auto moving=displayedPosition({Kind::Portal,0});
        drag(screen(moving),screen({moving.x+40,moving.y}));
        check(std::abs(doc.current().portals[0].center.x-origin.x-40)<1,"Rail-offset object could not be dragged where it was drawn");
        check(std::abs(preview.time()-0.6f)<0.001f,"Dragging reset the preview timeline");
        undo();preview.seek(0);paint();
        click(screen(doc.current().portals[1].center));
        auto inspectorButton=[&](const std::string& caption){
            inspectorScroll=0;
            for(int attempt=0;attempt<25;++attempt){
                paint();
                auto it=std::find_if(widgets.begin(),widgets.end(),[&](const auto& w){return w.action && w.value==caption;});
                if(it!=widgets.end()){auto rect=it->rect;click({rect.x+20,rect.y+14});return;}
                inspectorScroll+=120;
            }
            throw std::runtime_error("Missing inspector button: "+caption);
        };
        inspectorButton("Attach existing rail...");
        inspectorButton("Attach rail 1 (3 points)");
        check(doc.current().portals[1].rail==0,"Explicit rail attachment failed");
        inspectorButton("Detach from rail");
        check(doc.current().portals[1].rail==-1 && doc.current().portals[0].rail==0,"Detach changed another rail user");
        undo();
        undo();
        undo();
        check(doc.current().rails[0].stops.size()==2,"Waypoint undo failed");
        undo();
        check(doc.current().rails.empty() && doc.current().portals[0].rail==-1,"Rail creation undo did not detach the object");
        doc.selection.clear();
        click({80,495});
        click(screen(doc.current().surfaces[0].center));
        check(doc.current().rails.empty() && doc.selection.empty(),"Rail tool allowed a surface to move");
        doc.save(std::filesystem::path(smokeOutput).parent_path()/"smoke-course");
        check(loadCourse(doc.saveDirectory).holes[0].portals.size()==2,"Edited course did not save");
        doc=std::move(original);
        tool=0;
        doc.hole=2;
        doc.select({Kind::Wall,0});
        fit();
        resetPreview();
        paint();
        SDL_SetWindowSize(window,1100,720);paint();
        int windowWidth=0,windowHeight=0;SDL_GetWindowSize(window,&windowWidth,&windowHeight);
        SDL_Event scroll{};scroll.type=SDL_EVENT_MOUSE_MOTION;scroll.motion.x=float(windowWidth-100);scroll.motion.y=200;event(scroll);
        scroll.type=SDL_EVENT_MOUSE_WHEEL;scroll.wheel.y=-100;event(scroll);paint();
        check(std::any_of(widgets.begin(),widgets.end(),[&](const auto& widget){return widget.rect.x>windowWidth-310 && bool(widget.setter);}),"Inspector scrolled beyond its content");
        SDL_SetWindowSize(window,1440,900);
        scroll.wheel.y=100;event(scroll);paint();
        auto* image=SDL_RenderReadPixels(renderer,nullptr);
        if(!image || !SDL_SaveBMP(image,smokeOutput.c_str()))throw std::runtime_error(SDL_GetError());
        SDL_DestroySurface(image);
        click({510,30});
        check(testing && preview.ready(),"Playtest button failed");
        auto start=preview.ballPosition();
        auto pixel=screen(start);
        drag(pixel,{pixel.x+70,pixel.y+80});
        for(int i=0;
        i<120;
        ++i)preview.advance(1.0f/120);
        auto end=preview.ballPosition();
        check(std::hypot(end.x-start.x,end.y-start.y)>1,"Mouse putt did not move the ball");
        return 0;
    }
    while(running){
        SDL_Event e;
        while(SDL_PollEvent(&e)){event(e);
            paint();
        }
        {std::optional<std::pair<int,std::string>> result;
            {std::lock_guard lock(dialogMutex);
                result=std::move(dialogResult);
                dialogResult.reset();
            }
            if(result){dialogOpen=false;
                if(!result->second.empty())action([&]{if(result->first==0)open(std::filesystem::path(result->second).parent_path());
                    else{auto target=std::filesystem::path(result->second);
                        if(!confirmReplace(target))return;
                        doc.save(target);
                        status="Saved: "+target.string();
                    }});
            }
        }
        uint64_t now=SDL_GetTicks();
        float dt=std::min(0.1f,float(now-previous)/1000);
        previous=now;
        if(testing || animated)preview.advance(dt);
        autosaveTimer+=dt;
        if(autosaveTimer>30){autosaveTimer=0;
            if(doc.dirty())action([&]{saveCourse(doc.course,appDataPath("course-editor/recovery"));
                status="Autosaved recovery copy.";
            });
        }
        paint();
        SDL_RenderPresent(renderer);
        SDL_Delay(8);
    }
    return 0;
}
}
