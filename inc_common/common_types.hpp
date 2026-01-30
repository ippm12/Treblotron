/**
 * common_types.hpp
 * 
 * Common type definitions
 */

#ifndef COMMON_TYPES_HPP
#define COMMON_TYPES_HPP

#include <cstdint>

// Generics
typedef std::uint8_t  uint8_t;
typedef std::uint16_t uint16_t;
typedef std::uint32_t uint32_t;
typedef std::uint64_t uint64_t;

typedef std::int8_t   int8_t;
typedef std::int16_t  int16_t;
typedef std::int32_t  int32_t;
typedef std::int64_t  int64_t;

// Status
typedef int32_t Status;

#define IS_STATUS_OK(status)        (status >= 0)
#define IS_STATUS_NOT_OK(status)    (status < 0)

#define STATUS_OK                       (0)
#define STATUS_ERROR_GENERIC            (-1)
#define STATUS_ERROR_NULL               (-2)
#define STATUS_ERROR_NULL_PARAM         (-3)
#define STATUS_ERROR_INVALID_PARAM      (-4)
#define STATUS_ERROR_INVALID_STATE      (-5)


#endif // COMMON_TYPES_HPP