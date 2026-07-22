#pragma once

#include <utility>
#include <variant>

#include "engine/core/Assert.hpp"
#include "engine/core/Defines.hpp"

namespace core {

// Failure payload: `return core::Error { "why it failed" };`.
struct Error {
    str message;
};

// Minimal expected-like result (§8: recoverable errors carry their REASON;
// std::expected is C++23, we are C++20). The read API mirrors
// std::optional (operator bool / has_value / * / -> / value) so call sites
// migrating from `optional<T>` keep compiling unchanged; `error()` is what
// optional could never offer: the reason, for the caller to log, display
// (plugin panel, cooker CLI) or wrap.
//
// Introduced with its first real call path — the §5 plugin loader — per
// the audit lesson: core primitives arrive with a concrete client, not
// speculatively.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage(std::in_place_index<0>, std::move(value)) {}
    Result(Error error)
        : storage(std::in_place_index<1>, std::move(error.message)) {}

    explicit operator bool() const { return storage.index() == 0; }
    bool has_value() const { return storage.index() == 0; }

    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    T& value() {
        ENGINE_ASSERT_MSG(has_value(), "Result::value() on an error");
        return std::get<0>(storage);
    }
    const T& value() const {
        ENGINE_ASSERT_MSG(has_value(), "Result::value() on an error");
        return std::get<0>(storage);
    }

    // The failure reason. Only meaningful when !has_value().
    const str& error() const {
        ENGINE_ASSERT_MSG(!has_value(), "Result::error() on a value");
        return std::get<1>(storage);
    }

private:
    std::variant<T, str> storage;
};

} // namespace core
