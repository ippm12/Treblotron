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
#include <array>
#include <cstdint>
#include <vector>


namespace MiniGolf
{

constexpr uint8_t HOLES_PER_GAME = 9;
constexpr uint8_t MAX_PLAYERS    = 6;
// Maximum strokes recorded for a hole. A player who hasn't holed out
// after STROKE_CAP - 1 actual throws gets STROKE_CAP recorded as their
// score and moves on. With 3 throws per turn, a player gets 3 full
// turns of attempts before the cap forces a move-on.
constexpr uint8_t STROKE_CAP     = 8;


struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};


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
