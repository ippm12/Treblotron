/**
 * course_test_hole.cpp
 *
 * A nine-hole sampler. The first four introduce every obstacle and rail
 * attachment; later holes combine them. Coordinates are course pixels.
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
    h.cupRadius = 30.0f;
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
            c.name = "Obstacle Sampler";
            for(uint8_t i = 0; i < HOLES_PER_GAME; ++i)
            {
                c.holes[i] = makeTestHole();
            }
            break;
    }

    // The first hole gives four distinct routes across the same distance.
    auto& surfaces=c.holes[0];
    surfaces.name="Surfaces: sand / ice / rough / water";
    surfaces.surfaces={
        {{280,505},{240,300},SurfaceKind::Sand},
        {{565,505},{240,300},SurfaceKind::Ice},
        {{850,505},{240,300},SurfaceKind::Rough},
        {{1135,505},{200,260},SurfaceKind::Water}
    };
    surfaces.startPos={705,800};

    auto& gadgets=c.holes[1];
    gadgets.name="Bumpers, timed lasers and paired portals";
    gadgets.bumpers={{{530,565},40,550},{{880,440},40,600}};
    gadgets.lasers={{{705,620},{270,8},1.5f,2.5f,0},{{705,340},{240,8},2,2,2}};
    gadgets.portals={
        {{340,650},0,{170,90,220}},{{1030,320},0,{170,90,220}},
        {{1080,650},1,{245,145,45}},{{380,310},1,{245,145,45}}
    };

    auto& rails=c.holes[2];
    rails.name="Rails: moving cup, walls and bumpers";
    rails.rails={
        {{{{0,0},1,90},{{240,0},0.5f,160}},RailMode::PingPong},
        {{{{0,0},0.8f,100},{{0,-140},0.3f,65},{{180,-140},0.5f,140},{{180,0},0,100}},RailMode::Loop},
        {{{{0,0},0.6f,150},{{-330,0},1,85}},RailMode::PingPong}
    };
    rails.cupPos={585,235}; rails.cupRail=0;
    addTileBarrier(rails,{300,550},40,{"#..","#..","###"},1);
    addTileBarrier(rails,{990,390},40,{"###",".#.",".#."},2);
    rails.bumpers={{{780,640},40,520,1}};

    auto& moving=c.holes[3];
    moving.name="Stationary surfaces with moving lasers and portals";
    moving.rails={
        {{{{0,0},0.8f,75},{{170,0},1,110}},RailMode::PingPong},
        {{{{0,0},0.3f,80},{{0,-100},0.5f,120},{{120,-100},0,80},{{120,0},0.6f,100}},RailMode::Loop}
    };
    moving.surfaces={
        {{260,520},{130,130},SurfaceKind::Sand},
        {{525,520},{130,130},SurfaceKind::Rough},
        {{790,520},{130,130},SurfaceKind::Ice},
        {{1100,470},{130,130},SurfaceKind::Water}
    };
    addTileBarrier(moving,{450,290},35,{"###","#.#","#.#"});
    moving.lasers={{{680,350},{230,8},1.5f,2.5f,0,0}};
    moving.portals={{{340,720},0,{70,205,235},0},{{950,240},0,{70,205,235},0}};

    c.holes[4]=gadgets; c.holes[4].name="Sand banks and bumper shortcuts";
    c.holes[4].surfaces={{{705,480},{180,180},SurfaceKind::Sand}};
    c.holes[5]=rails; c.holes[5].name="Ice with moving barriers";
    c.holes[5].surfaces={{{705,500},{450,360},SurfaceKind::Ice}};
    c.holes[6]=gadgets; c.holes[6].name="Water crossing via portals";
    c.holes[6].surfaces={{{705,485},{650,130},SurfaceKind::Water}};
    c.holes[7]=moving; c.holes[7].name="Moving hazards and rough approaches";
    c.holes[7].bumpers={{{705,660},40,550}};
    c.holes[8]=rails; c.holes[8].name="Finale: moving cup and laser gate";
    c.holes[8].surfaces={{{550,500},{180,160},SurfaceKind::Sand},{{900,600},{180,180},SurfaceKind::Ice}};
    c.holes[8].lasers={{{705,350},{260,8},1.2f,2.8f,0,0}};
    return c;
}

}  // namespace MiniGolf
