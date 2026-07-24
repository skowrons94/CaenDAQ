#ifndef UTILS_H_INCLUDED
#define UTILS_H_INCLUDED

#include <cstdint>
#include <utility>

// Bit ranges are stored as (lowest bit, highest bit), both ends included.
using BitRange = std::pair<int,int>;

// Extract the bits [lo,hi] of a 32 bit word and right align them.
// This is the hot path of the whole unpacking, hence the plain mask and shift
// instead of the std::bitset shuffling that was used before.
inline uint32_t ExtractBits( uint32_t word, int lo, int hi )
{
  const int width = hi - lo + 1;
  if( width >= 32 ) return word >> lo;
  return ( word >> lo ) & ( ( 1u << width ) - 1u );
}

inline uint32_t ExtractBits( uint32_t word, const BitRange& range )
{
  return ExtractBits( word, range.first, range.second );
}

// Number of bits covered by a range.
inline int RangeWidth( const BitRange& range )
{
  return range.second - range.first + 1;
}

#endif // UTILS_H_INCLUDED
