#include "document.hpp"
#include "course_motion.hpp"
#include "debug/app_paths.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <map>
#include <stdexcept>
namespace GolfEditor {
std::string newId(const std::string& prefix) {
    static uint64_t counter=static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    return prefix+"-"+std::to_string(++counter);
}
Course blankCourse() {
    Course c;
    c.id=newId("course");
    c.name="Untitled course";
    for(size_t i=0;
    i<c.holes.size();
    ++i) {
        auto& h=c.holes[i];
        h.id=newId("hole");
        h.name="Hole "+std::to_string(i+1);
        h.areaTopLeft={65,145};
        h.areaBottomRight={1345,865};
        h.startPos={705,805};
        h.cupPos={705,205};
        h.cupRadius=30;
    }
    assignCourseIds(c);
    return c;
}
void Document::checkpoint() {
    undoStack.push_back({course,hole,revision});
    if(undoStack.size()>100) undoStack.erase(undoStack.begin());
    redoStack.clear();
    revision=nextRevision++;
}
void Document::edit(const std::function<void()>& action) {
    auto old=course;
    try { action();
        assignCourseIds(course);
        validateCourse(course);
    }
    catch(...) { course=std::move(old);
        throw;
    }
    auto changed=std::move(course);
    course=std::move(old);
    checkpoint();
    course=std::move(changed);
}
void Document::undo() {
    if(undoStack.empty()) return;
    redoStack.push_back({course,hole,revision});
    auto s=std::move(undoStack.back());
    undoStack.pop_back();
    course=std::move(s.course);
    hole=s.hole;
    revision=s.revision;
    selection.clear();
}
void Document::redo() {
    if(redoStack.empty()) return;
    undoStack.push_back({course,hole,revision});
    auto s=std::move(redoStack.back());
    redoStack.pop_back();
    course=std::move(s.course);
    hole=s.hole;
    revision=s.revision;
    selection.clear();
}
void Document::load(const std::filesystem::path& path) {
    auto loaded=loadCourse(path);
    course=std::move(loaded);
    hole=0;
    selection.clear();
    undoStack.clear();
    redoStack.clear();
    revision=savedRevision=0;
    nextRevision=1;
    // A bundled folder is never the implicit save destination.
    const auto bundled=std::filesystem::weakly_canonical(appAssetPath("assets/games/minigolf/courses"));
    const auto canonical=std::filesystem::weakly_canonical(path);
    sourceDirectory=canonical;
    const auto relative=canonical.lexically_relative(bundled);
    const bool isBundled=!relative.empty() && *relative.begin()!="..";
    saveDirectory=isBundled ? std::filesystem::path(appDataPath("courses"))/path.filename() : canonical;
}
void Document::save(const std::filesystem::path& path) {saveCourse(course,path);
    saveDirectory=path;
    savedRevision=revision;
    sourceDirectory=std::filesystem::weakly_canonical(path);
}
void Document::create() {sourceDirectory.clear();
    course=blankCourse();
    hole=0;
    selection.clear();
    undoStack.clear();
    redoStack.clear();
    savedRevision=0;
    revision=1;
    nextRevision=2;
    saveDirectory=std::filesystem::path(appDataPath("courses"))/course.id;
}
Vec2 Document::position(Object o) const {
    const auto& h=current();
    switch(o.kind) {
        case Kind::Tee:return h.startPos;
        case Kind::ExtraTee:return h.spawnPositions.at(o.index);
        case Kind::Cup:return h.cupPos;
        case Kind::Wall:{const auto& b=h.walls.at(o.index);
            return {b.centerX,b.centerY};
        }
        case Kind::Surface:return h.surfaces.at(o.index).center;
        case Kind::Bumper:return h.bumpers.at(o.index).center;
        case Kind::Laser:return h.lasers.at(o.index).center;
        case Kind::Portal:return h.portals.at(o.index).center;
    } return {};
}
Vec2 Document::extent(Object o) const {
    const auto& h=current();
    switch(o.kind) {
        case Kind::Wall:{const auto& b=h.walls.at(o.index);
            return {b.width,b.height};
        }
        case Kind::Surface:return h.surfaces.at(o.index).size;
        case Kind::Laser:return h.lasers.at(o.index).size;
        case Kind::Bumper:{float r=h.bumpers.at(o.index).radius;
            return {r*2,r*2};
        }
        case Kind::Portal:case Kind::Cup:return {h.cupRadius*2,h.cupRadius*2};
        default:return {32,32};
    }
}
int Document::rail(Object o) const {
    const auto& h=current();
    switch(o.kind) {case Kind::Cup:return h.cupRail;
        case Kind::Wall:return h.walls.at(o.index).rail;
        case Kind::Bumper:return h.bumpers.at(o.index).rail;
        case Kind::Laser:return h.lasers.at(o.index).rail;
        case Kind::Portal:return h.portals.at(o.index).rail;
        default:return -1;
    }
}
float Document::angle(Object o) const {
    const auto& h=current();
    if(o.kind==Kind::Wall)return h.walls.at(o.index).angleDegrees;
    if(o.kind==Kind::Surface)return h.surfaces.at(o.index).angleDegrees;
    if(o.kind==Kind::Laser)return h.lasers.at(o.index).angleDegrees;
    return 0;
}
void Document::rotate(float degrees) {
    if(selection.empty())return;
    const auto pivot=position(selection.front());
    for(auto o:selection){
        auto p=position(o),q=rotateVector({p.x-pivot.x,p.y-pivot.y},degrees);
        move(o,{pivot.x+q.x-p.x,pivot.y+q.y-p.y});
        float a=std::remainder(angle(o)+degrees,360.0f);
        if(o.kind==Kind::Wall)current().walls[o.index].angleDegrees=a;
        if(o.kind==Kind::Surface)current().surfaces[o.index].angleDegrees=a;
        if(o.kind==Kind::Laser)current().lasers[o.index].angleDegrees=a;
    }
}
void Document::attach(Object o,int r) {
    auto& h=current();
    switch(o.kind) {case Kind::Cup:h.cupRail=r;
        break;
        case Kind::Wall: {auto group=h.walls.at(o.index).group;
            h.walls[o.index].rail=r;
            if(!group.empty()) for(auto& w:h.walls) if(w.group==group) w.rail=r;
            break;
        } case Kind::Bumper:h.bumpers.at(o.index).rail=r;
        break;
        case Kind::Laser:h.lasers.at(o.index).rail=r;
        break;
        case Kind::Portal:h.portals.at(o.index).rail=r;
        break;
        default:break;
    }
}
void Document::move(Object o,Vec2 d) {
    auto& h=current();
    auto add=[&](Vec2& p){p.x+=d.x;
        p.y+=d.y;
    };
    switch(o.kind) {
        case Kind::Tee:add(h.startPos);
        break;
        case Kind::ExtraTee:add(h.spawnPositions.at(o.index));
        break;
        case Kind::Cup:add(h.cupPos);
        break;
        case Kind::Wall:h.walls.at(o.index).centerX+=d.x;
        h.walls.at(o.index).centerY+=d.y;
        break;
        case Kind::Surface:add(h.surfaces.at(o.index).center);
        break;
        case Kind::Bumper:add(h.bumpers.at(o.index).center);
        break;
        case Kind::Laser:add(h.lasers.at(o.index).center);
        break;
        case Kind::Portal:add(h.portals.at(o.index).center);
        break;
    }
}
void Document::select(Object o,bool additive) {
    if(!additive) selection.clear();
    auto append=[&](Object item){if(std::find(selection.begin(),selection.end(),item)==selection.end()) selection.push_back(item);
    };
    append(o);
    if(o.kind==Kind::Wall) {const auto& h=current();
        auto group=h.walls.at(o.index).group;
        if(!group.empty()) for(size_t i=0;
        i<h.walls.size();
        ++i) if(h.walls[i].group==group) append({Kind::Wall,i});
    }
}
void Document::translate(Vec2 d) {for(auto o:selection) move(o,d);
}
void Document::erase() {
    edit([&]{
        auto& h=current();
        auto selected=selection;
        for(auto o:selection) if(o.kind==Kind::Portal) for(size_t i=0;
        i<h.portals.size();
        ++i) if(h.portals[i].pair==h.portals[o.index].pair) selected.push_back({Kind::Portal,i});
        std::sort(selected.begin(),selected.end(),[](auto a,auto b){return a.kind==b.kind ? a.index>b.index : a.kind<b.kind;
        });
        selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
        for(auto o:selected) {auto erase=[&](auto& v){v.erase(v.begin()+o.index);
            };
            switch(o.kind) {
                case Kind::Wall:erase(h.walls);
                break;
                case Kind::Surface:erase(h.surfaces);
                break;
                case Kind::Bumper:erase(h.bumpers);
                break;
                case Kind::Laser:erase(h.lasers);
                break;
                case Kind::Portal:erase(h.portals);
                break;
                case Kind::ExtraTee:erase(h.spawnPositions);
                erase(h.spawnIds);
                break;
                default:break;
            }}
    });
    selection.clear();
}
void Document::duplicate() {
    auto chosen=selection;
    std::vector<Object> copies;
    edit([&]{auto& h=current();
        std::map<std::string,std::string> groups;
        std::set<int> pairs;
        for(auto o:chosen) {
            auto copy=[&](auto& v){auto item=v.at(o.index);
                item.id.clear();
                v.push_back(item);
                Object result{o.kind,v.size()-1};
                move(result,{40,40});
                copies.push_back(result);
            };
            switch(o.kind) {
                case Kind::Wall: {auto old=h.walls.at(o.index).group;
                    copy(h.walls);
                    if(!old.empty()){if(!groups.count(old)) groups[old]=newId("group");
                        h.walls.back().group=groups[old];
                    } break;
                }
                case Kind::Surface:copy(h.surfaces);
                break;
                case Kind::Bumper:copy(h.bumpers);
                break;
                case Kind::Laser:copy(h.lasers);
                break;
                case Kind::Portal: {int pair=h.portals.at(o.index).pair;
                    if(!pairs.insert(pair).second) break;
                    int next=0;
                    for(auto& p:h.portals) next=std::max(next,p.pair+1);
                    auto originals=h.portals;
                    for(auto p:originals) if(p.pair==pair){p.pair=next;
                        p.id.clear();
                        p.center.x+=40;
                        p.center.y+=40;
                        h.portals.push_back(p);
                        copies.push_back({Kind::Portal,h.portals.size()-1});
                    }
                    break;
                }
                case Kind::ExtraTee:h.spawnPositions.push_back({position(o).x+40,position(o).y+40});
                h.spawnIds.push_back("");
                copies.push_back({Kind::ExtraTee,h.spawnPositions.size()-1});
                break;
                default:break;
            }
        }
    });
    selection=copies;
}
void Document::add(Kind kind,Vec2 p,Vec2 size,SurfaceKind surface) {
    Object object{kind,0};
    edit([&]{auto& h=current();
        switch(kind){
            case Kind::Tee:h.startPos=p;
            break;
            case Kind::Cup:h.cupPos=p;
            break;
            case Kind::ExtraTee:h.spawnPositions.push_back(p);
            h.spawnIds.push_back("");
            object.index=h.spawnPositions.size()-1;
            break;
            case Kind::Wall:h.walls.push_back({p.x,p.y,size.x,size.y});
            object.index=h.walls.size()-1;
            break;
            case Kind::Surface:h.surfaces.push_back({p,size,surface});
            object.index=h.surfaces.size()-1;
            break;
            case Kind::Bumper:h.bumpers.push_back({p,40,550});
            object.index=h.bumpers.size()-1;
            break;
            case Kind::Laser:h.lasers.push_back({p,{200,8},2,2});
            object.index=h.lasers.size()-1;
            break;
            case Kind::Portal:throw std::runtime_error("Place both portal endpoints");
        }});
    select(object);
}
void Document::portals(Vec2 a,Vec2 b) {
    edit([&]{auto& h=current();
        int pair=0;
        for(auto& p:h.portals) pair=std::max(pair,p.pair+1);
        Color colors[]={{170,90,220},{70,205,235},{245,145,45},{230,80,140}};
        auto color=colors[pair%4];
        h.portals.push_back({a,pair,color});
        h.portals.push_back({b,pair,color});
    });
    select({Kind::Portal,current().portals.size()-2});
}
void Document::removeRail(size_t r) {
    auto& h=current();
    auto fix=[&](int& i){if(i==int(r)) i=-1;
        else if(i>int(r)) --i;
    };
    fix(h.cupRail);
    for(auto& w:h.walls) fix(w.rail);
    for(auto& b:h.bumpers) fix(b.rail);
    for(auto& l:h.lasers) fix(l.rail);
    for(auto& p:h.portals) fix(p.rail);
    h.rails.erase(h.rails.begin()+r);
}
}
