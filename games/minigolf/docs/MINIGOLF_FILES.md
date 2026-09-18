# Mini Golf course files

Use the [desktop Course Editor](COURSE_EDITOR.md) to create and edit these files.

Default and custom courses use the same JSON format and C++ save/load API.
The bundled sampler is `games/minigolf/assets/courses/obstacle-sampler/`:

```text
course.json                 Course ID, name, formatVersion, ordered hole IDs
holes/surfaces.json          One hole per stable ID
holes/gadgets.json
...
```

The manifest's hole IDs resolve to `holes/<id>.json`. IDs allow letters, digits,
hyphens and underscores, up to 80 characters. They are identities, not labels:
renaming a hole should change its name, not its ID or filename. Current match
logic requires exactly nine holes; the existing 3/6/9-hole ranges select from
that course. Partial drafts are not yet a separate supported format.

## Storage and discovery

Bundled courses ship under `<executable>/assets/games/minigolf/courses/`. Writable courses live
under `appDataPath("courses")`: beside the executable in portable builds, or
under the application's user data directory in installed builds. Each course
has its own directory. Local directories take precedence over bundled ones of
the same directory name. A local override is a complete course, in the same
format, not a patch. Updates do not merge or overwrite these local copies.

The course menu discovers these directories when its settings refresh. Invalid
courses remain selectable so starting one reports the file error instead of
crashing or silently using another course. Starting a match reloads its files;
editing files does not mutate a match already in progress.

For development, edit the repository's `games/minigolf/assets/courses` files for Git diffs.
The app build stages them even if no C++ recompilation is necessary. Installer
rules include the same files. Changes made to a staged bundled copy can be
overwritten by the next build; the editor should save to a selected source
directory or the writable course directory.

## Hole data

Each hole contains its own `formatVersion`, ID, name, par, bounds, primary tee,
extra tees, cup, rails, walls, surfaces, bumpers, lasers and portals. Positions,
sizes and offsets are two-number arrays in course pixels. Timing uses seconds;
speed uses pixels per second. Outer boundary walls are derived from bounds.

Hole format 2 adds optional `angleDegrees` to walls, surfaces and lasers, defaulting
to zero. Positive angles rotate clockwise around the object center. Saves omit
zero angles and retain hole format 1 when no rotated objects are present, avoiding
unnecessary diffs. Both versions load directly; older readers reject version 2.
Course manifest versions are independent of hole versions. Lasers retain `center`
and `size` in files, with the editor creating a thin beam between two endpoints;
rotation is applied consistently to rendering, physics and hazard checks.

- Object IDs are unique within a hole; waypoint IDs are unique within a rail.
  Primary tee and cup have fixed IDs `tee` and `cup`.
- Rail references are stable string IDs, or an empty string for stationary
  objects. Rails contain ordered stops and use `loop` or `pingPong` mode.
- Walls retain individual boxes and a `group` ID for editing compound square
  barriers together. An empty group means independent. Group members share a
  rail. `showRail` is an integer flag, 0 or 1, to draw the shared path once.
- Surfaces use `rough`, `sand`, `ice`, or `water` and cannot have rail fields.
- Portal `pair` numbers remain stable within the hole; each pair must have two
  members with matching RGB colors. Portals use cup radius and have no angle.

The sampler files are complete examples of all supported fields. Unknown fields
and unsupported versions are rejected, preventing older saves from silently
discarding newer data. Future format changes need an explicit migration.

## Shared editor API

`games/minigolf/course_io.hpp` provides:

```cpp
auto course = MiniGolf::loadCourse(directory);
// Change names, geometry, timing, etc. Retain IDs on existing objects.
MiniGolf::assignCourseIds(course); // Assign IDs to newly created objects only.
MiniGolf::validateCourse(course);
MiniGolf::saveCourse(course, directory);
```

`availableCourses()` returns names and resolved directories for the menu/editor.
The runtime structs own their strings. Rail references in memory remain vector
indices for gameplay; an editor that reorders/deletes rails must remap those
indices. Disk references always use IDs. Preserve object array ordering during
ordinary edits; waypoint, tee and hole order have gameplay meaning.

Saves validate the entire course before writing, serialize consistently, round
numbers to six decimal places, and leave byte-identical files untouched. Positive
dimensions and speeds must be at least 0.000001 so rounding cannot turn them
into zero. Each
changed file is written to a sibling temporary file and then replaced; the
manifest is written last. This protects individual files from interrupted writes,
but is not an all-or-nothing transaction across the whole course. Saves to the
same directory must be serialized by the editor. Unreferenced hole files are
retained, allowing undo/recovery rather than deleting author work automatically.

Validation checks IDs, references, enums, finite/bounded numbers, dimensions,
timings and portal pairs. It does not prove a hole is playable or restrict larger
course bounds. The editor provides autosave recovery and motion/playtesting tools; geometric
solvability checks and partial-course draft files are not implemented.

## Migration verification

The original C++ sampler is retained only as a test fixture, not linked into
gameplay. Tests compare its full serialized data with the shipped files, check
save/load stability and isolated edits, reject invalid data, and run the usual
physics and rendering suites against the file-loaded course:

```sh
python tests/run.py minigolf --render
```

Intentional default-course edits must update the migration expectation or replace
that historical comparison with a reviewed golden fixture. The test executable's
`--export-course <directory>` option exports the historical fixture for migration
verification; normal course authoring uses the shared save API.

## Theme manifests (version 2)

Classic courses keep their version-1 manifest unchanged. A non-classic theme
uses a version-2 `course.json` with an additional required `theme` string, for
example `"theme": "tungsten-ridge"`. Hole files remain version 1 and are not
rewritten when only the theme changes. Both manifest versions load in the game
and editor. Unknown theme IDs and unsupported versions fail explicitly.
Older game builds reject version 2 instead of silently discarding the theme.

[Theme presets](THEMES.md) are shared by gameplay and the editor. Selecting
Classic again saves a version-1 manifest. Existing local overrides still take
precedence, including their theme selection.
