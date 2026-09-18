# Starter course catalog

Seven nine-hole starter courses (63 holes), plus the existing Obstacle Sampler.

Choose **Mini Golf > Course** in game setup. To edit a bundled course, open its
`games/minigolf/assets/courses/<course-id>/course.json` in the course editor.
Use Save As to author repository files; opening the installed bundled copy saves
a local override by default. Local overrides take precedence over bundled updates.

Tungsten Ridge, Ochemont, The Big Fish and Whistling Flights now have 36 individually
composed concept layouts. Footprints range from wide, shallow fairways to tall,
narrow climbs; tees and cups change direction with each concept. All retain a
30-pixel cup radius and a shared tee. Pars are provisional editing suggestions.
Checkout Valley, Double Dunes, Treble Beach and Obstacle Sampler were not redesigned.

The layouts use existing mechanics as themed props: mine carts, ferry portals,
clubhouse columns, fishing nets and standing stones. Names describe design intent;
there is no new wind, tide, elevation or spinning-lighthouse simulation. Everything
is editable geometry in the existing course files. Hole IDs remain stable even
where display names changed.

Some holes deliberately depend on portals, notably Three Island Ferry. The layout
check checks swept bounds, stationary clearances and potential ball-sized routes,
including portal connections. It excludes moving obstacles and timed lasers from
route blocking; their timing, real-dart difficulty and pars require playtesting.
The game regression renderer initializes and samples all 63 themed holes.

## Tungsten Ridge

Folder: `tungsten-ridge`. Total provisional par: **40**.

| Hole | Name | Par | Design idea |
| --- | --- | --- | --- |
| 1 | The Split Summit | 4 | A granite chevron splits the ascent into an icy direct face and a longer scree traverse. |
| 2 | Pine Needle Switchbacks | 5 | A tall descent reverses direction three times through alternating granite shelves. |
| 3 | The Abandoned Fork | 4 | A mine forks around a central ore chamber: portals provide a shortcut through the collapsed workings. |
| 4 | Glacier Confluence | 4 | Three diagonal ice streams meet at a bumper boulder; water pools punish missing the glacier forks. |
| 5 | Ore Sorting Yard | 5 | Three ore carts cross the yard on different schedules, with collection bins above and below. |
| 6 | Copper Coil | 5 | A broken square coil winds toward a central cup; a copper tunnel skips one turn, while sparking gaps cycle. |
| 7 | The Blue Crevasse | 4 | A long glacier is split by staggered water fissures; each fissure has a different dry end. |
| 8 | Cable Car Summit | 4 | The summit cup rides a diagonal cable between two stations. A tunnel offers access to the far ridge. |
| 9 | The Tungsten Crown | 5 | A jagged crown surrounds the final cup. Its open lower gate, icy teeth and pulsing survey beams invite bank shots. |


## Checkout Valley

Autumn parkland with generous creek crossings, orchard bumpers and a gradual introduction to gadgets.

Folder: `checkout-valley`. Total par: **31**.

| Hole | Name | Par | Features |
| --- | --- | --- | --- |
| 1 | First Checkout | 3 | rough, sand |
| 2 | Autumn Bend | 3 | rough, sand |
| 3 | Creekside | 3 | water, rough, bumpers |
| 4 | Covered Crossing | 3 | water, sand |
| 5 | Orchard Bounce | 4 | rough, sand, bumpers |
| 6 | Harvest Gate | 4 | sand, lasers, rails |
| 7 | Frosty Morning | 3 | ice, sand, rough |
| 8 | Leaf Tunnel | 4 | water, bumpers, portals |
| 9 | Golden Homecoming | 4 | water, sand, rough, bumpers, lasers, rails |

## Ochemont

Folder: `ochemont`. Total provisional par: **38**.

| Hole | Name | Par | Design idea |
| --- | --- | --- | --- |
| 1 | The Clubhouse Courtyard | 4 | A grand clubhouse block forces a choice of side verandas; sand terraces flank the entrance. |
| 2 | The Ancient Oak | 4 | One enormous oak grove dominates the lawn: a ring of trunk bumpers surrounds a rough root plate. |
| 3 | Bunker Archipelago | 5 | Diagonal fingers of deep sand form a chain of green pockets; the short diagonal cuts across every bunker. |
| 4 | Members Billiard Room | 3 | A compact symmetrical bank-shot salon with a triangular bumper rack and angled cushions. |
| 5 | The Long Colonnade | 4 | A narrow portrait hole threads a ceremonial arcade; staggered laser doors alternate between the columns. |
| 6 | Winter Croquet | 4 | An icy croquet lawn offers three offset hoops, with sand catchers beyond each opening. |
| 7 | Groundskeepers on Parade | 4 | Three mower bars slide across separate strips of rough, leaving changing diagonal routes. |
| 8 | The Island Green | 5 | A rectangular ornamental moat surrounds the island green. A narrow bottom causeway competes with a members-only portal. |
| 9 | The Championship Amphitheatre | 5 | Terraced grandstands wrap a central stage. The cup slowly traverses the stage while two ceremonial gates flash. |


## Treble Beach

Coastal water, cliff corridors and cave portals, with a moving-cup headland finish.

Folder: `treble-beach`. Total par: **34**.

| Hole | Name | Par | Features |
| --- | --- | --- | --- |
| 1 | Sunny Approach | 3 | sand, water |
| 2 | Cliff Walk | 4 | water, sand |
| 3 | Surf Bridge | 3 | water, sand, bumpers |
| 4 | Sea Caves | 3 | water, rough, portals |
| 5 | Tidal Skim | 4 | ice, sand, water, bumpers |
| 6 | Beach Patrol | 4 | sand, bumpers, lasers, rails |
| 7 | Cove Route | 4 | water, sand, portals |
| 8 | Driftwood Crossing | 4 | water, sand, bumpers, rails |
| 9 | Headland Finish | 5 | water, sand, lasers, portals, rails, moving cup |

## The Big Fish

Folder: `the-big-fish`. Total provisional par: **41**.

| Hole | Name | Par | Design idea |
| --- | --- | --- | --- |
| 1 | Through the Fish | 4 | A giant fish skeleton becomes the fairway: tilted ribs and a tail funnel frame an open central spine. |
| 2 | Dock Fingers | 5 | Water-filled berths alternate from opposite banks, making a walk around a working fishing harbor. |
| 3 | Buoy Regatta | 4 | A portrait slalom of moving buoys on independent cross-channel tracks. |
| 4 | Three Island Ferry | 5 | Three land masses are separated by full water channels. Two colored ferry portals make island hopping mandatory. |
| 5 | Lighthouse Courtyard | 5 | Broken concentric sea walls curl around a lighthouse cup; timed beams guard two entrances. |
| 6 | The Ice Cannery | 4 | Fish slide along an icy packing line past staggered crusher bumpers and a moving loading gate. |
| 7 | Shipwreck Causeway | 5 | A tilted shipwreck splits the beach into bow and stern routes; broken ribs interrupt the sand-filled hull. |
| 8 | Fishing Net Gauntlet | 4 | A diagonal net of short timed laser strands opens in waves, with moving mooring posts along its edges. |
| 9 | The Last Whirlpool | 5 | Four quarter-pools suggest a whirlpool. Paired colored currents lead inward, and the cup circles the quiet eye. |


## Whistling Flights

Folder: `whistling-flights`. Total provisional par: **39**.

| Hole | Name | Par | Design idea |
| --- | --- | --- | --- |
| 1 | The Exposed Spine | 4 | A diagonal exposed ridge cuts through broad marram banks: ice makes the direct route fast, rough makes the wide route deliberate. |
| 2 | The Standing Circle | 3 | A compact stone circle has unequal gaps and two inner monoliths; enter from any side. |
| 3 | The Marram Labyrinth | 4 | Thick grass forms an open-ended maze. Cutting straight through is possible, but its friction makes every shortcut costly. |
| 4 | The Broken Chapel | 5 | A roofless chapel sits diagonally across the links, with broken nave walls, an open doorway and a sandy altar. |
| 5 | Frost Runnels | 4 | Alternating diagonal ice and rough runnels create a speed-control puzzle, without repeating the water-crossing layout. |
| 6 | Storm Signal Station | 5 | Three diagonal signal beams guard the climb to a bluff-top station, with stone shelters between them. |
| 7 | The Horseshoe Bluff | 5 | A long sea inlet forces a horseshoe journey around the bluff, with sand at the turning point and a risky portal shortcut. |
| 8 | The Kites | 4 | Two portal mouths trace different kite-shaped loops above grass banks; their moving entries alter the exit momentum. |
| 9 | The Compass Rose | 5 | A radial compass rose replaces a conventional finishing fairway: short stone spokes, sand quadrants and one icy approach. |


## Double Dunes

Sandstone bends, oasis crossings and desert portals, ending with a choice of shortcut or longer dry route.

Folder: `double-dunes`. Total par: **34**.

| Hole | Name | Par | Features |
| --- | --- | --- | --- |
| 1 | Desert Welcome | 3 | sand |
| 2 | Sandstone Bend | 4 | sand, rough |
| 3 | Oasis Crossing | 3 | water, sand, bumpers |
| 4 | Mirage Tunnels | 4 | sand, portals |
| 5 | Canyon Echo | 4 | sand, bumpers |
| 6 | Cold Desert Night | 3 | ice, sand |
| 7 | Sunbeam Gate | 4 | sand, lasers, portals |
| 8 | Shifting Stone | 4 | sand, water, bumpers, rails |
| 9 | Double or Nothing | 5 | sand, water, bumpers, lasers, portals, rails, moving cup |
