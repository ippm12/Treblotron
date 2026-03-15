/**
 * x01_register.cpp
 *
 * Auto-registers the X01 game with the game registry via static initialization.
 * No manual registration call needed — just compile this file.
 */

#include "game_lib/game_registry.hpp"
#include "x01.hpp"

#include <memory>


static struct X01Registrar
{
    X01Registrar()
    {
        GameDescriptor desc;
        desc.name        = "X01";
        desc.description = "Count down to zero";
        desc.maxPlayers  = 6;
        desc.settings.push_back({
            "Starting Score",
            {{"301"}, {"501"}, {"701"}, {"1001"}},
            1  // default = 501
        });
        desc.createGame = [](const std::vector<size_t>& choices) -> GamePtr {
            static const X01Variant variants[] = {
                X01Variant::V301, X01Variant::V501,
                X01Variant::V701, X01Variant::V1001
            };
            size_t idx = (choices.empty() || choices[0] >= 4) ? 1 : choices[0];
            return std::make_shared<X01Game>(variants[idx]);
        };
        registerGame(desc);
    }
} s_x01Registrar;
