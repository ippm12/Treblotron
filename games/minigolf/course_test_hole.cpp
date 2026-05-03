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

static CourseHole makeTestHole()
{
    CourseHole h;
    // ~1500 x 700 play area, comfortably fits inside the 1920x1080 frame
    // with the scoreboard panel (480 px) on the right.
    h.areaTopLeft     = { 100.0f,  150.0f };
    h.areaBottomRight = { 1380.0f, 950.0f };

    // Start near the bottom centre, cup near the top centre.
    h.startPos = { 740.0f, 850.0f };
    h.cupPos   = { 740.0f, 250.0f };
    h.cupRadius = 24.0f;
    h.par      = 3;

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
