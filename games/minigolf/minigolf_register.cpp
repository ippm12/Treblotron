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
        desc.description = "Individual or team putting — angle from bullseye aims, "
                           "distance from bullseye sets power";
        desc.maxPlayers  = MiniGolf::MAX_PLAYERS;

        desc.settings={
            {"Course",{{"Obstacle Sampler"}},0},
            {"Ball collisions",{{"On"},{"Off"}},0},
            {"Holes",{{"3"},{"6"},{"9"}},2},
            {"Hole range",{{"1-9"}},0},
            {"Teams",{{"Individual"},{"Alternate shot"},{"Scramble"}},0}
        };
        desc.updateSettings=[](std::vector<GameSetting>& settings,std::vector<size_t>& choices) {
            if(choices.size()!=settings.size()) {
                choices.clear();
                for(const auto& setting:settings) choices.push_back(setting.defaultIndex);
            }
            if(choices[2]>2) choices[2]=2;
            if(choices[2]==0) settings[3].options={{"1-3"},{"4-6"},{"7-9"}};
            else if(choices[2]==1) settings[3].options={{"1-6"},{"4-9"},{"7-3"}};
            else settings[3].options={{"1-9"}};
            for(size_t i=0;i<settings.size();++i)
                if(choices[i]>=settings[i].options.size()) choices[i]=0;
        };
        desc.createGame = [](const std::vector<size_t>& choices) -> GamePtr {
            MiniGolf::GameOptions options;
            options.ballCollisions=choices.size()<2 || choices[1]!=1;
            options.holeCount=choices.size()>2 && choices[2]<3 ? static_cast<uint8_t>((choices[2]+1)*3) : 9;
            options.startHole=options.holeCount<9 && choices.size()>3 && choices[3]<3 ? static_cast<uint8_t>(choices[3]*3):0;
            options.teams=choices.size()>4 && choices[4]<3 ? static_cast<MiniGolf::TeamMode>(choices[4]):MiniGolf::TeamMode::Individual;
            return std::make_shared<MiniGolf::MiniGolfGame>(MiniGolf::CourseId::TestHole,options);
        };
        registerGame(desc);
    }
} s_miniGolfRegistrar;
