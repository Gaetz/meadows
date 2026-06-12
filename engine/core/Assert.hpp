#pragma once

#include "engine/core/Log.hpp"

// ENGINE_ASSERT(cond)            — invariant check, compiled out in release (§8).
// ENGINE_ASSERT_MSG(cond, ...)   — same, with a formatted message.

#ifndef NDEBUG
    #if defined(_MSC_VER)
        #define ENGINE_DEBUGBREAK() __debugbreak()
    #else
        #define ENGINE_DEBUGBREAK() __builtin_trap()
    #endif

    #define ENGINE_ASSERT(cond)                                                \
        do {                                                                   \
            if (!(cond)) {                                                     \
                LOG_CRITICAL("Assertion failed: {} ({}:{})", #cond, __FILE__,  \
                             __LINE__);                                        \
                ENGINE_DEBUGBREAK();                                           \
            }                                                                  \
        } while (false)

    #define ENGINE_ASSERT_MSG(cond, ...)                                       \
        do {                                                                   \
            if (!(cond)) {                                                     \
                LOG_CRITICAL("Assertion failed: {} ({}:{})", #cond, __FILE__,  \
                             __LINE__);                                        \
                LOG_CRITICAL(__VA_ARGS__);                                     \
                ENGINE_DEBUGBREAK();                                           \
            }                                                                  \
        } while (false)
#else
    #define ENGINE_ASSERT(cond)          ((void)0)
    #define ENGINE_ASSERT_MSG(cond, ...) ((void)0)
#endif
