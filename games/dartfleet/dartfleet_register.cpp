/**
 * dartfleet_register.cpp
 *
 * Registers the Dartfleet game with the game registry via static
 * initialization. Picked up automatically by the games/ auto-discovery
 * in the top-level CMakeLists.txt.
 */

#include "game_lib/game_registry.hpp"
#include "dartfleet.hpp"

#include <memory>


static struct DartfleetRegistrar
{
    DartfleetRegistrar()
    {
        GameDescriptor desc;
        desc.name        = "Dartfleet";
        desc.description = "Place a hidden fleet of ships and sink the opposing one";
        desc.maxPlayers  = 6;  // up to 3 per team in teams mode

        desc.settings.push_back({
            "Teams",
            {{"Off"}, {"On"}},
            0  // default = Off (1 player vs 1 player)
        });
        desc.settings.push_back({
            "Team 1 Ship Size",
            {{"Small"}, {"Default"}, {"Large"}},
            1  // default = Default
        });
        desc.settings.push_back({
            "Team 2 Ship Size",
            {{"Small"}, {"Default"}, {"Large"}},
            1  // default = Default
        });
        desc.settings.push_back({
            "Volley Size",
            {{"Salvo (ships left)"}, {"3"}, {"2"}, {"1"}},
            0  // default = Salvo
        });

        desc.createGame = [](const std::vector<size_t>& choices) -> GamePtr {
            bool teamsMode = (choices.size() >= 1 && choices[0] == 1);

            auto pickSize = [](size_t idx) -> FleetSize {
                if(idx == 0) return FleetSize::Small;
                if(idx == 2) return FleetSize::Large;
                return FleetSize::Default;
            };

            auto pickVolley = [](size_t idx) -> VolleySize {
                if(idx == 1) return VolleySize::Three;
                if(idx == 2) return VolleySize::Two;
                if(idx == 3) return VolleySize::One;
                return VolleySize::Salvo;
            };

            FleetSize  team0Size  = (choices.size() >= 2) ? pickSize(choices[1])   : FleetSize::Default;
            FleetSize  team1Size  = (choices.size() >= 3) ? pickSize(choices[2])   : FleetSize::Default;
            VolleySize volleySize = (choices.size() >= 4) ? pickVolley(choices[3]) : VolleySize::Salvo;

            return std::make_shared<DartfleetGame>(teamsMode, team0Size, team1Size, volleySize);
        };
        registerGame(desc);
    }
} s_dartfleetRegistrar;
