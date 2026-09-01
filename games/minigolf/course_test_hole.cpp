/**
 * course_test_hole.cpp
 *
 * The single built-in course: a rectangular box with the cup near the
 * top edge and the start position near the bottom. The same hole repeats
 * 9 times — useful as a baseline while the menu only exposes one course.
 */

#include "course_defs.hpp"


namespace MiniGolf
{

// A 1280 x 720 play area centred in the course window: inside
// COURSE_MAX_AREA_* (1350 x 790) with room to spare, so every boundary wall
// lands clear of the scoreboard panel and the player bar.
static constexpr float AREA_W = 1280.0f;
static constexpr float AREA_H =  720.0f;

// Tee to cup. The stroke tuning in minigolf.cpp is solved against this
// distance — a full-power putt rolls 1520 px and the cup sits at a dart
// radius of about 0.60 — so moving it means re-checking those numbers.
static constexpr float TEE_TO_CUP = 600.0f;


static CourseHole makeTestHole()
{
    CourseHole h;

    h.areaTopLeft     = { COURSE_VIEW_CENTER_X - 0.5f * AREA_W,
                          COURSE_VIEW_CENTER_Y - 0.5f * AREA_H };
    h.areaBottomRight = { COURSE_VIEW_CENTER_X + 0.5f * AREA_W,
                          COURSE_VIEW_CENTER_Y + 0.5f * AREA_H };

    // Cup near the top, tee TEE_TO_CUP below it, both on the centre line.
    // 60 px of run-off behind each keeps the ball clear of the walls.
    h.cupPos    = { COURSE_VIEW_CENTER_X, h.areaTopLeft.y + 60.0f };
    h.startPos  = { COURSE_VIEW_CENTER_X, h.cupPos.y + TEE_TO_CUP };
    h.cupRadius = 24.0f;
    h.par       = 3;

    // No interior obstacles for the test hole.
    return h;
}


Course buildCourse(CourseId id)
{
    Course c;
    switch(id)
    {
        case CourseId::TestHole:
        default:
            c.name = "Test Hole";
            for(uint8_t i = 0; i < HOLES_PER_GAME; ++i)
            {
                c.holes[i] = makeTestHole();
            }
            break;
    }
    return c;
}

}  // namespace MiniGolf
