# Tests

Tests live beside the code they exercise. This directory owns the shared runner
and support code; `scripts/` is for release and asset tools.

```text
tests/
  run.py                         Single entry point
  support/build.py               Shared compile/link adapter and isolated output
  support/process.py             Process execution and captured logs
games/minigolf/tests/
  course_io_test.cpp              File migration, validation and save/load
  physics_test.cpp
  obstacles_test.cpp
  spawning_test.cpp
  turns_test.cpp
  settings_test.cpp
  team_play_test.cpp
  rendering_test.cpp
  fixtures/golf_fixture.hpp       Shared Mini Golf setup
  test_main.cpp
  suite.py
games/minigolf/editor/tests/
  document_test.cpp              Editing, saving and shared preview physics
  suite.py                       Also drives editor mouse/render smoke checks
game_lib/tests/
  turn_skipping_test.cpp
  suite.py
debug/tests/
  logging/{durability_test.cpp,suite.py}
  crash_reporting/{capture_test.cpp,suite.py}
```

Reserve `tests/integration/<suite>/` for tests spanning modules without a clear
owner. Game-specific integration tests still belong to their game. Playable
course content lives in `games/minigolf/assets/courses/`; the old C++ layout is now
a migration fixture under Mini Golf tests.

## Running

Use Python 3.9+ and an already configured GCC-compatible Ninja application build.
The runner rebuilds the application before compiling suites against its objects.
From the repository root:

```sh
python tests/run.py --list
python tests/run.py
python tests/run.py minigolf --render
python tests/run.py minigolf/editor --render
python tests/run.py debug
python tests/run.py game_lib/turn_skipping --build-dir build
python tests/run.py --render --build-dir build
```

`--render` adds headless SDL software rendering checks and screenshots for suites
that support them. A failed suite produces a nonzero exit status; other selected
suites still run.

Executables, object files, process logs, synthetic crash dumps, and screenshots
stay under `<build-dir>/tests/<suite>/`. Portable data files isolate test logs
from the game's logs and crash reports. Mini Golf screenshots are in
`<build-dir>/tests/minigolf/renders/`. Generated output is ignored by Git.

## Adding tests

Add behavior tests to the owning module's `tests/` directory, grouped by topic.
Keep shared fixtures within that suite until another owner actually needs them.
Each `suite.py` declares a unique `NAME` and `run(context, render=False)` function.
Use `context.executable(...)` and `context.run(...)` for build and process work.
The runner discovers game suites, module suites and their immediate sub-suites,
and cross-module integration suites.

Production CMake source globs do not include these nested test directories.
The shared build adapter currently reuses the application's GCC/Ninja commands;
dedicated CMake test targets can replace that adapter later without relocating
tests again. Mini Golf alone enables private-member access for its fixtures.
