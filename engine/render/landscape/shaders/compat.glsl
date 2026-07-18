// Cross-backend spellings (chantier VULKAN, V3).
//
// One shader corpus feeds both backends: the sources stay GLSL 460 and the
// GL 4.6 backend compiles them unchanged, while the Vulkan backend compiles
// them to SPIR-V through glslang. Almost everything is already identical —
// explicit layout(location=)/layout(binding=) are valid in both. The builtins
// below are the exception, so they get a neutral spelling here.
//
// The Vulkan backend injects `#define VULKAN 1` right after the #version line;
// nothing is injected on the GL path, which is why the fallback is the GL
// spelling.

#ifdef VULKAN
// SPIR-V has no gl_VertexID: gl_VertexIndex already includes the base vertex.
#define MEADOWS_VERTEX_INDEX gl_VertexIndex
#else
#define MEADOWS_VERTEX_INDEX gl_VertexID
#endif
