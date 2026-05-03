/**
 * minigolf_register.cpp
 *
 * Registers the Mini Golf game with the game registry. Auto-discovered
 * by the games subdirectory glob in the top-level CMakeLists.txt.
 */

#include "game_lib/game_registry.hpp"
#include "minigolf.hpp"

#include <memory>


static struct MiniGolfRegistrar
{
    MiniGolfRegistrar()
    {
        GameDescriptor desc;
        desc.name        = "Mini Golf";
        desc.description = "9-hole putting — angle from bullseye aims, "
                           "distance from bullseye sets power";
        desc.maxPlayers  = MiniGolf::MAX_PLAYERS;

        // Only one course for now. Adding a course is a new entry here
        // plus a new case in MiniGolf::buildCourse().
        desc.settings.push_back({
            "Course",
            { { "Test Hole" } },
            0  // default
        });

        desc.createGame = [](const std::vector<size_t>& choices) -> GamePtr {
            MiniGolf::CourseId id = MiniGolf::CourseId::TestHole;
            if(!choices.empty())
            {
                // Each menu index maps to a CourseId. Out-of-range
                // selections fall back to TestHole.
                switch(choices[0])
                {
                    case 0: default: id = MiniGolf::CourseId::TestHole; break;
                }
            }
            return std::make_shared<MiniGolf::MiniGolfGame>(id);
        };

        registerGame(desc);
    }
} s_miniGolfRegistrar;
