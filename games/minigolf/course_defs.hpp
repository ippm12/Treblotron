/**
 * course_defs.hpp
 *
 * Course / hole data structures for mini golf, plus the per-player ball
 * colour palette. Courses are hard-coded as static const data for now.
 * The struct fields mirror what a future JSON loader would parse, so
 * migrating to data files is mechanical when courses are user-authored.
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
 * Axis-aligned wall box. The course's outer rectangle is constructed as
 * four of these by the loader; obstacles are extra entries in the
 * walls vector.
 */
struct WallBox
{
    float centerX = 0.0f;
    float centerY = 0.0f;
    float width   = 0.0f;
    float height  = 0.0f;
};


struct CourseHole
{
    Vec2  startPos;
    Vec2  cupPos;
    float cupRadius = 22.0f;

    // Outer play area extent. Used for camera bounds and to draw the
    // floor. The loader auto-builds the four boundary walls from this
    // rectangle so authors don't have to type them out per hole.
    Vec2  areaTopLeft;
    Vec2  areaBottomRight;

    // Interior obstacles only — boundary walls are auto-generated.
    std::vector<WallBox> walls;

    int   par = 3;
};


struct Course
{
    const char* name = "";
    std::array<CourseHole, HOLES_PER_GAME> holes;
};


enum class CourseId : uint8_t
{
    TestHole = 0,
};


/** Build the course for a given id. Lives in course_<name>.cpp. */
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
