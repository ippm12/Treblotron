/**
 * dart_board_geometry.hpp
 *
 * Shared dart board geometry constants. All radii are expressed as
 * proportions of the board's outer edge (1.0 = double-ring outer edge).
 * Multiply by BASE_RADIUS * scale to get pixel values.
 */

#ifndef DART_BOARD_GEOMETRY_HPP
#define DART_BOARD_GEOMETRY_HPP

namespace DartBoardGeometry
{
    /** Base radius in pixels before scaling. */
    static constexpr float BASE_RADIUS = 200.0f;

    /** Ring boundary radii (outer edge of each ring, as proportion of board edge).
     *  Derived from WDF dartboard spec in mm, normalized to double-outer = 170 mm:
     *    double outer 170, double inner 162, triple outer 107, triple inner 99,
     *    outer bull 15.9, inner bull 6.35. */
    static constexpr float RADIUS_DOUBLE_OUTER        = 1.000f;
    static constexpr float RADIUS_DOUBLE_INNER        = 0.953f;
    static constexpr float RADIUS_OUTER_SINGLE_INNER  = 0.629f;
    static constexpr float RADIUS_TRIPLE_INNER        = 0.582f;
    static constexpr float RADIUS_INNER_SINGLE_INNER  = 0.094f;
    static constexpr float RADIUS_OUTER_BULL_INNER    = 0.037f;

    /** Background circle proportion (slightly larger than the board). */
    static constexpr float RADIUS_BACKGROUND = 1.15f;
}

#endif // DART_BOARD_GEOMETRY_HPP
