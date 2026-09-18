# Mini Golf module

All golf-owned source, courses, editor tools, tests, documentation, and packaging
rules live here. The host discovers game sources and module CMake files without
golf-specific entries in the root build configuration.

```text
*.cpp, *.hpp     Gameplay, rendering, physics, course persistence and registration
CMakeLists.txt  Module assets, installation and optional editor target
assets/courses/ Bundled, versioned JSON courses
editor/         SDL desktop course editor
editor/tests/   Editor model and interactive smoke tests
tests/          Gameplay, rendering and course persistence tests
docs/           Course format, editor controls and gameplay documentation
edit_courses.ps1  Build and launch the editor
```

From the repository root:

```powershell
./games/minigolf/edit_courses.ps1
python tests/run.py minigolf --render
```

The test command runs both gameplay and editor suites. Use `minigolf/editor` to
select only editor tests. Configure `TREBLOTRON_BUILD_COURSE_EDITOR=OFF` to omit
the editor executable.

The module depends on the host's shared game/player interfaces, rendering,
physics, logging, paths, and third-party libraries. It is a module of Treblotron,
not an independently buildable engine. The editor links the shared application
runtime but does not start cameras or dart detection. Shared build/test machinery
remains at repository level.

Bundled data is staged and installed at `assets/games/minigolf/courses` beside
the executable. Existing writable `courses/` saves retain their location and
override bundled courses as before. Old staged `assets/courses` files are no
longer used; builds do not delete potentially edited files there.

See [editor controls](docs/COURSE_EDITOR.md), [course file format](docs/MINIGOLF_FILES.md),
and [gameplay documentation](docs/MINIGOLF_COURSES.md).

See the [starter course catalog](docs/COURSE_CATALOG.md) for all seven themed courses and their hole lists.
