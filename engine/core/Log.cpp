#include "engine/core/Log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace core {

void Log::init() {
    auto logger = spdlog::stdout_color_mt("meadows");
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern("[%T.%e] [%^%l%$] %v");
#ifndef NDEBUG
    spdlog::set_level(spdlog::level::trace);
#endif
    // Flush every line: when stdout is a pipe (CI, tooling) a crash must
    // not swallow the last messages — the ones that say where it died.
    spdlog::flush_on(spdlog::level::trace);
}

} // namespace core
