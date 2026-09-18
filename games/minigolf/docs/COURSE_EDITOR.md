# Mini Golf Course Editor

The editor is a separate SDL3 desktop application. It shares compiled course
rendering, physics and JSON persistence code with the game. It does not initialize
cameras, dart detection, or the match manager.

## Launch

From the repository root:

```powershell
./games/minigolf/edit_courses.ps1
```

The launcher builds the editor and stages course assets before opening it, so it
will not silently run an old build. Use `-BuildDir build-app-sim` for another
configured application build. You can also run `build/bin/TreblotronCourseEditor.exe`
directly after building, or launch the installed `TreblotronCourseEditor.exe`.

```powershell
cmake --build build --target TreblotronCourseEditor --parallel 4
```

The `TREBLOTRON_BUILD_COURSE_EDITOR` CMake option defaults to ON for app builds.

## Create, open and save

- **New** creates a valid nine-hole course with empty layouts. Name the course and
  current hole in the property panel with nothing selected (Escape clears selection).
- **Open** chooses a `course.json` manifest through the native file dialog.
- **Save** on a bundled course writes a complete local copy under the game's
  writable `courses/<folder>/` directory. In this build that is `build/bin/courses/`.
  The game discovers it as an override of the bundled course.
- **Save As** chooses the exact course folder to write. To edit version-controlled
  defaults, choose `games/minigolf/assets/courses/obstacle-sampler/` in the repository. Existing
  courses that were not the source of this document require overwrite confirmation.
- Editing an already-local course saves back to its source folder.
- Dirty documents show `*` in the title. New/Open/Close prompt to save or discard.
  A recovery copy is written every 30 seconds while dirty. **Recover** restores it
  as an unsaved document; normal saving does not depend on autosave.

All saves use the [shared JSON format](MINIGOLF_FILES.md): stable object IDs,
unchanged-file preservation, and per-file temporary-file replacement. Publishing
still uses nine holes, with the game's existing 3/6/9-hole range selection.

## Canvas tools

- **Select / move:** click and drag. Shift-click adds objects. Clicking a grouped
  wall selects its whole group. Arrow keys nudge the selection.
- **Paint wall tiles:** drag to paint connected 40-pixel squares. One stroke makes
  one undoable barrier group. Use the inspector to group or ungroup selected walls.
- **Sand / ice / rough / water:** drag out a rectangle. Select a rectangle
  and drag its lower-right blue handle to resize; exact dimensions are also editable.
- **Laser beam:** drag from one endpoint to the other, at any angle. In Select / move,
  drag either blue endpoint to change direction and length while keeping the other
  end fixed. The inspector controls length, beam thickness, angle and timing.
- **Rotation:** select a wall, surface or laser and drag its yellow **Rotate** handle,
  or enter **Angle (degrees)** in the inspector. Angles increase clockwise; zero is
  horizontal. Grouped walls rotate together around the first selected wall, preserving
  their shape. Rotated resize handles follow the object's local axes. Circular
  objects have no direction and do not need rotation. Rails still translate objects;
  these angles set orientation, not animated spinning.
- **Bumper, tee, cup, extra tee:** click to place. There is always one primary tee
  and one cup; these tools relocate them. Extra tees retain their assignment order.
- **Portal pair:** click twice for the two ends. Escape/right-click cancels the
  pending pair. Color properties change both ends. Deleting or duplicating a portal
  operates on its pair, keeping files valid.
- Mouse wheel over the course zooms; middle-drag pans; **Fit** or F fits the hole.
  Grid toggles both visible grid points and 20-pixel snapping. Painted walls always
  use their own 40-pixel tile grid.
- Surface locking prevents accidental selection. Surface/rail visibility controls
  are editing aids only; playtesting displays the full course.

Ctrl+D duplicates selected obstacles, Delete removes them, Ctrl+Z/Ctrl+Y undo/redo.
Undo history keeps 100 edits. Portal and wall-group IDs remain independent when
copied. Reordering holes changes the manifest order; their IDs stay the same.

## Properties and rails

Click a field, type a value, and press Enter or click elsewhere to commit. Escape
cancels the field edit. Invalid values are rejected with a message at the bottom.
Scroll over the property panel to reach additional controls.

**Select / move** prioritizes objects, including an object overlapping its first
waypoint. Moving an object moves its entire path along with it.

**Rail / waypoints** reveals path handles and pauses motion. Click any path or
numbered handle to select its rail, then drag a handle to move that waypoint.
Handle hit areas stay the same size on screen at every zoom level. The rail tool
prioritizes handles where they overlap objects; use Select / move for the object.

To create a rail, select a cup, wall, bumper, laser or portal, choose the rail tool,
then click empty space for its second point. The first point starts at the object's
original position. Keep clicking empty space to add points, then right-click to
finish. For an existing rail, explicitly enable **Add points on canvas** in the
inspector; **Finish adding points** ends placement. Handles remain draggable while
adding points. **Create and attach new rail** also creates a two-point path that
you can immediately reshape.

**Attach existing rail...** lists rails by number and point count; **Detach from
rail** removes the selected object's attachment. Shared rails apply the same
relative offsets to each attached object's original position. Editing a shared
waypoint affects every attached object. Surfaces and tees cannot attach.
The inspector edits waypoint offsets, pauses, departure speeds, loop/back-and-forth
mode, and phase. Deleting a rail safely detaches/remaps its references.

**Run motion**, **Pause motion**, **0 sec**, and **+0.5 sec** preview timing without a
ball. Pause before editing geometry. This preview never changes saved positions.

## Playtesting

Click **Playtest**, then drag backward and release anywhere on the course to putt
in the opposite direction. Longer drags produce stronger shots, capped at the
game's maximum speed. Right-click places the test ball at a chosen location.
**Reset** returns it to the tee. **Stop test** returns to the authored layout.

Physics, rolling appearance, surfaces, moving obstacles, portals, water/laser
resets, trails and cup interactions use the game's implementation. Test mode
bypasses match turns and allows repeated attempts; actual dart input is not enabled.

**Validate** checks the file schema and references. It does not prove that a hole is
solvable or that every moving obstacle stays inside the boundary; use playtesting
and motion preview for these checks. Normal selection targets authored base positions.

## Verification and code ownership

```powershell
python tests/run.py minigolf/editor --render
```

Tests live beside the editor in `games/minigolf/editor/tests/`. They cover history,
rollback on invalid edits, groups, portal pairs, rail reference remapping, local
save destinations and shared physics. The rendering smoke test drives the mouse
handlers for placement, resizing, painting, undo, portal creation and putting,
then writes a screenshot and a sample course under `build/tests/minigolf/editor/`.

`games/minigolf/course_preview.*` provides the preview adapter. Match updates and
editor previews both call the same physics step. `TreblotronRuntime` is an object
library linked into both executables; the editor does not maintain a physics copy.


## Course themes

Deselect objects to show **COURSE & HOLE** in the inspector. Click **Theme** to
cycle through Classic and the seven new course themes. The preview updates
immediately; theme changes support undo/redo, autosave, Save and Save As.
A theme applies to all nine holes. It does not change the course's name.

See [theme presets](THEMES.md). These are visual presets, not populated courses.
