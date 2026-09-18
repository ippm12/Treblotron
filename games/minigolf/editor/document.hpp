#pragma once
#include "course_io.hpp"
#include <functional>
#include <optional>

namespace GolfEditor {
using namespace MiniGolf;
enum class Kind { Tee, ExtraTee, Cup, Wall, Surface, Bumper, Laser, Portal };
struct Object { Kind kind; size_t index=0; bool operator==(const Object&) const = default; };
struct Document {
    Course course;
    size_t hole=0;
    std::vector<Object> selection;
    uint64_t revision=0, savedRevision=0, nextRevision=1;
    struct Snapshot { Course course; size_t hole; uint64_t revision; };
    std::vector<Snapshot> undoStack, redoStack;
    std::filesystem::path saveDirectory, sourceDirectory;
    CourseHole& current() {return course.holes.at(hole);}
    const CourseHole& current() const {return course.holes.at(hole);}
    bool dirty() const {return revision!=savedRevision;}
    void checkpoint();
    void undo();
    void redo();
    void edit(const std::function<void()>& action);
    void load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path);
    void create();
    Vec2 position(Object o) const;
    Vec2 extent(Object o) const;
    float angle(Object o) const;
    void rotate(float degrees);
    int rail(Object o) const;
    void attach(Object o,int railIndex);
    void move(Object o,Vec2 delta);
    void select(Object o,bool additive=false);
    void translate(Vec2 delta);
    void erase();
    void duplicate();
    void add(Kind kind,Vec2 position,Vec2 size={120,80},SurfaceKind surface=SurfaceKind::Sand);
    void portals(Vec2 first,Vec2 second);
    void removeRail(size_t index);
};
std::string newId(const std::string& prefix);
Course blankCourse();
}
