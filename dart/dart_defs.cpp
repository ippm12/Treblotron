/**
 * dart_defs.cpp
 *
 * Implementation of generic dart board helper functions.
 */

#include "dart/dart_defs.hpp"
#include "dart/dart_board_geometry.hpp"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace DartBoardGeometry;

/** Standard dart board layout: clockwise order starting from top. */
static const uint8_t s_boardLayout[DART_NUM_SECTIONS] =
{
    20, 1, 18, 4, 13, 6, 10, 15, 2, 17,
    3, 19, 7, 16, 8, 11, 14, 9, 12, 5
};


DartRing getSegmentRing(DartSegment segment)
{
    uint8_t idx = static_cast<uint8_t>(segment);

    if(idx < DART_NUM_SECTIONS)
    {
        return DartRing::Double;
    }
    if(idx < DART_NUM_SECTIONS * 2)
    {
        return DartRing::OuterSingle;
    }
    if(idx < DART_NUM_SECTIONS * 3)
    {
        return DartRing::Triple;
    }
    if(idx < DART_NUM_SECTIONS * 4)
    {
        return DartRing::InnerSingle;
    }
    if(idx == static_cast<uint8_t>(DartSegment::OUTER_BULL))
    {
        return DartRing::OuterBull;
    }

    return DartRing::InnerBull;
}


uint8_t getSegmentSection(DartSegment segment)
{
    uint8_t idx = static_cast<uint8_t>(segment);

    if(idx >= DART_NUM_SECTIONS * DART_NUM_SECTION_RINGS)
    {
        return 0; // Bulls have no section number
    }

    // Each ring group has 20 entries, numbered 1-20
    return static_cast<uint8_t>((idx % DART_NUM_SECTIONS) + 1);
}


uint16_t getSegmentPoints(DartSegment segment)
{
    uint8_t section = getSegmentSection(segment);

    if(section == 0)
    {
        // Bull segments
        if(segment == DartSegment::INNER_BULL)
        {
            return 50;
        }
        return 25; // OUTER_BULL
    }

    return static_cast<uint16_t>(section * getSegmentMultiplier(segment));
}


uint8_t getSegmentMultiplier(DartSegment segment)
{
    DartRing ring = getSegmentRing(segment);

    switch(ring)
    {
        case DartRing::Double:
            return 2;
        case DartRing::Triple:
            return 3;
        default:
            return 1;
    }
}


const uint8_t* getBoardLayout()
{
    return s_boardLayout;
}


uint8_t getSectionPositionIndex(uint8_t sectionNumber)
{
    for(uint8_t i = 0; i < DART_NUM_SECTIONS; i++)
    {
        if(s_boardLayout[i] == sectionNumber)
        {
            return i;
        }
    }

    return 0xFF;
}


std::optional<DartRing> polarToRing(float normalizedRadius)
{
    if(normalizedRadius > RADIUS_DOUBLE_OUTER)
    {
        return std::nullopt; // Off-board
    }
    if(normalizedRadius <= RADIUS_OUTER_BULL_INNER)
    {
        return DartRing::InnerBull;
    }
    if(normalizedRadius <= RADIUS_INNER_SINGLE_INNER)
    {
        return DartRing::OuterBull;
    }
    if(normalizedRadius <= RADIUS_TRIPLE_INNER)
    {
        return DartRing::InnerSingle;
    }
    if(normalizedRadius <= RADIUS_OUTER_SINGLE_INNER)
    {
        return DartRing::Triple;
    }
    if(normalizedRadius <= RADIUS_DOUBLE_INNER)
    {
        return DartRing::OuterSingle;
    }

    return DartRing::Double;
}


uint8_t angleToSection(float angle)
{
    // Convert from atan2 convention to board convention:
    // atan2: 0=right, +CCW. Board: section 20 at top (-90 degrees).
    // Shift so section 20 (posIndex 0) is centered at 0, then offset
    // by half-section so boundaries fall at integer multiples of 18.
    float adjusted = angle + 90.0f + 9.0f;

    // Normalize to [0, 360)
    while(adjusted < 0.0f)
    {
        adjusted += 360.0f;
    }
    while(adjusted >= 360.0f)
    {
        adjusted -= 360.0f;
    }

    int posIndex = static_cast<int>(adjusted / 18.0f);
    if(posIndex < 0)
    {
        posIndex = 0;
    }
    if(posIndex >= DART_NUM_SECTIONS)
    {
        posIndex = DART_NUM_SECTIONS - 1;
    }

    return s_boardLayout[posIndex];
}


std::optional<DartSegment> polarToSegment(float angle, float normalizedRadius)
{
    auto ring = polarToRing(normalizedRadius);
    if(!ring.has_value())
    {
        return std::nullopt; // Off-board
    }

    // Bulls don't need a section
    if(ring.value() == DartRing::InnerBull)
    {
        return DartSegment::INNER_BULL;
    }
    if(ring.value() == DartRing::OuterBull)
    {
        return DartSegment::OUTER_BULL;
    }

    uint8_t section = angleToSection(angle);

    // Compute enum offset: each ring has 20 entries, section 1 at offset 0
    uint8_t ringOffset = 0;
    switch(ring.value())
    {
        case DartRing::Double:      ringOffset = 0;  break;
        case DartRing::OuterSingle: ringOffset = 20; break;
        case DartRing::Triple:      ringOffset = 40; break;
        case DartRing::InnerSingle: ringOffset = 60; break;
        default: break;
    }

    uint8_t segmentIndex = ringOffset + (section - 1);
    return static_cast<DartSegment>(segmentIndex);
}
