/**
 * dart_defs.hpp
 *
 * Generic dart board definitions: segment IDs, ring types, board layout,
 * and helper functions for scoring and geometry. This module has no
 * rendering or ECS dependencies.
 */

#ifndef DART_DEFS_HPP
#define DART_DEFS_HPP

#include <cstdint>
#include <optional>

/**
 * Every individually addressable segment on a standard dart board.
 * Grouped by ring type, numbered 1-20 within each group.
 * 20 sections x 4 rings + 2 bulls = 82 total.
 */
enum class DartSegment : uint8_t
{
    // Doubles (outermost thin ring) - 0 to 19
    DOUBLE_1 = 0,
    DOUBLE_2,
    DOUBLE_3,
    DOUBLE_4,
    DOUBLE_5,
    DOUBLE_6,
    DOUBLE_7,
    DOUBLE_8,
    DOUBLE_9,
    DOUBLE_10,
    DOUBLE_11,
    DOUBLE_12,
    DOUBLE_13,
    DOUBLE_14,
    DOUBLE_15,
    DOUBLE_16,
    DOUBLE_17,
    DOUBLE_18,
    DOUBLE_19,
    DOUBLE_20,

    // Outer singles (large outer area) - 20 to 39
    OUTER_SINGLE_1,
    OUTER_SINGLE_2,
    OUTER_SINGLE_3,
    OUTER_SINGLE_4,
    OUTER_SINGLE_5,
    OUTER_SINGLE_6,
    OUTER_SINGLE_7,
    OUTER_SINGLE_8,
    OUTER_SINGLE_9,
    OUTER_SINGLE_10,
    OUTER_SINGLE_11,
    OUTER_SINGLE_12,
    OUTER_SINGLE_13,
    OUTER_SINGLE_14,
    OUTER_SINGLE_15,
    OUTER_SINGLE_16,
    OUTER_SINGLE_17,
    OUTER_SINGLE_18,
    OUTER_SINGLE_19,
    OUTER_SINGLE_20,

    // Triples (thin ring) - 40 to 59
    TRIPLE_1,
    TRIPLE_2,
    TRIPLE_3,
    TRIPLE_4,
    TRIPLE_5,
    TRIPLE_6,
    TRIPLE_7,
    TRIPLE_8,
    TRIPLE_9,
    TRIPLE_10,
    TRIPLE_11,
    TRIPLE_12,
    TRIPLE_13,
    TRIPLE_14,
    TRIPLE_15,
    TRIPLE_16,
    TRIPLE_17,
    TRIPLE_18,
    TRIPLE_19,
    TRIPLE_20,

    // Inner singles (large inner area) - 60 to 79
    INNER_SINGLE_1,
    INNER_SINGLE_2,
    INNER_SINGLE_3,
    INNER_SINGLE_4,
    INNER_SINGLE_5,
    INNER_SINGLE_6,
    INNER_SINGLE_7,
    INNER_SINGLE_8,
    INNER_SINGLE_9,
    INNER_SINGLE_10,
    INNER_SINGLE_11,
    INNER_SINGLE_12,
    INNER_SINGLE_13,
    INNER_SINGLE_14,
    INNER_SINGLE_15,
    INNER_SINGLE_16,
    INNER_SINGLE_17,
    INNER_SINGLE_18,
    INNER_SINGLE_19,
    INNER_SINGLE_20,

    // Bulls - 80 to 81
    OUTER_BULL,
    INNER_BULL,

    COUNT // = 82
};

/** The ring (region) a segment belongs to. */
enum class DartRing : uint8_t
{
    Double,       // Outermost thin ring (2x multiplier)
    OuterSingle,  // Large outer single area (1x)
    Triple,       // Thin ring (3x multiplier)
    InnerSingle,  // Large inner single area (1x)
    OuterBull,    // Outer bullseye ring (25 points)
    InnerBull,    // Inner bullseye dot (50 points)
    COUNT
};

/** Number of numbered sections on the board. */
static constexpr uint8_t DART_NUM_SECTIONS = 20;

/** Number of rings that have per-section segments (excludes bulls). */
static constexpr uint8_t DART_NUM_SECTION_RINGS = 4;

/** Total number of addressable segments. */
static constexpr uint8_t DART_NUM_SEGMENTS = static_cast<uint8_t>(DartSegment::COUNT);

/** Which ring does this segment belong to? */
DartRing getSegmentRing(DartSegment segment);

/** Section number (1-20) this segment belongs to. Returns 0 for bulls. */
uint8_t getSegmentSection(DartSegment segment);

/** Point value of hitting this segment (e.g., TRIPLE_20 = 60, INNER_BULL = 50). */
uint16_t getSegmentPoints(DartSegment segment);

/** Score multiplier for this segment's ring (1, 2, or 3). Bulls return 1. */
uint8_t getSegmentMultiplier(DartSegment segment);

/**
 * Standard board layout: clockwise order of section numbers starting from top.
 * Returns pointer to a static array of 20 values:
 * {20, 1, 18, 4, 13, 6, 10, 15, 2, 17, 3, 19, 7, 16, 8, 11, 14, 9, 12, 5}
 */
const uint8_t* getBoardLayout();

/**
 * Get the clockwise position index (0-19) for a given section number (1-20).
 * This determines the section's angular position on the board.
 * Returns 0xFF if sectionNumber is invalid.
 */
uint8_t getSectionPositionIndex(uint8_t sectionNumber);

/**
 * Convert normalized radius to a DartRing. Returns nullopt for off-board (miss).
 * normalizedRadius: 0.0 (center) to 1.0 (double outer edge).
 */
std::optional<DartRing> polarToRing(float normalizedRadius);

/**
 * Convert angle to section number (1-20).
 * angle: degrees, atan2 convention (0=right, positive CCW).
 */
uint8_t angleToSection(float angle);

/**
 * Convert polar coordinates to a DartSegment. Returns nullopt for off-board (miss).
 * angle: degrees, atan2 convention (0=right, positive CCW).
 * normalizedRadius: 0.0 (center) to 1.0 (double outer edge).
 */
std::optional<DartSegment> polarToSegment(float angle, float normalizedRadius);

/**
 * Human-readable name for a segment (e.g. "Double 20", "Triple 19", "Inner Bull").
 * Returns a pointer to a static string — do not free.
 */
const char* getSegmentName(DartSegment segment);

#endif // DART_DEFS_HPP
