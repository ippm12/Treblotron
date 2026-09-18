#pragma once
#include "course_defs.hpp"
#include <stdexcept>

namespace MiniGolf {
// Cosmetic only. Surface identity, portal pair colors and physics stay unchanged.
struct CourseTheme {
    const char* id;
    const char* name;
    Color grass, wall, backdrop, foliage, accent, sand, sandDetail, rough, roughDetail, water, waterDetail;
};
inline const std::array<CourseTheme,8> COURSE_THEMES = {{
    {"classic", "Classic", {35,110,55},{100,70,40},{35,110,55},{35,78,35},{230,220,180},{190,160,85},{225,200,125},{35,78,35},{65,120,55},{30,100,170},{85,180,230}},
    {"tungsten-ridge", "Tungsten Ridge", {57,103,77},{105,117,125},{48,62,69},{24,65,55},{190,139,83},{174,157,117},{211,197,163},{38,71,52},{69,102,69},{43,112,143},{118,179,192}},
    {"checkout-valley", "Checkout Valley", {102,137,65},{133,85,50},{79,98,53},{173,85,42},{232,180,70},{203,174,112},{236,208,149},{66,96,42},{108,127,58},{52,127,159},{117,189,205}},
    {"ochemont", "Ochemont", {48,119,65},{161,150,119},{36,72,48},{25,77,42},{188,156,90},{217,202,157},{242,228,192},{28,74,38},{59,108,48},{39,105,142},{102,167,189}},
    {"treble-beach", "Treble Beach", {71,142,99},{206,177,124},{34,124,157},{45,105,76},{245,225,171},{225,196,125},{250,229,176},{37,96,62},{79,140,86},{27,133,175},{121,221,226}},
    {"the-big-fish", "The Big Fish", {61,120,91},{140,110,78},{28,68,102},{35,88,67},{231,190,92},{188,167,117},{222,207,160},{37,79,54},{73,116,73},{29,92,147},{97,167,204}},
    {"whistling-flights", "Whistling Flights", {111,132,77},{139,139,123},{76,93,99},{91,105,54},{203,191,137},{196,181,135},{226,213,172},{73,87,41},{122,131,69},{53,107,138},{137,177,194}},
    {"double-dunes", "Double Dunes", {80,133,91},{175,91,57},{203,160,100},{74,105,65},{238,196,120},{213,172,102},{243,208,142},{50,90,49},{100,128,65},{32,140,163},{124,218,221}}
}};
inline const CourseTheme& courseTheme(const std::string& id) {
    for(const auto& theme:COURSE_THEMES) if(id==theme.id) return theme;
    throw std::runtime_error("Unknown course theme: "+id);
}
}
