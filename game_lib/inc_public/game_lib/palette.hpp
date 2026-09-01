/**
 * @file palette.hpp
 * @brief The application's colour system, in one place.
 *
 * Derived from the app icon (assets/branding/icon.svg), which draws one dart
 * from three camera angles: red, green and blue rays converging on a white
 * fused result over a charcoal tile. Those five colours are the brand, and
 * everything else here is derived from them.
 *
 * Two rules keep this readable on a dartboard:
 *
 *  1. **Team colours never use red or green.** A real board is red, green,
 *     cream and black, and game state is drawn on top of it. Teams sit on the
 *     blue/amber axis instead, which stays legible over any segment and is
 *     also the safer pair for red-green colour blindness.
 *
 *  2. **Red and green are reserved for meaning** -- invalid and valid. Using
 *     them for identity as well would make "green" ambiguous the moment a
 *     green team did something wrong.
 */
#pragma once

#include "game_lib/components/render_object.hpp"

namespace Palette
{
    // ---- Brand, straight from the icon ------------------------------------
    inline constexpr Color CHARCOAL   = {  38,  38,  43 };   // #26262B tile
    inline constexpr Color RED        = { 232,  69,  60 };   // #E8453C camera 1
    inline constexpr Color GREEN      = {  61, 201,  90 };   // #3DC95A camera 2
    inline constexpr Color BLUE       = {  76, 134, 240 };   // #4C86F0 camera 3
    inline constexpr Color WHITE      = { 245, 245, 247 };   // #F5F5F7 fused

    // ---- Chrome ------------------------------------------------------------
    inline constexpr Color BG         = {  26,  26,  30 };   // behind everything
    inline constexpr Color BG_PANEL   = CHARCOAL;            // cards, keys
    inline constexpr Color BG_RAISED  = {  52,  52,  59 };   // hover / unselected
    inline constexpr Color BG_SELECT  = {  38,  58, 102 };   // selected row, blue-tinted

    inline constexpr Color TEXT       = WHITE;
    inline constexpr Color TEXT_DIM   = { 150, 150, 160 };
    inline constexpr Color TEXT_MUTED = { 110, 110, 120 };

    // ---- Meaning -----------------------------------------------------------
    inline constexpr Color SELECT     = BLUE;
    inline constexpr Color CONFIRM    = GREEN;
    inline constexpr Color INVALID    = RED;
    inline constexpr Color WARNING    = { 240, 176,  76 };   // #F0B04C

    // ---- Identity ----------------------------------------------------------
    //
    // Amber is the blue's complement at matching saturation, so the two read as
    // a deliberate pair rather than two colours that happen to differ.
    inline constexpr Color TEAM0      = BLUE;
    inline constexpr Color TEAM1      = { 240, 162,  76 };   // #F0A24C

    /** Dart markers, distinct from both teams and from the board. */
    inline constexpr Color MARKER     = { 190, 110, 235 };   // #BE6EEB
}
