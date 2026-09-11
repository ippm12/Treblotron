# Mini Golf obstacle sampler

Select **Mini Golf → Course: Obstacle Sampler**. The first four holes expose
all elements; holes 5–9 combine them. The numbered title above the course
identifies what each hole demonstrates.

| Hole | Features |
|---|---|
| 1 | Sand, ice, rough and water, arranged left to right |
| 2 | Pinball bumpers, timed lasers, purple and orange portal pairs |
| 3 | Rail-mounted cup, L/T-shaped tile barriers and bumper; loops, reversals and pauses |
| 4 | Stationary surfaces, moving laser and cyan portals; U-shaped barrier |

## Behaviours

- **Sand:** 5.25 times the normal rolling resistance.
- **Rough:** 2.5 times the normal rolling resistance.
- **Ice:** 0.12 times the resistance, with low visual spin grip. The ball
  slides while its rotation catches up. Patches change resistance; they do
  not carry a resting ball. All surfaces, including water, are stationary.
- **Water:** once the ball centre is inside the inset water boundary, the
  ball disappears for a splash, gains one penalty stroke, and returns to
  its position at the beginning of the putt. The disabled ball cannot collide
  with another ball during the animation. A penalty does not consume another
  dart from the turn, and scores remain capped at eight.
  Brief respawn protection prevents repeated penalties if a moving hazard
  covers the return position. Starting a new putt ends that protection.
- **Lasers:** active beams explode any player's ball, applying the same
  penalty/reset as water, including during collection and hole banners.
  Dim beams are off. All obstacles and the cup affect inactive players too.
- **Bumpers:** larger solid orange caps that kick contacted balls outward and
  compress, flash and emit impact rays on contact. Each bumper
  has its own kick speed. The kick adds only the missing outward speed rather
  than repeatedly adding energy on every physics step.
- **Portals:** exactly two per pair ID, with the same colour. Entry transports
  the ball just outside the partner along its travel direction relative to
  the entrance. Portals have the same radius as the cup and no facing angle.
  Fixed portals preserve world-axis velocity exactly. Moving portals use
  `outgoing = ball velocity - entrance velocity + exit velocity`, without rotation.
  Visual orientation is preserved and the old trail clears.
  A cooldown and a leave-the-portal requirement prevent immediate return loops.
  Exits blocked by walls, bumpers, other balls or the course boundary do not
  teleport the ball.

Cup/bumper radii and surface dimensions are course pixels. Surface response
uses the ball centre. Overlapping patches use fixed priority: water, sand,
rough, then ice. Rendering uses the same priority, regardless of definition
order. Explicit CourseLayer values put surfaces below rails, then walls, bumpers,
lasers, portals, cup, trails, balls and effects. Alignment guides and their
labels appear above all course objects but remain within the course window,
below interface overlays. Alignment guide distance remains the
unobstructed **normal felt** estimate, not a prediction through patches.

## Authoring rails

Definitions live in `games/minigolf/course_defs.hpp`; layouts live in
`course_test_hole.cpp`. Every bumper, laser, portal and interior wall
has a `rail` index (`-1` means fixed). `CourseHole::cupRail` attaches the cup.
Boundary walls and all surface patches remain fixed; surfaces have no rail field.

A rail has a sequence of `RailStop { offset, pauseSeconds, speed }` values.
Offsets translate the attached element from its base position. Speed is in
pixels/second and applies when leaving that stop, including on the return
journey. `PingPong` traverses the stops then reverses; `Loop` includes the
closing segment from the last stop back to the first. `phaseSeconds` staggers
elements' timing. Sharing one rail index synchronizes attached elements.

```cpp
h.rails.push_back({
    { {{0,0}, 1.0f, 80}, {{200,0}, 0.5f, 140} },
    RailMode::PingPong
});
h.walls.push_back({500, 450, 180, 24, 0});
h.cupRail = 0;
```

Rail paths and pause stops are drawn on the course. Place **the entire path
and the element's extent** within the course window; paths are not automatically
clamped. Rails translate without rotating their attachments. Moving walls and
bumpers use kinematic Box2D bodies; beams, portals and the cup use
the same time-based position calculation for both rendering and interactions.
Moving-cup capture evaluates velocity relative to the cup.

Laser durations are `onSeconds`, `offSeconds`, and optional `phaseSeconds`.
Portal colours identify pairs; there are no authored exit angles.

## Verification

With the GCC/Ninja `app-sim` build configured:

```powershell
python tests/run.py minigolf --build-dir build
python tests/run.py minigolf --build-dir build --render
```

The runner builds the app and links integration checks against its actual
objects/libraries. It checks rails, surface resistance, all-player lasers,
single penalties, respawns, stroke caps, bumper kicks, portals and moving
walls/cup, then steps all nine holes. `--render` additionally writes SDL
software-rendered screenshots to `build/tests/minigolf/renders`.

## Square-tile barriers

Use `addTileBarrier` to build L, T, U, stair-step or other grid shapes.
Each `#` creates one square wall; dots are empty space. Positions refer to
the top-left of the grid, and tile size is in course pixels. The optional
rail index moves every tile together, with one visible rail path.

```cpp
addTileBarrier(h, {300,550}, 40, {"#..", "#..", "###"}, 1);
```

## Multiplayer spawning and returns

Balls first appear when their player's first turn begins on each hole, at
`startPos`. Optional `spawnPositions` assigns exact tee positions round-robin;
there is no multiplayer offset. Later turns keep the ball where it stopped.

A first spawn or hazard return destroys any overlapping ball, with a brief
explosion. Displaced balls respawn after 0.4 seconds at their saved putt-start
positions without another penalty, potentially destroying another ball.

A cascade remembers its initiating ball and every victim, across all delayed
returns. A return that overlaps any participant instead searches for nearby
safe ground. Search uses an 8-pixel grid with local 1-pixel refinement, so the
result approximates the nearest safe position. It excludes walls, bumpers,
water, lasers (even when off), portals, cup, other balls, tees and saved return
positions. Sand, rough and ice are valid. Obstacles are checked at spawn time.
If no safe position exists, retry after 0.4 seconds without forcing a collision.
Turns do not bypass pending delays. Hazard returns retain their one-stroke
penalty; cascading displacement never adds strokes.

## Match settings

- **Ball collisions:** on by default. Off allows overlapping balls and disables
  all spawn/respawn destruction. Walls, bumpers and hazards still work.
- **Holes:** 3, 6 or 9 (default). Range options depend on this selection:
  3 offers 1-3 / 4-6 / 7-9; 6 offers 1-6 / 4-9 / 7-3; 9 offers only 1-9.
  The wrapped range plays 7, 8, 9, 1, 2, 3. Displayed hole numbers retain their
  course numbers; progress shows how many holes of this match have been played.
- **Teams:** Individual, Alternate shot, or Scramble. Team modes use existing
  assignments in Player Settings, including unequal teams and single-member
  teams. Teammates follow their player-list order.

Alternate shot gives each team one ball and score. Teammates rotate after
each set of up to three darts, with collecting early counting unused darts as
misses. The starting teammate advances each hole, as does the starting team.

Scramble gives each teammate a set of up to three darts from the team's shared
position. Completed attempts become stationary numbered result markers, not
additional physical balls. After all teammates finish, choose a result in the
right sidebar using Up/Down and Enter, a gamepad D-pad and A, or a mouse click.
Only the chosen attempt's strokes and penalties enter the team score; a chosen
holed result finishes the hole. The next cycle starts at that selected position.
The choice panel leaves the course visible. Saved results stay fixed while
moving obstacles continue to run.
