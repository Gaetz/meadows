#pragma once

#include <string>
#include <vector>
#include <array>
#include <memory>

/**
 * Types
 */

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

using i8 = signed char;
using i16 = signed short;
using i32 = signed int;
using i64 = signed long long;

using f32 = float;
using f64 = double;

using str = std::string;
using std::vector;
using std::array;

template<class T>
using uptr = std::unique_ptr<T>;

template<class T>
using sptr = std::shared_ptr<T>;
