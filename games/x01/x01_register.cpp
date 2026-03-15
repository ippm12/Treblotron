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
        desc.settings.push_back({
            "Out Rule",
            {{"Double"}, {"Master"}, {"Any"}},
            0  // default = Double
        });
        desc.settings.push_back({
            "In Rule",
            {{"Any"}, {"Double"}, {"Master"}},
            0  // default = Any
        });
        desc.createGame = [](const std::vector<size_t>& choices) -> GamePtr {
            static const X01Variant variants[] = {
                X01Variant::V301, X01Variant::V501,
                X01Variant::V701, X01Variant::V1001
            };
            static const X01InOutRule inOutRules[] = {
                X01InOutRule::Any, X01InOutRule::Double, X01InOutRule::Master
            };

            size_t varIdx = (choices.size() < 1 || choices[0] >= 4) ? 1 : choices[0];
            // Out Rule: Double(0), Master(1), Any(2) — map to enum order
            size_t outIdx = (choices.size() < 2 || choices[1] >= 3) ? 0 : choices[1];
            // In Rule: Any(0), Double(1), Master(2) — already matches enum order
            size_t inIdx  = (choices.size() < 3 || choices[2] >= 3) ? 0 : choices[2];

            // Out Rule options are ordered Double, Master, Any
            static const X01InOutRule outRules[] = {
                X01InOutRule::Double, X01InOutRule::Master, X01InOutRule::Any
            };

            return std::make_shared<X01Game>(variants[varIdx], outRules[outIdx], inOutRules[inIdx]);
        };
        registerGame(desc);
    }
} s_x01Registrar;
