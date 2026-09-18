/**
 * course_defs.hpp
 *
 * Course / hole data structures for mini golf, plus the per-player ball
 * colour palette. Courses are loaded from versioned JSON files.
 * course_io.hpp is the shared editor/game persistence API. IDs survive edits.
 *
 * All coordinates are in **course pixels** (the world space the
 * PhysicsCamera looks into). One course pixel == one screen pixel at the
 * default zoom of 1.
 */

#ifndef MINIGOLF_COURSE_DEFS_HPP
#define MINIGOLF_COURSE_DEFS_HPP

#include "game_lib/components/render_object.hpp"  // Color
#include "game_lib/game_helpers.hpp"            // GameLayout
#include <array>
#include <cstdint>
#include <vector>
#include <initializer_list>
#include <string_view>
#include <string>


namespace MiniGolf
{

constexpr uint8_t HOLES_PER_GAME = 9;
constexpr uint8_t MAX_PLAYERS    = 6;
// Maximum strokes recorded for a hole. A player who hasn't holed out
// once they reach STROKE_CAP throws gets STROKE_CAP recorded as their
// score and moves on. With MAX_THROWS_PER_TURN (3) throws per turn that
// is two full turns plus a two-throw third turn before the cap ends it.
constexpr uint8_t STROKE_CAP     = 8;


struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

enum class RailMode { Loop, PingPong };
struct RailStop
{
    Vec2 offset;                 // Offset from each attached element's base position.
    float pauseSeconds = 0;
    float speed = 100;            // Pixels/second leaving this stop.
    std::string id{};
};
struct Rail
{
    std::vector<RailStop> stops;
    RailMode mode = RailMode::PingPong;
    float phaseSeconds = 0;
    std::string id{};
};

enum class SurfaceKind { Rough, Sand, Ice, Water };

// Leave room above each layer for details. All course geometry stays below
// GameLayout::SIDEBAR_Z (100); chrome/overlays retain their existing priority.
namespace CourseLayer {
constexpr uint32_t Felt=1, Rail=20;
constexpr uint32_t Ice=10, Rough=12, Sand=14, Water=16;
constexpr uint32_t Guide=95, Wall=30, Bumper=40, Laser=45;
constexpr uint32_t Portal=50, Cup=60, Trail=70, Ball=80, Aim=85, Effect=90;
}
constexpr uint32_t surfaceLayer(SurfaceKind kind)
{
    switch(kind) {
        case SurfaceKind::Ice: return CourseLayer::Ice;
        case SurfaceKind::Rough: return CourseLayer::Rough;
        case SurfaceKind::Sand: return CourseLayer::Sand;
        case SurfaceKind::Water: return CourseLayer::Water;
    }
    return CourseLayer::Felt;
}
struct SurfacePatch
{
    Vec2 center;
    Vec2 size;
    SurfaceKind kind = SurfaceKind::Rough; // Surfaces are always stationary.
    std::string id{};
    float angleDegrees = 0;
};
struct Bumper
{
    Vec2 center;
    float radius = 40;
    float kickSpeed = 550;
    int rail = -1;
    std::string id{};
};
struct Laser
{
    Vec2 center;
    Vec2 size{250, 8};
    float onSeconds = 2;
    float offSeconds = 2;
    float phaseSeconds = 0;
    int rail = -1;
    std::string id{};
    float angleDegrees = 0; // Beam direction; size.x is length, size.y is thickness.
};
struct Portal
{
    Vec2 center;
    // Portals use their hole's cupRadius for both drawing and entry tests.
    int pair = 0;                // Exactly two portals per pair, sharing a colour.
    Color color{170, 90, 220};
    int rail = -1;
    std::string id{};
};


// ============================================================================
// The course window
//
// The on-screen rectangle a course is drawn into, in screen pixels. It is
// bounded by the two pieces of chrome the game manager draws over everything:
// the scoreboard panel on the right, and the player bar along the bottom. The
// bar in particular is drawn at UINT32_MAX - 10, so anything a course puts
// below it is invisible regardless of its own z — which is exactly how the
// bottom boundary wall went missing when the window was 920 px tall and ran
// 70 px underneath it.
//
// A course that fits inside COURSE_MAX_AREA_* is pinned by the camera rather
// than scrolled, and because the standard area is centred in the window, course
// pixels then map 1:1 onto screen pixels.
// ============================================================================

constexpr float COURSE_VIEW_X = 0.0f;
constexpr float COURSE_VIEW_Y = 80.0f;    // strip above for the hole banner
constexpr float COURSE_VIEW_W = GameLayout::RIGHT_PANEL_X;              // 1410
constexpr float COURSE_VIEW_H = GameLayout::BAR_Y - COURSE_VIEW_Y;      //  850

constexpr float COURSE_VIEW_CENTER_X = COURSE_VIEW_X + 0.5f * COURSE_VIEW_W;  // 705
constexpr float COURSE_VIEW_CENTER_Y = COURSE_VIEW_Y + 0.5f * COURSE_VIEW_H;  // 505

/** Boundary walls are auto-built at this thickness, centred on the area edge. */
constexpr float COURSE_WALL_THICKNESS = 30.0f;

/** Largest play area whose boundary walls still land inside the window. */
constexpr float COURSE_MAX_AREA_W = COURSE_VIEW_W - 2.0f * COURSE_WALL_THICKNESS;  // 1350
constexpr float COURSE_MAX_AREA_H = COURSE_VIEW_H - 2.0f * COURSE_WALL_THICKNESS;  //  790


/**
 * Rotatable wall box. The course's outer rectangle is constructed as
 * four of these by the loader; obstacles are extra entries in the
 * walls vector.
 */
struct WallBox
{
    float centerX = 0.0f;
    float centerY = 0.0f;
    float width   = 0.0f;
    float height  = 0.0f;
    int rail = -1;
    bool showRail = true;
    std::string id{};
    std::string group{}; // Stable editor grouping; each wall retains its own geometry.
    float angleDegrees = 0;
};


struct CourseHole
{
    Vec2  startPos;
    std::vector<Vec2> spawnPositions; // Optional tees, assigned round-robin by player.
    Vec2  cupPos;
    float cupRadius = 22.0f;

    // Outer play area extent. Used for camera bounds and to draw the
    // floor. The loader auto-builds the four boundary walls from this
    // rectangle so authors don't have to type them out per hole.
    Vec2  areaTopLeft;
    Vec2  areaBottomRight;

    // Interior obstacles only — boundary walls are auto-generated.
    std::vector<WallBox> walls;

    std::string name = "Practice";
    std::vector<Rail> rails;
    int cupRail = -1;
    std::vector<SurfacePatch> surfaces;
    std::vector<Bumper> bumpers;
    std::vector<Laser> lasers;
    std::vector<Portal> portals;

    int   par = 3;
    std::string id{};
    std::vector<std::string> spawnIds{};
};


// Build a compound barrier from a square grid. '#' is solid; other cells
// are empty. Tiles share one rail so the entire shape translates together.
// Adjacent square faces meet exactly in both drawing and collision geometry.
inline void addTileBarrier(CourseHole& hole,Vec2 topLeft,float tileSize,
                           std::initializer_list<std::string_view> rows,int rail=-1)
{
    if(tileSize<=0) return;
    const auto group="barrier-"+std::to_string(hole.walls.size()+1);
    size_t y=0;
    bool first=true;
    for(auto row:rows) {
        for(size_t x=0;x<row.size();++x) if(row[x]=='#') {
            hole.walls.push_back({topLeft.x+(x+0.5f)*tileSize,
                topLeft.y+(y+0.5f)*tileSize,tileSize,tileSize,rail,first});
            hole.walls.back().group=group;
            first=false;
        }
        ++y;
    }
}


struct Course
{
    std::string name = "";
    std::string theme = "classic";
    std::array<CourseHole, HOLES_PER_GAME> holes;
    std::string id{};
};


enum class CourseId : uint8_t
{
    TestHole = 0,
};


/** Load a built-in course (including any local override) from JSON. */
Course buildCourse(CourseId id);


/**
 * Per-player ball colour palette. Index 0..5 maps to player 0..5.
 * Order chosen to read distinctly even on small swatches: blue, red,
 * green, yellow, purple, orange.
 */
constexpr std::array<Color, MAX_PLAYERS> BALL_COLORS = {{
    {  60, 130, 240 },  // 0 blue
    { 230,  60,  70 },  // 1 red
    {  70, 200,  90 },  // 2 green
    { 240, 220,  60 },  // 3 yellow
    { 170,  90, 220 },  // 4 purple
    { 240, 140,  40 },  // 5 orange
}};

constexpr float BALL_RADIUS_PX = 16.0f;

}  // namespace MiniGolf

#endif // MINIGOLF_COURSE_DEFS_HPP
