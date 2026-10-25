#include "engine/rhi/backends/vulkan/VulkanInternal.hpp"

#include <algorithm>
#include <string>

#include <shaderc/shaderc.h>
#include <spirv_reflect.h>

#include "engine/core/Log.hpp"

namespace rhi {

namespace {

// Rewrites every `layout(... binding = N ...)` so each descriptor class lands
// in its own range, and records what it found. The declarations in the corpus
// are regular enough for this to be exact: a binding qualifier is always
// followed by `uniform <Block|samplerX|imageX>` or `buffer <Block>`.
// SPIR-V does not care which GLSL #version a source declares, but shaderc
// gates features SYNTACTICALLY on it: explicit bindings are 420+, and the
// dual-source 2D shaders declare 410 for the GL 4.1 driver's sake. Promote
// everything to 460 (the corpus standard) before compiling — the GL backends
// never see this rewrite.
str promoteVersion(const str& source) {
    const size_t at = source.find("#version");
    if (at == str::npos) {
        return source;
    }
    const size_t eol = source.find('\n', at);
    if (eol == str::npos) {
        return source;
    }
    str out = source;
    out.replace(at, eol - at, "#version 460 core");
    return out;
}

str remapBindings(const str& source, vector<ShaderResource>& resources) {
    str out;
    out.reserve(source.size() + 64);

    size_t pos = 0;
    while (true) {
        const size_t layoutAt = source.find("layout(", pos);
        if (layoutAt == str::npos) {
            out.append(source, pos, str::npos);
            break;
        }
        const size_t close = source.find(')', layoutAt);
        if (close == str::npos) {
            out.append(source, pos, str::npos);
            break;
        }
        const str layout = source.substr(layoutAt, close - layoutAt + 1);
        const size_t bindingAt = layout.find("binding");
        if (bindingAt == str::npos) {
            out.append(source, pos, close - pos + 1); // location=, std140-only…
            pos = close + 1;
            continue;
        }

        // Parse the binding number out of the qualifier list.
        const size_t digit = layout.find_first_of("0123456789", bindingAt);
        if (digit == str::npos) {
            out.append(source, pos, close - pos + 1);
            pos = close + 1;
            continue;
        }
        size_t digitEnd = layout.find_first_not_of("0123456789", digit);
        if (digitEnd == str::npos) {
            digitEnd = layout.size();
        }
        const u32 original = static_cast<u32>(
            std::stoul(layout.substr(digit, digitEnd - digit)));

        // Classify from the declaration that follows the qualifier, by reading
        // words until the `uniform` or `buffer` keyword: memory qualifiers
        // (readonly/writeonly/coherent) may sit in between.
        DescriptorClass klass = DescriptorClass::Uniform;
        bool comparison = false;
        ImageDim dim = ImageDim::Any;
        size_t at = close + 1;
        auto nextWord = [&]() -> str {
            at = source.find_first_not_of(" \t\r\n", at);
            if (at == str::npos) {
                return {};
            }
            const size_t end = source.find_first_of(" \t\r\n;{(", at);
            const str word = source.substr(
                at, end == str::npos ? str::npos : end - at);
            at = (end == str::npos) ? source.size() : end;
            return word;
        };
        for (u32 guard = 0; guard < 4; ++guard) {
            const str word = nextWord();
            if (word.empty() || word == "buffer") {
                klass = DescriptorClass::Storage;
                break;
            }
            if (word == "uniform") {
                // Memory qualifiers sit on EITHER side of `uniform`
                // (`writeonly uniform image3D` and `uniform writeonly
                // image3D` are both legal GLSL): skip them before reading
                // the type, or a storage image classifies as a uniform
                // block and its binding collides with the real UBOs
                // (found the hard way on rc_inject.comp).
                str type = nextWord();
                for (u32 skip = 0; skip < 4; ++skip) {
                    if (type == "readonly" || type == "writeonly" ||
                        type == "coherent" || type == "volatile" ||
                        type == "restrict") {
                        type = nextWord();
                        continue;
                    }
                    break;
                }
                // find(), not a prefix test: isampler/usampler/uimage/
                // iimage must classify like their float cousins.
                if (type.find("sampler") != str::npos ||
                    type.find("texture") != str::npos) {
                    klass = DescriptorClass::Sampler;
                    comparison = type.find("Shadow") != str::npos;
                    if (type.find("Cube") != str::npos) {
                        dim = ImageDim::Cube;
                    } else if (type.find("3D") != str::npos) {
                        dim = ImageDim::T3D;
                    } else if (type.find("2DArray") != str::npos) {
                        dim = ImageDim::T2DArray;
                    } else if (type.find("2D") != str::npos) {
                        dim = ImageDim::T2D;
                    }
                } else if (type.find("image") != str::npos) {
                    klass = DescriptorClass::StorageImage;
                    if (type.find("3D") != str::npos) {
                        dim = ImageDim::T3D;
                    } else if (type.find("2D") != str::npos) {
                        dim = ImageDim::T2D;
                    }
                } else {
                    klass = DescriptorClass::Uniform; // uniform block
                }
                break;
            }
            // readonly / writeonly / coherent / restrict — keep looking.
        }

        const u32 shifted = original + bindingOffset(klass);
        resources.push_back({ shifted, klass, comparison, dim });

        // Emit the qualifier with the shifted number.
        out.append(source, pos, layoutAt - pos);
        out.append(layout, 0, digit);
        out.append(std::to_string(shifted));
        out.append(layout, digitEnd, str::npos);
        pos = close + 1;
    }
    return out;
}

// Compiles one GLSL stage to SPIR-V. The sources are the SAME GLSL 460 the GL
// backend consumes (ShaderLibrary has already expanded the #includes): the
// corpus carries explicit layout(location=)/layout(binding=) everywhere, and
// the only builtin that differs between the dialects is spelled through
// compat.glsl, keyed on the VULKAN macro that shaderc/glslang predefines.
bool compileToSpv(const str& source, shaderc_shader_kind kind,
                  const str& debugName, vector<u32>& out) {
    // One compiler + options for the whole process (shaderc compilation
    // is thread-safe on a shared compiler): initializing per STAGE paid
    // a full glslang bring-up on every stage of every shader, at boot
    // and on each hot reload.
    static shaderc_compiler_t compiler = shaderc_compiler_initialize();
    static shaderc_compile_options_t options = [] {
        shaderc_compile_options_t o = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(
            o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        shaderc_compile_options_set_optimization_level(
            o, shaderc_optimization_level_performance);
        return o;
    }();

    shaderc_compilation_result_t result =
        shaderc_compile_into_spv(compiler, source.c_str(), source.size(), kind,
                                 debugName.c_str(), "main", options);

    const bool ok = shaderc_result_get_compilation_status(result) ==
                    shaderc_compilation_status_success;
    if (ok) {
        const auto* bytes =
            reinterpret_cast<const u32*>(shaderc_result_get_bytes(result));
        const size_t words = shaderc_result_get_length(result) / sizeof(u32);
        out.assign(bytes, bytes + words);
    } else {
        LOG_ERROR("Vulkan shader '{}' failed to compile:\n{}", debugName,
                  shaderc_result_get_error_message(result));
    }
    shaderc_result_release(result);
    return ok;
}

// Reflects the compiled SPIR-V, records what the stage declares (class,
// image dim, depth-compare — all authoritative in SPIR-V, no GLSL text
// parsing) and PATCHES each binding decoration up into its class range
// (bindingOffset — GL's per-class namespaces do not survive into Vulkan).
bool reflectAndShiftBindings(vector<u32>& spv,
                             vector<ShaderResource>& resources,
                             const str& debugName, const char* stage) {
    SpvReflectShaderModule module {};
    if (spvReflectCreateShaderModule(spv.size() * sizeof(u32), spv.data(),
                                     &module) !=
        SPV_REFLECT_RESULT_SUCCESS) {
        LOG_ERROR("Vulkan shader '{}.{}': SPIR-V reflection failed",
                  debugName, stage);
        return false;
    }
    u32 count = 0;
    spvReflectEnumerateDescriptorBindings(&module, &count, nullptr);
    vector<SpvReflectDescriptorBinding*> bindings(count);
    spvReflectEnumerateDescriptorBindings(&module, &count, bindings.data());
    bool ok = true;
    for (SpvReflectDescriptorBinding* b : bindings) {
        DescriptorClass klass = DescriptorClass::Uniform;
        ImageDim dim = ImageDim::Any;
        bool comparison = false;
        switch (b->descriptor_type) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            klass = DescriptorClass::Uniform;
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            klass = DescriptorClass::Storage;
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            klass = DescriptorClass::Sampler;
            comparison = b->image.depth == 1;
            if (b->image.dim == SpvDimCube) {
                dim = ImageDim::Cube;
            } else if (b->image.dim == SpvDim3D) {
                dim = ImageDim::T3D;
            } else if (b->image.dim == SpvDim2D) {
                dim = b->image.arrayed ? ImageDim::T2DArray : ImageDim::T2D;
            }
            break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            klass = DescriptorClass::StorageImage;
            if (b->image.dim == SpvDim3D) {
                dim = ImageDim::T3D;
            } else if (b->image.dim == SpvDim2D) {
                dim = ImageDim::T2D;
            }
            break;
        default:
            LOG_ERROR("Vulkan shader '{}.{}': unsupported descriptor type "
                      "{} at binding {}",
                      debugName, stage,
                      static_cast<int>(b->descriptor_type), b->binding);
            ok = false;
            continue;
        }
        const u32 shifted = b->binding + bindingOffset(klass);
        if (spvReflectChangeDescriptorBindingNumbers(
                &module, b, shifted, SPV_REFLECT_SET_NUMBER_DONT_CHANGE) !=
            SPV_REFLECT_RESULT_SUCCESS) {
            LOG_ERROR("Vulkan shader '{}.{}': binding {} shift failed",
                      debugName, stage, b->binding);
            ok = false;
            continue;
        }
        resources.push_back({ shifted, klass, comparison, dim });
    }
    if (ok) {
        const u32* code = spvReflectGetCode(&module);
        spv.assign(code, code + spvReflectGetCodeSize(&module) / sizeof(u32));
    }
    spvReflectDestroyShaderModule(&module);
    return ok;
}

} // namespace

// ShaderDesc::uniformBlocks / ::samplers are deliberately ignored here: they
// exist so the GL backend can assign bindings AFTER link (GLSL 4.10 has no
// layout(binding=)). The shader corpus already carries explicit binding
// qualifiers, which SPIR-V reads directly — so on Vulkan there is nothing to
// patch up.
ShaderHandle VulkanDevice::createShader(const ShaderDesc& desc) {
    Impl& d = *impl;
    const bool isCompute = !desc.computeSource.empty();
    if (isCompute &&
        (!desc.vertexSource.empty() || !desc.fragmentSource.empty())) {
        LOG_ERROR("Vulkan createShader '{}': compute excludes vertex/fragment",
                  desc.debugName);
        return {};
    }
    if (!isCompute &&
        (desc.vertexSource.empty() || desc.fragmentSource.empty())) {
        LOG_ERROR("Vulkan createShader '{}': needs vertex + fragment",
                  desc.debugName);
        return {};
    }

    VulkanShader shader {};

    auto build = [&](const str& source, shaderc_shader_kind kind,
                     const char* stage, VkShaderModule& module) {
        // Compile the ORIGINAL source, then reflect the SPIR-V: the
        // per-class binding shift (GL's separate namespaces do not
        // survive into Vulkan) is patched into the decorations, and the
        // pipeline-layout metadata comes from reflection, not from
        // parsing GLSL text.
        const size_t before = shader.resources.size();
        vector<u32> spv;
        if (!compileToSpv(promoteVersion(source), kind,
                          desc.debugName + "." + stage, spv) ||
            !reflectAndShiftBindings(spv, shader.resources, desc.debugName,
                                     stage)) {
            return false;
        }
        // Transitional cross-check: the retired GLSL text parser must
        // agree with the reflection (order-insensitive). Remove with it
        // once the corpus has cycled through a few releases.
        vector<ShaderResource> legacy;
        remapBindings(promoteVersion(source), legacy);
        auto key = [](const ShaderResource& r) {
            return (static_cast<u64>(r.binding) << 8) |
                   (static_cast<u64>(r.klass) << 2) |
                   (static_cast<u64>(r.comparison) << 1) |
                   static_cast<u64>(r.dim == ImageDim::Any);
        };
        vector<u64> a;
        for (size_t i = before; i < shader.resources.size(); ++i) {
            a.push_back(key(shader.resources[i]));
        }
        vector<u64> b;
        for (const ShaderResource& r : legacy) {
            b.push_back(key(r));
        }
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        // Subset check: the legacy parser lists DECLARED bindings, the
        // optimizer strips the unused ones before reflection — smaller
        // is expected. The alarm is a reflected resource the parser
        // missed or classified differently.
        if (!std::includes(b.begin(), b.end(), a.begin(), a.end())) {
            LOG_ERROR("Vulkan shader '{}.{}': SPIR-V reflection found "
                      "resources the legacy GLSL parser disagrees on "
                      "({} reflected vs {} parsed)",
                      desc.debugName, stage, a.size(), b.size());
        }
        VkShaderModuleCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = spv.size() * sizeof(u32);
        info.pCode = spv.data();
        return vkOk(vkCreateShaderModule(d.device, &info, nullptr, &module),
                    "vkCreateShaderModule");
    };

    bool ok = true;
    if (isCompute) {
        ok = build(desc.computeSource, shaderc_compute_shader, "comp",
                   shader.compute);
    } else {
        ok = build(desc.vertexSource, shaderc_vertex_shader, "vert",
                   shader.vertex) &&
             build(desc.fragmentSource, shaderc_fragment_shader, "frag",
                   shader.fragment);
    }
    if (!ok) {
        // Partial success still leaves a module behind; drop it.
        vkDestroyShaderModule(d.device, shader.vertex, nullptr);
        vkDestroyShaderModule(d.device, shader.fragment, nullptr);
        vkDestroyShaderModule(d.device, shader.compute, nullptr);
        return {};
    }

    // Vertex and fragment legitimately declare the same resource (FrameUbo…):
    // collapse duplicates, then assert the remap actually separated the
    // classes. A leftover clash would only surface as an invalid descriptor
    // set layout much later, so it is caught loudly here instead.
    std::sort(shader.resources.begin(), shader.resources.end(),
              [](const ShaderResource& a, const ShaderResource& b) {
                  return a.binding < b.binding;
              });
    shader.resources.erase(
        std::unique(shader.resources.begin(), shader.resources.end(),
                    [](const ShaderResource& a, const ShaderResource& b) {
                        return a.binding == b.binding && a.klass == b.klass;
                    }),
        shader.resources.end());
    for (size_t i = 1; i < shader.resources.size(); ++i) {
        if (shader.resources[i].binding == shader.resources[i - 1].binding) {
            LOG_ERROR("Vulkan createShader '{}': binding {} claimed by two "
                      "descriptor classes after remap",
                      desc.debugName, shader.resources[i].binding);
            vkDestroyShaderModule(d.device, shader.vertex, nullptr);
            vkDestroyShaderModule(d.device, shader.fragment, nullptr);
            vkDestroyShaderModule(d.device, shader.compute, nullptr);
            return {};
        }
    }

    const u32 id = d.nextId++;
    d.shaders.emplace(id, shader);
    return { id };
}

void VulkanDevice::destroyShader(ShaderHandle handle) {
    Impl& d = *impl;
    auto it = d.shaders.find(handle.id);
    if (it == d.shaders.end()) {
        return;
    }
    d.pendingShaders.push_back({ it->second.vertex, it->second.fragment,
                                 it->second.compute, d.frameCounter });
    d.shaders.erase(it);
}

} // namespace rhi
