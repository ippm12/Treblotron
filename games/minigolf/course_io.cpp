#include "course_io.hpp"
#include "course_themes.hpp"
#include "debug/app_paths.hpp"
#include <opencv2/core.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace MiniGolf {
namespace {
using Node=cv::FileNode;
using Writer=cv::FileStorage;
void require(bool ok,const std::string& message) { if(!ok) throw std::runtime_error(message); }
bool validId(const std::string& id) {
    return !id.empty() && id.size()<=80 && std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_';
    });
}
std::string str(Node n) { require(n.isString(),"Expected string"); return static_cast<std::string>(n); }
float num(Node n) {
    require(n.isInt() || n.isReal(),"Expected number");
    double v=static_cast<double>(n);
    require(std::isfinite(v) && std::abs(v)<=1000000,"Number out of range");
    return static_cast<float>(v);
}
int integer(Node n) { float v=num(n); require(v==std::floor(v),"Expected integer"); return static_cast<int>(v); }
void keys(Node n,std::initializer_list<const char*> allowed,bool angles=false) {
    require(n.isMap(),"Expected object");
    for(auto item:n) require((angles && item.name()=="angleDegrees") || std::any_of(allowed.begin(),allowed.end(),[&](const char* k){return item.name()==k;}),"Unknown field: "+item.name());
    for(auto key:allowed) require(!n[key].isNone(),"Missing field: "+std::string(key));
}
Node array(Node n) { require(n.isSeq() && n.size()<=10000,"Expected bounded array"); return n; }
Vec2 vec(Node n) { array(n); require(n.size()==2,"Expected two coordinates"); return {num(n[0]),num(n[1])}; }
void value(Writer& w,float n) { w << std::round(static_cast<double>(n)*1000000)/1000000; }
void vector(Writer& w,Vec2 v) { w << "[:"; value(w,v.x); value(w,v.y); w << "]"; }
void field(Writer& w,const char* key,float n) { w << key; value(w,n); }
void field(Writer& w,const char* key,Vec2 v) { w << key; vector(w,v); }
std::string railId(const CourseHole& h,int index) { return index<0 ? "" : h.rails.at(index).id; }
int railIndex(const CourseHole& h,Node n) {
    auto id=str(n); if(id.empty()) return -1;
    for(size_t i=0;i<h.rails.size();++i) if(h.rails[i].id==id) return static_cast<int>(i);
    throw std::runtime_error("Unknown rail: "+id);
}
// OpenCV supplies JSON escaping/parsing. Normalize only numeric tokens outside
// strings so its 17-digit double formatting does not pollute human edits.
std::string canonicalJson(const std::string& text) {
    std::string out; bool quoted=false,escaped=false;
    for(size_t i=0;i<text.size();) {
        char c=text[i];
        if(quoted) { out+=c; ++i; if(escaped) escaped=false; else if(c=='\\') escaped=true; else if(c=='"') quoted=false; continue; }
        if(c=='"') { quoted=true; out+=c; ++i; continue; }
        if(c=='-' || std::isdigit(static_cast<unsigned char>(c))) {
            size_t end=i+1; while(end<text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end]=='.' || text[end]=='e' || text[end]=='E' || text[end]=='+' || text[end]=='-')) ++end;
            double number=std::stod(text.substr(i,end-i)); char buffer[128];
            auto result=std::to_chars(buffer,buffer+sizeof(buffer),number,std::chars_format::fixed,6);
            require(result.ec==std::errc{},"Cannot format course number");
            std::string token(buffer,result.ptr); while(token.back()=='0') token.pop_back(); if(token.back()=='.') token.pop_back(); if(token=="-0") token="0";
            out+=token; i=end;
        } else { out+=c; ++i; }
    }
    return out;
}
Writer writer() { return Writer(".json",Writer::WRITE|Writer::MEMORY|Writer::FORMAT_JSON); }
std::string holeText(const CourseHole& h) {
    auto w=writer();
    const bool rotated=std::any_of(h.walls.begin(),h.walls.end(),[](const auto& o){return o.angleDegrees!=0;})
        || std::any_of(h.surfaces.begin(),h.surfaces.end(),[](const auto& o){return o.angleDegrees!=0;})
        || std::any_of(h.lasers.begin(),h.lasers.end(),[](const auto& o){return o.angleDegrees!=0;});
    w << "formatVersion" << (rotated?2:1) << "id" << h.id << "name" << h.name << "par" << h.par;
    w << "bounds" << "{"; field(w,"topLeft",h.areaTopLeft); field(w,"bottomRight",h.areaBottomRight); w << "}";
    w << "tee" << "{" << "id" << "tee"; field(w,"position",h.startPos); w << "}";
    w << "extraTees" << "[";
    for(size_t i=0;i<h.spawnPositions.size();++i) { w << "{" << "id" << h.spawnIds[i]; field(w,"position",h.spawnPositions[i]); w << "}"; } w << "]";
    w << "cup" << "{" << "id" << "cup"; field(w,"position",h.cupPos); field(w,"radius",h.cupRadius); w << "rail" << railId(h,h.cupRail) << "}";
    w << "rails" << "[";
    for(const auto& r:h.rails) {
        w << "{" << "id" << r.id << "mode" << (r.mode==RailMode::Loop ? "loop":"pingPong"); field(w,"phaseSeconds",r.phaseSeconds);
        w << "stops" << "[";
        for(const auto& s:r.stops) { w << "{" << "id" << s.id; field(w,"offset",s.offset); field(w,"pauseSeconds",s.pauseSeconds); field(w,"speed",s.speed); w << "}"; }
        w << "]" << "}";
    } w << "]";
    w << "walls" << "[";
    for(const auto& b:h.walls) { w << "{" << "id" << b.id << "group" << b.group; field(w,"center",Vec2{b.centerX,b.centerY}); field(w,"size",Vec2{b.width,b.height}); if(b.angleDegrees!=0) field(w,"angleDegrees",b.angleDegrees); w << "rail" << railId(h,b.rail) << "showRail" << int(b.showRail) << "}"; } w << "]";
    const char* kinds[]={"rough","sand","ice","water"};
    w << "surfaces" << "[";
    for(const auto& s:h.surfaces) { w << "{" << "id" << s.id << "kind" << kinds[static_cast<int>(s.kind)]; field(w,"center",s.center); field(w,"size",s.size); if(s.angleDegrees!=0) field(w,"angleDegrees",s.angleDegrees); w << "}"; } w << "]";
    w << "bumpers" << "[";
    for(const auto& b:h.bumpers) { w << "{" << "id" << b.id; field(w,"center",b.center); field(w,"radius",b.radius); field(w,"kickSpeed",b.kickSpeed); w << "rail" << railId(h,b.rail) << "}"; } w << "]";
    w << "lasers" << "[";
    for(const auto& l:h.lasers) { w << "{" << "id" << l.id; field(w,"center",l.center); field(w,"size",l.size); if(l.angleDegrees!=0) field(w,"angleDegrees",l.angleDegrees); field(w,"onSeconds",l.onSeconds); field(w,"offSeconds",l.offSeconds); field(w,"phaseSeconds",l.phaseSeconds); w << "rail" << railId(h,l.rail) << "}"; } w << "]";
    w << "portals" << "[";
    for(const auto& p:h.portals) { w << "{" << "id" << p.id; field(w,"center",p.center); w << "pair" << p.pair << "color" << "[:" << int(p.color.r) << int(p.color.g) << int(p.color.b) << "]" << "rail" << railId(h,p.rail) << "}"; } w << "]";
    return canonicalJson(w.releaseAndGetString());
}
Writer readFile(const std::filesystem::path& path,bool /*manifest*/=false) {
    require(std::filesystem::file_size(path)<=4*1024*1024,"Course file exceeds 4 MiB: "+path.string());
    std::ifstream input(path,std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)),{});
    require(input.good() || input.eof(),"Cannot read "+path.string());
    Writer reader(text,Writer::READ|Writer::MEMORY|Writer::FORMAT_JSON);
    require(reader.isOpened(),"Cannot parse "+path.string());
    const int version=integer(reader["formatVersion"]);
    require(version==1 || version==2,"Unsupported course format version");
    return reader;
}
CourseHole readHole(const std::filesystem::path& path) {
    try {
        auto f=readFile(path); auto root=f.root();
        bool angles=integer(root["formatVersion"])>=2;
        auto angle=[](Node n){return n["angleDegrees"].isNone()?0.0f:num(n["angleDegrees"]);};
        keys(root,{"formatVersion","id","name","par","bounds","tee","extraTees","cup","rails","walls","surfaces","bumpers","lasers","portals"});
        CourseHole h; h.id=str(root["id"]); h.name=str(root["name"]); h.par=integer(root["par"]);
        auto bounds=root["bounds"]; keys(bounds,{"topLeft","bottomRight"}); h.areaTopLeft=vec(bounds["topLeft"]); h.areaBottomRight=vec(bounds["bottomRight"]);
        auto tee=root["tee"]; keys(tee,{"id","position"}); require(str(tee["id"])=="tee","Primary tee ID must be tee"); h.startPos=vec(tee["position"]);
        for(auto n:array(root["extraTees"])) { keys(n,{"id","position"}); h.spawnIds.push_back(str(n["id"])); h.spawnPositions.push_back(vec(n["position"])); }
        for(auto n:array(root["rails"])) {
            keys(n,{"id","mode","phaseSeconds","stops"}); Rail r; r.id=str(n["id"]); auto mode=str(n["mode"]);
            require(mode=="loop" || mode=="pingPong","Unknown rail mode"); r.mode=mode=="loop" ? RailMode::Loop:RailMode::PingPong; r.phaseSeconds=num(n["phaseSeconds"]);
            for(auto s:array(n["stops"])) { keys(s,{"id","offset","pauseSeconds","speed"}); r.stops.push_back({vec(s["offset"]),num(s["pauseSeconds"]),num(s["speed"]),str(s["id"])}); } h.rails.push_back(r);
        }
        auto cup=root["cup"]; keys(cup,{"id","position","radius","rail"}); require(str(cup["id"])=="cup","Cup ID must be cup"); h.cupPos=vec(cup["position"]); h.cupRadius=num(cup["radius"]); h.cupRail=railIndex(h,cup["rail"]);
        for(auto n:array(root["walls"])) {
            keys(n,{"id","group","center","size","rail","showRail"},angles); auto p=vec(n["center"]),s=vec(n["size"]); int show=integer(n["showRail"]); require(show==0 || show==1,"showRail must be 0 or 1");
            h.walls.push_back({p.x,p.y,s.x,s.y,railIndex(h,n["rail"]),show!=0,str(n["id"]),str(n["group"]),angle(n)});
        }
        for(auto n:array(root["surfaces"])) {
            keys(n,{"id","kind","center","size"},angles); auto kind=str(n["kind"]); const std::map<std::string,SurfaceKind> kinds={{"rough",SurfaceKind::Rough},{"sand",SurfaceKind::Sand},{"ice",SurfaceKind::Ice},{"water",SurfaceKind::Water}};
            require(kinds.count(kind),"Unknown surface: "+kind); h.surfaces.push_back({vec(n["center"]),vec(n["size"]),kinds.at(kind),str(n["id"]),angle(n)});
        }
        for(auto n:array(root["bumpers"])) { keys(n,{"id","center","radius","kickSpeed","rail"}); h.bumpers.push_back({vec(n["center"]),num(n["radius"]),num(n["kickSpeed"]),railIndex(h,n["rail"]),str(n["id"])}); }
        for(auto n:array(root["lasers"])) { keys(n,{"id","center","size","onSeconds","offSeconds","phaseSeconds","rail"},angles); h.lasers.push_back({vec(n["center"]),vec(n["size"]),num(n["onSeconds"]),num(n["offSeconds"]),num(n["phaseSeconds"]),railIndex(h,n["rail"]),str(n["id"]),angle(n)}); }
        for(auto n:array(root["portals"])) {
            keys(n,{"id","center","pair","color","rail"}); auto color=array(n["color"]); require(color.size()==3,"Expected RGB color"); int rgb[3]; for(int i=0;i<3;++i) { rgb[i]=integer(color[i]); require(rgb[i]>=0 && rgb[i]<=255,"Color outside 0..255"); }
            h.portals.push_back({vec(n["center"]),integer(n["pair"]),{static_cast<uint8_t>(rgb[0]),static_cast<uint8_t>(rgb[1]),static_cast<uint8_t>(rgb[2])},railIndex(h,n["rail"]),str(n["id"])});
        }
        return h;
    } catch(const std::exception& e) { throw std::runtime_error(path.string()+": "+e.what()); }
}
void writeChanged(const std::filesystem::path& path,const std::string& text) {
    std::ifstream input(path,std::ios::binary); std::string old((std::istreambuf_iterator<char>(input)),{}); input.close();
    if(old==text) return;
    std::filesystem::create_directories(path.parent_path());
    auto temp=path; temp += ".tmp";
    try {
        std::ofstream out(temp,std::ios::binary|std::ios::trunc); out.exceptions(std::ios::failbit|std::ios::badbit); out << text; out.flush(); out.close();
#ifdef _WIN32
        require(MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot replace "+path.string());
#else
        std::filesystem::rename(temp,path);
#endif
    } catch(...) { std::error_code ec; std::filesystem::remove(temp,ec); throw; }
}
}

void assignCourseIds(Course& c) {
    if(c.id.empty()) c.id="course";
    auto assign=[](auto& items,const std::string& prefix) {
        std::set<std::string> used; for(const auto& item:items) if(!item.id.empty()) used.insert(item.id);
        size_t next=1;
        for(auto& item:items) if(item.id.empty()) { std::string id; do { id=prefix+std::to_string(next++); } while(used.count(id)); item.id=id; used.insert(id); }
    };
    assign(c.holes,"hole-");
    for(auto& h:c.holes) {
        assign(h.rails,"rail-"); for(auto& r:h.rails) assign(r.stops,"stop-");
        assign(h.walls,"wall-"); assign(h.surfaces,"surface-"); assign(h.bumpers,"bumper-"); assign(h.lasers,"laser-"); assign(h.portals,"portal-");
        h.spawnIds.resize(h.spawnPositions.size()); std::set<std::string> used(h.spawnIds.begin(),h.spawnIds.end()); size_t n=1;
        for(auto& id:h.spawnIds) if(id.empty()) { do { id="tee-"+std::to_string(n++); } while(used.count(id)); used.insert(id); }
    }
}
void validateCourse(const Course& c) {
    (void)courseTheme(c.theme);
    require(validId(c.id),"Invalid course ID"); require(!c.name.empty(),"Course name is empty");
    std::set<std::string> holes;
    auto finite=[](float x){ require(std::isfinite(x) && std::abs(x)<=1000000,"Invalid numeric value"); };
    auto point=[&](Vec2 p){ finite(p.x); finite(p.y); };
    auto positive=[&](float x){ finite(x); require(x>=0.000001f,"Positive values must be at least 0.000001 (file precision)"); };
    auto nonnegative=[&](float x){ finite(x); require(x>=0,"Expected nonnegative value"); };
    auto size=[&](Vec2 s){positive(s.x); positive(s.y);};
    for(const auto& h:c.holes) {
        try {
            require(validId(h.id) && holes.insert(h.id).second,"Invalid or duplicate hole ID"); require(!h.name.empty(),"Hole name is empty"); require(h.par>0 && h.par<=STROKE_CAP,"Par outside stroke range");
            point(h.areaTopLeft); point(h.areaBottomRight); size({h.areaBottomRight.x-h.areaTopLeft.x,h.areaBottomRight.y-h.areaTopLeft.y});
            point(h.startPos); point(h.cupPos); positive(h.cupRadius);
            auto rail=[&](int i){require(i>=-1 && i<static_cast<int>(h.rails.size()),"Invalid rail reference");}; rail(h.cupRail);
            std::set<std::string> ids{"tee","cup"};
            auto id=[&](const std::string& s){require(validId(s) && ids.insert(s).second,"Invalid or duplicate object ID: "+s);};
            require(h.spawnIds.size()==h.spawnPositions.size(),"Extra tee IDs missing");
            for(size_t i=0;i<h.spawnPositions.size();++i) { id(h.spawnIds[i]); point(h.spawnPositions[i]); }
            for(const auto& r:h.rails) { id(r.id); require(r.mode==RailMode::Loop || r.mode==RailMode::PingPong,"Invalid rail mode"); nonnegative(r.phaseSeconds); require(r.stops.size()>=2,"Rail needs two stops"); std::set<std::string> stops;
                for(const auto& s:r.stops) { require(validId(s.id) && stops.insert(s.id).second,"Duplicate/invalid stop ID"); point(s.offset); nonnegative(s.pauseSeconds); positive(s.speed); }
            }
            std::map<std::string,int> groups;
            for(const auto& b:h.walls) { id(b.id); point({b.centerX,b.centerY}); size({b.width,b.height}); finite(b.angleDegrees); rail(b.rail); if(!b.group.empty()) { require(validId(b.group),"Invalid wall group"); auto [it,inserted]=groups.emplace(b.group,b.rail); require(inserted || it->second==b.rail,"Wall group must share a rail"); } }
            for(const auto& s:h.surfaces) { id(s.id); point(s.center); size(s.size); finite(s.angleDegrees); require(static_cast<int>(s.kind)>=0 && static_cast<int>(s.kind)<=3,"Invalid surface kind"); }
            for(const auto& b:h.bumpers) { id(b.id); point(b.center); positive(b.radius); nonnegative(b.kickSpeed); rail(b.rail); }
            for(const auto& l:h.lasers) { id(l.id); point(l.center); size(l.size); finite(l.angleDegrees); nonnegative(l.onSeconds); nonnegative(l.offSeconds); positive(l.onSeconds+l.offSeconds); nonnegative(l.phaseSeconds); rail(l.rail); }
            std::map<int,std::vector<Color>> pairs;
            for(const auto& p:h.portals) { id(p.id); point(p.center); require(p.pair>=0,"Invalid portal pair"); rail(p.rail); pairs[p.pair].push_back(p.color); }
            for(const auto& [pair,colors]:pairs) { (void)pair; require(colors.size()==2,"Portal pair needs exactly two portals"); require(colors[0].r==colors[1].r && colors[0].g==colors[1].g && colors[0].b==colors[1].b,"Portal pair colors differ"); }
        } catch(const std::exception& e) { throw std::runtime_error("Hole "+h.id+": "+e.what()); }
    }
}
Course loadCourse(const std::filesystem::path& directory) {
    try {
        auto f=readFile(directory/"course.json",true); auto root=f.root();
        const bool themed=integer(root["formatVersion"])==2;
        if(themed) keys(root,{"formatVersion","id","name","theme","holes"});
        else keys(root,{"formatVersion","id","name","holes"});
        Course c; if(themed) c.theme=str(root["theme"]); c.id=str(root["id"]); c.name=str(root["name"]); auto holes=array(root["holes"]); require(holes.size()==HOLES_PER_GAME,"A playable course must contain nine holes");
        size_t i=0; for(auto n:holes) { auto id=str(n); require(validId(id),"Invalid hole file ID"); c.holes[i]=readHole(directory/"holes"/(id+".json")); require(c.holes[i].id==id,"Hole ID differs from manifest"); ++i; }
        validateCourse(c); return c;
    } catch(const std::exception& e) { throw std::runtime_error("Course "+directory.string()+": "+e.what()); }
}
void saveCourse(const Course& c,const std::filesystem::path& directory) {
    validateCourse(c);
    std::vector<std::pair<std::filesystem::path,std::string>> files;
    for(const auto& h:c.holes) files.emplace_back(directory/"holes"/(h.id+".json"),holeText(h));
    auto w=writer(); w << "formatVersion" << (c.theme=="classic" ? 1:2) << "id" << c.id << "name" << c.name;
    if(c.theme!="classic") w << "theme" << c.theme;
    w << "holes" << "["; for(const auto& h:c.holes) w << h.id; w << "]";
    files.emplace_back(directory/"course.json",canonicalJson(w.releaseAndGetString()));
    for(const auto& [path,text]:files) writeChanged(path,text);
}
std::filesystem::path defaultCourseDirectory() {
    auto local=std::filesystem::path(appDataPath("courses/obstacle-sampler"));
    if(std::filesystem::exists(local/"course.json")) return local;
    return appAssetPath("assets/games/minigolf/courses/obstacle-sampler");
}
std::vector<CourseFile> availableCourses() {
    std::map<std::string,CourseFile> found;
    for(const auto& root:{std::filesystem::path(appAssetPath("assets/games/minigolf/courses")),std::filesystem::path(appDataPath("courses"))}) {
        std::error_code ec;
        for(const auto& entry:std::filesystem::directory_iterator(root,ec)) {
            if(!entry.is_directory() || !std::filesystem::exists(entry.path()/"course.json")) continue;
            auto id=entry.path().filename().string();
            std::string name=id+" (invalid course)";
            try { auto f=readFile(entry.path()/"course.json",true); name=str(f["name"]); } catch(const std::exception&) { /* Keep invalid courses selectable so load reports the error. */ }
            found[id]={name,entry.path()};
        }
    }
    std::vector<CourseFile> result;
    auto sampler=found.find("obstacle-sampler");
    if(sampler!=found.end()) { result.push_back(sampler->second); found.erase(sampler); }
    for(const auto& [id,entry]:found) { (void)id; result.push_back(entry); }
    if(result.empty()) result.push_back({"Obstacle Sampler",defaultCourseDirectory()});
    return result;
}
} // namespace MiniGolf

