# Course themes

Themes are flat, top-down cosmetic presets. Choose one in the editor's course
inspector, then save normally. All seven presets now have [nine-hole starter courses](COURSE_CATALOG.md).
The original Obstacle Sampler remains available for mechanic experiments.

| Preset / stable ID | Palette and border scenery | Direction for future layouts |
| --- | --- | --- |
| Tungsten Ridge / `tungsten-ridge` | Slate walls, copper, dark pines, mine entrances | Granite passes, mine-cart rails, industrial lasers |
| Checkout Valley / `checkout-valley` | Warm green, autumn crowns, timber | Gentle creek crossings, bridges, approachable shortcuts |
| Ochemont / `ochemont` | Deep green oaks, pale sand, warm stone | Strategic bunkers, estate grounds, championship finish |
| Treble Beach / `treble-beach` | Turquoise coast, golden rocks, surf marks | Cliffside fairways, coves, headland finish |
| The Big Fish / `the-big-fish` | Navy harbor, timber piers, buoys, lighthouse symbols | Docks, moving harbor obstacles, island greens |
| Whistling Flights / `whistling-flights` | Muted olive, weathered stone, tall grasses | Exposed links, stone banks, deep bunker approaches |
| Double Dunes / `double-dunes` | Red sandstone, gold sand, cacti, turquoise water | Canyon passages, oasis routes, sandy shortcuts |

`classic` preserves the original appearance. Palettes live in
`course_themes.hpp`; shared rendering is in `minigolf.cpp` and
`course_obstacles.cpp`. These use ordinary renderer shapes, with no bitmap assets,
lighting, shaders, or texture downloads.

Scenery is automatically arranged outside the boundary, has no collision, and
stays below gameplay objects. Leave approximately 100 course pixels of visible
space outside the bounds when composing new holes. On courses filling the whole
viewport the palette remains visible but border scenery may be offscreen.
The editor's zoom lets you inspect it. Scenery placement is currently automatic;
individual trees/buildings are not yet selectable editor objects.

Sand, rough and water get matching colors while retaining their existing
identifying patterns. Ice remains pale blue. Lasers, bumpers and portal-pair
colors retain their gameplay cues. All mechanics remain available in all themes;
this adds no wind, slopes, collision bodies or friction changes. Decorative
coastal/harbor backdrops are outside the enclosing wall; playable water hazards
must still be placed explicitly.

The current pass provides palettes and simple border motifs. Larger landmarks
such as a clubhouse, mountains and offshore islands belong to the next scenery
and layout pass; they are not simulated terrain or completed holes.
