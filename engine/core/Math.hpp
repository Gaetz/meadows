#pragma once

#include "engine/core/Defines.hpp"

// The shared float constants. Both spellings the code base used
// (3.14159265f, 3.1415927f; 6.2831853f) round to the same f32, so these
// are bit-identical replacements for every literal they absorbed.
namespace core {

inline constexpr f32 kPi = 3.14159265f;
inline constexpr f32 kTau = 6.2831853f;

} // namespace core
