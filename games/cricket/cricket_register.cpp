/**
 * cricket_register.cpp
 *
 * Auto-registers the Cricket game with the game registry via static
 * initialization. No manual registration call needed — just compile this file.
 */

#include "game_lib/game_registry.hpp"
#include "cricket.hpp"

#include <memory>


static struct CricketRegistrar
{
    CricketRegistrar()
    {
        GameDescriptor desc;
        desc.name        = "Cricket";
        desc.description = "Close numbers and outscore opponents";
        desc.maxPlayers  = 6;
        desc.settings.push_back({
            "Scoring",
            {{"Standard"}, {"Cut-throat"}, {"No Score"}},
            0  // default = Standard
        });
        desc.settings.push_back({
            "Numbers",
            {{"Standard (15-20)"}, {"Random"}},
            0  // default = Standard
        });
        desc.settings.push_back({
            "Teams",
            {{"Off"}, {"On"}},
            0  // default = Off
        });
        desc.createGame = [](const std::vector<size_t>& choices) -> GamePtr {
            static const CricketScoring scoringModes[] = {
                CricketScoring::Standard,
                CricketScoring::CutThroat,
                CricketScoring::NoScore
            };

            size_t scoringIdx = (choices.size() < 1 || choices[0] >= 3) ? 0 : choices[0];
            bool randomNums   = (choices.size() >= 2 && choices[1] == 1);
            bool teamsMode    = (choices.size() >= 3 && choices[2] == 1);

            return std::make_shared<CricketGame>(scoringModes[scoringIdx], randomNums, teamsMode);
        };
        registerGame(desc);
    }
} s_cricketRegistrar;
