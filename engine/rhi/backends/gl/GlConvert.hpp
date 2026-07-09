#pragma once

#include <glad/gl.h>

#include "engine/rhi/Rhi.hpp"

// rhi-enum → GL-enum converters, implemented in GlDeviceBase.cpp and
// shared by both backends. Declared ONCE here (audit U2-06) — the
// subclass .cpp files used to re-declare them by hand, a silent
// signature-drift hazard. Backend-internal header (includes glad).
namespace rhi {

u32 glVertexFormatComponents(VertexFormat format);
GLenum glToTopology(PrimitiveTopology topology);
GLenum glToCompare(CompareFunc func);

} // namespace rhi
