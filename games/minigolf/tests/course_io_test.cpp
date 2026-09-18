#include "course_io.hpp"
#include "course_themes.hpp"
#include "fixtures/legacy_course.hpp"
#include <cassert>
#include <algorithm>
#include <fstream>
#include <limits>

namespace {
MiniGolf::Course legacy() {
    auto c=MiniGolf::buildLegacyCourse(MiniGolf::CourseId::TestHole);
    c.id="obstacle-sampler";
    const char* ids[]={"surfaces","gadgets","rails","moving","sand-banks","ice-barriers","water-crossing","rough-approaches","finale"};
    for(size_t i=0;i<c.holes.size();++i) c.holes[i].id=ids[i];
    MiniGolf::assignCourseIds(c); return c;
}
std::string bytes(const std::filesystem::path& p) { std::ifstream in(p,std::ios::binary); return {(std::istreambuf_iterator<char>(in)),{}}; }
}
void export_legacy_course(const std::string& directory) { MiniGolf::saveCourse(legacy(),directory); }
void test_course_io() {
    using namespace MiniGolf;
    const auto output=std::filesystem::current_path()/"course-io";
    auto expected=legacy(); saveCourse(expected,output/"expected");
    auto rotated=expected;
    rotated.holes[0].walls.push_back({600,400,160,20});
    rotated.holes[0].walls.back().angleDegrees=37;
    rotated.holes[0].surfaces[0].angleDegrees=-23;
    rotated.holes[0].lasers.push_back({{700,500},{220,8}});
    rotated.holes[0].lasers.back().angleDegrees=65;
    assignCourseIds(rotated);saveCourse(rotated,output/"rotated");
    auto restored=loadCourse(output/"rotated");
    assert(restored.holes[0].walls.back().angleDegrees==37);
    assert(restored.holes[0].surfaces[0].angleDegrees==-23);
    assert(restored.holes[0].lasers.back().angleDegrees==65);
    auto rotatedFile=output/"rotated/holes/surfaces.json";
    auto rotatedBytes=bytes(rotatedFile);saveCourse(restored,output/"rotated");
    assert(bytes(rotatedFile)==rotatedBytes);
    auto loaded=loadCourse(defaultCourseDirectory()); saveCourse(loaded,output/"loaded");
    // Full serialization compares every authored field, including wall groups.
    for(const auto& entry:std::filesystem::recursive_directory_iterator(output/"expected")) if(entry.is_regular_file()) {
        auto relative=std::filesystem::relative(entry.path(),output/"expected");
        assert(bytes(entry.path())==bytes(output/"loaded"/relative));
    }
    auto roundtrip=loadCourse(output/"loaded");
    const auto hole=output/"loaded/holes/surfaces.json";
    auto stamp=std::filesystem::last_write_time(hole);
    saveCourse(roundtrip,output/"loaded"); assert(std::filesystem::last_write_time(hole)==stamp);
    std::swap(roundtrip.holes[0],roundtrip.holes[1]); saveCourse(roundtrip,output/"loaded");
    assert(std::filesystem::last_write_time(hole)==stamp);
    roundtrip.holes[0].bumpers[0].center.x+=1;
    saveCourse(roundtrip,output/"loaded"); assert(std::filesystem::last_write_time(hole)==stamp);
    auto edited=loadCourse(output/"loaded"); assert(edited.holes[0].bumpers[0].center.x==expected.holes[1].bumpers[0].center.x+1);
    // Theme changes never rewrite hole geometry, and legacy remains readable.
    for(const auto& theme:COURSE_THEMES) {
        auto themed=expected; themed.theme=theme.id;
        saveCourse(themed,output/"themes");
        assert(loadCourse(output/"themes").theme==theme.id);
        assert(bytes(output/"themes/holes/surfaces.json")==bytes(output/"expected/holes/surfaces.json"));
    }
    // Load every authored starter course through the real strict JSON reader.
    const auto bundledRoot=defaultCourseDirectory().parent_path();
    const auto catalog=availableCourses();
    for(const auto& theme:COURSE_THEMES) {
        if(std::string(theme.id)=="classic") continue;
        const auto starter=loadCourse(bundledRoot/theme.id);
        assert(starter.theme==theme.id && starter.name==theme.name);
        assert(std::any_of(catalog.begin(),catalog.end(),[&](const auto& entry){return entry.name==starter.name;}));
        saveCourse(starter,output/"starter-roundtrip"/theme.id);
        assert(loadCourse(output/"starter-roundtrip"/theme.id).holes[8].name==starter.holes[8].name);
    }
    auto rejects=[&](auto change) { auto bad=expected; change(bad); bool rejected=false; try {saveCourse(bad,output/"loaded");} catch(const std::exception&) {rejected=true;} assert(rejected); };
    rejects([](auto& c){c.theme="unknown-theme";});
    rejects([](auto& c){c.holes[0].id="../outside";});
    rejects([](auto& c){c.holes[1].portals.pop_back();});
    rejects([](auto& c){c.holes[2].cupRail=999;});
    rejects([](auto& c){c.holes[0].surfaces[0].size.x=-1;});
    rejects([](auto& c){c.holes[0].cupRadius=std::numeric_limits<float>::quiet_NaN();});
    rejects([](auto& c){c.holes[0].cupRadius=0.0000001f;});
    rejects([](auto& c){c.holes[1].portals[1].id=c.holes[1].portals[0].id;});
    auto names=expected; names.name="Course 123 with \"quotes\" and \\ paths";
    names.holes[0].name="Hole 42 / 0.8";
    saveCourse(names,output/"strings"); auto named=loadCourse(output/"strings");
    assert(named.name==names.name && named.holes[0].name==names.holes[0].name);
    // The catalog discovers custom folders and resolves local default overrides.
    const auto localRoot=std::filesystem::current_path()/"courses";
    auto checkCatalog=[&](const std::string& folder,bool overrideDefault) {
        const auto directory=localRoot/folder;
        assert(!std::filesystem::exists(directory) || std::filesystem::is_empty(directory));
        saveCourse(names,directory);
        auto catalog=availableCourses();
        assert(std::any_of(catalog.begin(),catalog.end(),[&](const auto& entry){return entry.directory==directory && entry.name==names.name;}));
        if(overrideDefault) {
            assert(defaultCourseDirectory()==directory);
            assert(loadCourse(defaultCourseDirectory()).name==names.name);
            assert(std::count_if(catalog.begin(),catalog.end(),[](const auto& entry){return entry.directory.filename()=="obstacle-sampler";})==1);
        }
        for(const auto& h:names.holes) std::filesystem::remove(directory/"holes"/(h.id+".json"));
        std::filesystem::remove(directory/"holes"); std::filesystem::remove(directory/"course.json"); std::filesystem::remove(directory);
    };
    checkCatalog("editor-test",false); checkCatalog("obstacle-sampler",true);
    auto fresh=expected; fresh.holes[1].bumpers.insert(fresh.holes[1].bumpers.begin(),Bumper{});
    assignCourseIds(fresh); assert(fresh.holes[1].bumpers[1].id==expected.holes[1].bumpers[0].id);
    assert(fresh.holes[1].bumpers[0].id!=fresh.holes[1].bumpers[1].id);
    // Bad version and unknown fields must not silently disappear on save.
    auto manifest=output/"loaded/course.json"; auto original=bytes(manifest);
    for(const auto& text:{std::string("{\"formatVersion\": 99}"),std::string("{broken"),std::string("{\"formatVersion\":1,\"surprise\":true}")}) {
        {std::ofstream out(manifest); out<<text;}
        bool rejected=false; try {loadCourse(output/"loaded");} catch(const std::exception&) {rejected=true;} assert(rejected);
    }
    {std::ofstream out(manifest); out<<original;}
}
