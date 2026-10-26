#pragma once

#include <cstddef>

#include "engine/core/Defines.hpp"

namespace reflect {

// ONE definition per field shared by two structs: a Form (data) and the
// engine params it feeds, or any other pair that must stay in step. A lane
// is a pair of member pointers, compile-checked against both types, and
// every table of lanes is walked in BOTH directions — apply and capture —
// so a field added to a table is read AND written, and a field forgotten
// in one of two hand-written lists (how nine conifer fields once silently
// stopped saving) cannot happen. The template needs no data/ include; only
// the tables, written next to their Forms, do.
template <typename Params, typename Form, typename T>
struct FieldLane {
    using ParamsType = Params;
    using FormType = Form;
    using ValueType = T;

    T Params::* param;
    T Form::* form;
    // Display name for a panel drawing the lane (nullptr = the Form
    // field's reflected name). The field's slider range is reflected
    // metadata on the Form (REFLECT_FIELD_RANGE), not a lane column.
    const char* label { nullptr };
};

template <typename Params, typename Form, typename T, std::size_t N>
void applyLanes(Params& params, const Form& form,
                const FieldLane<Params, Form, T> (&lanes)[N]) {
    for (const auto& lane : lanes) {
        params.*lane.param = form.*lane.form;
    }
}

template <typename Params, typename Form, typename T, std::size_t N>
void captureLanes(const Params& params, Form& form,
                  const FieldLane<Params, Form, T> (&lanes)[N]) {
    for (const auto& lane : lanes) {
        form.*lane.form = params.*lane.param;
    }
}

// Several tables of one Params/Form pair in one call: a panel splits a
// pair's lanes into draw-order runs, and apply/capture walk every run.
template <typename Params, typename Form, typename... Tables>
    requires(sizeof...(Tables) > 1)
void applyLanes(Params& params, const Form& form, const Tables&... tables) {
    (applyLanes(params, form, tables), ...);
}

template <typename Params, typename Form, typename... Tables>
    requires(sizeof...(Tables) > 1)
void captureLanes(const Params& params, Form& form,
                  const Tables&... tables) {
    (captureLanes(params, form, tables), ...);
}

// Per-lane visitor for the callers that do more than copy (a blend between
// two Forms, a clamp, a hash): fn(T& param, const T& form).
template <typename Params, typename Form, typename T, std::size_t N,
          typename Fn>
void forEachLane(Params& params, const Form& form,
                 const FieldLane<Params, Form, T> (&lanes)[N], Fn&& fn) {
    for (const auto& lane : lanes) {
        fn(params.*lane.param, form.*lane.form);
    }
}

} // namespace reflect
