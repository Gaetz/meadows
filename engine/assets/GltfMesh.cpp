#include "engine/assets/GltfMesh.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "engine/assets/Image.hpp"
#include "engine/core/Log.hpp"

namespace assets {

namespace {

const cgltf_accessor* findAttribute(const cgltf_primitive& primitive,
                                    cgltf_attribute_type type) {
    for (cgltf_size a = 0; a < primitive.attributes_count; ++a) {
        if (primitive.attributes[a].type == type &&
            primitive.attributes[a].index == 0) {
            return primitive.attributes[a].data;
        }
    }
    return nullptr;
}

// Flat normals for a vertex range appended without NORMAL data:
// area-weighted accumulation per referenced vertex, then normalize.
void reconstructNormals(render::MeshData& mesh, u32 firstVertex,
                        u32 firstIndex) {
    for (u64 i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
        render::MeshVertex& a = mesh.vertices[mesh.indices[i]];
        render::MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
        render::MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 face = glm::cross(b.position - a.position,
                                     c.position - a.position);
        a.normal += face;
        b.normal += face;
        c.normal += face;
    }
    for (u64 v = firstVertex; v < mesh.vertices.size(); ++v) {
        const f32 len = glm::length(mesh.vertices[v].normal);
        mesh.vertices[v].normal = len > 1e-8f
                                      ? mesh.vertices[v].normal / len
                                      : Vec3 { 0.0f, 1.0f, 0.0f };
    }
}

// The mesh pipeline is untextured (flat albedo x ramp): textured kits
// (Quaternius village...) bake the AVERAGE of each material's baseColor
// texture into the vertex color instead — flat stylized zones, no texture
// sampling, one draw. sRGB average linearized (vertex colors are linear).
// Texture files resolve next to the glTF, then in ../Textures (the
// Quaternius layout). Failures average to white (logged once per file).
using TextureAverages = std::unordered_map<str, Vec3>;

Vec3 averageTextureColor(const std::filesystem::path& baseDir,
                         const char* uri, TextureAverages& cache) {
    if (const auto it = cache.find(uri); it != cache.end()) {
        return it->second;
    }
    std::filesystem::path path = baseDir / uri;
    if (!std::filesystem::exists(path)) {
        path = baseDir / ".." / "Textures" / uri;
    }
    Vec3 average { 1.0f };
    if (const auto image = loadImageFile(path)) {
        f64 r = 0.0, g = 0.0, b = 0.0;
        const size_t pixels =
            static_cast<size_t>(image->width) * image->height;
        for (size_t i = 0; i < pixels; ++i) {
            r += image->pixels[i * 4 + 0];
            g += image->pixels[i * 4 + 1];
            b += image->pixels[i * 4 + 2];
        }
        const f64 inv = pixels > 0 ? 1.0 / (255.0 * pixels) : 0.0;
        // sRGB -> linear (gamma 2.2 approximation).
        average = { std::pow(static_cast<f32>(r * inv), 2.2f),
                    std::pow(static_cast<f32>(g * inv), 2.2f),
                    std::pow(static_cast<f32>(b * inv), 2.2f) };
    } else {
        LOG_WARN("glTF: baseColor texture '{}' unreadable — white bake",
                 uri);
    }
    cache.emplace(uri, average);
    return average;
}

void appendPrimitive(render::MeshData& mesh, const cgltf_primitive& primitive,
                     const Mat4& world,
                     const std::filesystem::path& baseDir,
                     TextureAverages* textureAverages) {
    const cgltf_accessor* positions =
        findAttribute(primitive, cgltf_attribute_type_position);
    if (!positions || primitive.type != cgltf_primitive_type_triangles) {
        return; // points/lines and position-less primitives are skipped
    }
    const cgltf_accessor* normals =
        findAttribute(primitive, cgltf_attribute_type_normal);
    const cgltf_accessor* uvs =
        findAttribute(primitive, cgltf_attribute_type_texcoord);
    const cgltf_accessor* colors =
        findAttribute(primitive, cgltf_attribute_type_color);

    Vec3 baseColor { 1.0f };
    if (primitive.material &&
        primitive.material->has_pbr_metallic_roughness) {
        const cgltf_float* factor =
            primitive.material->pbr_metallic_roughness.base_color_factor;
        baseColor = { factor[0], factor[1], factor[2] };
        const cgltf_texture* texture =
            primitive.material->pbr_metallic_roughness.base_color_texture
                .texture;
        if (textureAverages && texture && texture->image &&
            texture->image->uri) {
            baseColor *= averageTextureColor(baseDir, texture->image->uri,
                                             *textureAverages);
        }
    }

    const Mat3 normalMatrix =
        glm::inverseTranspose(Mat3(world));
    const u32 firstVertex = static_cast<u32>(mesh.vertices.size());
    const u32 firstIndex = static_cast<u32>(mesh.indices.size());

    for (cgltf_size v = 0; v < positions->count; ++v) {
        render::MeshVertex vertex {};
        f32 buffer[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        cgltf_accessor_read_float(positions, v, buffer, 3);
        vertex.position =
            Vec3(world * Vec4(buffer[0], buffer[1], buffer[2], 1.0f));
        if (normals && cgltf_accessor_read_float(normals, v, buffer, 3)) {
            vertex.normal = glm::normalize(
                normalMatrix * Vec3(buffer[0], buffer[1], buffer[2]));
        }
        if (uvs && cgltf_accessor_read_float(uvs, v, buffer, 2)) {
            vertex.uv = { buffer[0], buffer[1] };
        }
        vertex.color = baseColor;
        if (colors) {
            buffer[0] = buffer[1] = buffer[2] = 1.0f;
            cgltf_accessor_read_float(colors, v, buffer, 4);
            vertex.color *= Vec3(buffer[0], buffer[1], buffer[2]);
        }
        mesh.vertices.push_back(vertex);
    }

    if (primitive.indices) {
        for (cgltf_size i = 0; i < primitive.indices->count; ++i) {
            mesh.indices.push_back(
                firstVertex +
                static_cast<u32>(cgltf_accessor_read_index(primitive.indices,
                                                           i)));
        }
    } else {
        for (cgltf_size i = 0; i < positions->count; ++i) {
            mesh.indices.push_back(firstVertex + static_cast<u32>(i));
        }
    }

    if (!normals) {
        reconstructNormals(mesh, firstVertex, firstIndex);
    }
}

std::optional<render::MeshData> buildMesh(const cgltf_data& data,
                                          const str& label,
                                          const std::filesystem::path* baseDir) {
    render::MeshData mesh;
    TextureAverages textureAverages;
    for (cgltf_size n = 0; n < data.nodes_count; ++n) {
        const cgltf_node& node = data.nodes[n];
        if (!node.mesh) {
            continue;
        }
        Mat4 world { 1.0f };
        cgltf_node_transform_world(&node, glm::value_ptr(world));
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            appendPrimitive(mesh, node.mesh->primitives[p], world,
                            baseDir ? *baseDir : std::filesystem::path {},
                            baseDir ? &textureAverages : nullptr);
        }
    }
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        LOG_ERROR("glTF '{}': no triangle geometry found", label);
        return std::nullopt;
    }
    return mesh;
}

struct GltfDeleter {
    void operator()(cgltf_data* data) const { cgltf_free(data); }
};

} // namespace

std::optional<render::MeshData> loadGltfMesh(
    const std::filesystem::path& path) {
    const str pathStr = path.string();
    cgltf_options options {};
    cgltf_data* raw = nullptr;
    cgltf_result result = cgltf_parse_file(&options, pathStr.c_str(), &raw);
    if (result != cgltf_result_success) {
        LOG_ERROR("glTF parse failed '{}' (cgltf error {})", pathStr,
                  static_cast<int>(result));
        return std::nullopt;
    }
    std::unique_ptr<cgltf_data, GltfDeleter> data { raw };
    result = cgltf_load_buffers(&options, data.get(), pathStr.c_str());
    if (result != cgltf_result_success) {
        LOG_ERROR("glTF buffer load failed '{}' (cgltf error {})", pathStr,
                  static_cast<int>(result));
        return std::nullopt;
    }
    const std::filesystem::path baseDir = path.parent_path();
    return buildMesh(*data, pathStr, &baseDir);
}

std::optional<render::MeshData> loadGltfMeshFromMemory(const void* bytes,
                                                       u64 byteCount) {
    cgltf_options options {};
    cgltf_data* raw = nullptr;
    cgltf_result result = cgltf_parse(&options, bytes, byteCount, &raw);
    if (result != cgltf_result_success) {
        LOG_ERROR("glTF parse failed (in-memory, cgltf error {})",
                  static_cast<int>(result));
        return std::nullopt;
    }
    std::unique_ptr<cgltf_data, GltfDeleter> data { raw };
    // nullptr base path: only GLB bin chunks and data: URIs can resolve.
    result = cgltf_load_buffers(&options, data.get(), nullptr);
    if (result != cgltf_result_success) {
        LOG_ERROR("glTF buffer load failed (in-memory, cgltf error {})",
                  static_cast<int>(result));
        return std::nullopt;
    }
    return buildMesh(*data, "<memory>", nullptr);
}

// --- Skeletal data ----------------------------------------------------------------

namespace {

struct ParsedGltf {
    cgltf_data* data { nullptr };
    ~ParsedGltf() {
        if (data) {
            cgltf_free(data);
        }
    }
};

bool parseWithBuffers(const std::filesystem::path& path, ParsedGltf& out) {
    const str pathStr = path.string();
    cgltf_options options {};
    if (cgltf_parse_file(&options, pathStr.c_str(), &out.data) !=
        cgltf_result_success) {
        LOG_ERROR("glTF parse failed '{}'", pathStr);
        return false;
    }
    if (cgltf_load_buffers(&options, out.data, pathStr.c_str()) !=
        cgltf_result_success) {
        LOG_ERROR("glTF buffer load failed '{}'", pathStr);
        return false;
    }
    return true;
}

// Skin joints in glTF come in arbitrary order; the anim runtime wants
// parents before children. Returns old-index -> new-index.
vector<u32> topologicalJointOrder(const cgltf_skin& skin) {
    const size_t count = skin.joints_count;
    vector<i32> parent(count, -1);
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < count; ++j) {
            if (skin.joints[i]->parent == skin.joints[j]) {
                parent[i] = static_cast<i32>(j);
            }
        }
    }
    vector<u32> order(count, 0);
    vector<bool> placed(count, false);
    u32 next = 0;
    // O(n^2) worst case — joint counts are tiny.
    while (next < count) {
        for (size_t i = 0; i < count; ++i) {
            if (placed[i]) {
                continue;
            }
            if (parent[i] < 0 || placed[static_cast<size_t>(parent[i])]) {
                order[i] = next++;
                placed[i] = true;
            }
        }
    }
    return order;
}

} // namespace

std::optional<anim::Skeleton> loadGltfSkeleton(
    const std::filesystem::path& path) {
    ParsedGltf gltf;
    if (!parseWithBuffers(path, gltf) || gltf.data->skins_count == 0) {
        return std::nullopt;
    }
    const cgltf_skin& skin = gltf.data->skins[0];
    const vector<u32> order = topologicalJointOrder(skin);

    anim::Skeleton skeleton;
    skeleton.joints.resize(skin.joints_count);
    for (size_t i = 0; i < skin.joints_count; ++i) {
        const cgltf_node* node = skin.joints[i];
        anim::Joint& joint = skeleton.joints[order[i]];
        joint.name = node->name ? node->name : ("joint" + std::to_string(i));
        joint.parent = -1;
        for (size_t j = 0; j < skin.joints_count; ++j) {
            if (node->parent == skin.joints[j]) {
                joint.parent = static_cast<i32>(order[j]);
            }
        }
        if (node->has_translation) {
            joint.bindPosition = { node->translation[0],
                                   node->translation[1],
                                   node->translation[2] };
        }
        if (node->has_rotation) {
            joint.bindRotation.x = node->rotation[0];
            joint.bindRotation.y = node->rotation[1];
            joint.bindRotation.z = node->rotation[2];
            joint.bindRotation.w = node->rotation[3];
        }
        if (node->has_scale) {
            joint.bindScale = { node->scale[0], node->scale[1],
                                node->scale[2] };
        }
        if (skin.inverse_bind_matrices) {
            f32 m[16];
            cgltf_accessor_read_float(skin.inverse_bind_matrices, i, m, 16);
            joint.inverseBind = glm::make_mat4(m);
        }
    }
    return skeleton;
}

vector<GltfClip> loadGltfAnimations(const std::filesystem::path& path,
                                    const anim::Skeleton& skeleton) {
    vector<GltfClip> clips;
    ParsedGltf gltf;
    if (!parseWithBuffers(path, gltf)) {
        return clips;
    }
    for (cgltf_size a = 0; a < gltf.data->animations_count; ++a) {
        const cgltf_animation& animation = gltf.data->animations[a];
        GltfClip named;
        named.name = animation.name ? animation.name : "";
        anim::AnimClip& clip = named.clip;
        clip.name = named.name;
        clip.tracks.resize(skeleton.joints.size());

        for (cgltf_size c = 0; c < animation.channels_count; ++c) {
            const cgltf_animation_channel& channel = animation.channels[c];
            if (!channel.target_node || !channel.target_node->name) {
                continue;
            }
            const i32 joint =
                skeleton.findJoint(channel.target_node->name);
            if (joint < 0) {
                continue; // channel animates a non-skeleton node
            }
            anim::JointTrack& track =
                clip.tracks[static_cast<size_t>(joint)];
            const cgltf_animation_sampler& sampler = *channel.sampler;
            const cgltf_size keys = sampler.input->count;
            vector<f32> times(keys);
            for (cgltf_size k = 0; k < keys; ++k) {
                cgltf_accessor_read_float(sampler.input, k, &times[k], 1);
                clip.duration = glm::max(clip.duration, times[k]);
            }
            if (channel.target_path ==
                cgltf_animation_path_type_translation) {
                track.positionTimes = times;
                track.positions.resize(keys);
                for (cgltf_size k = 0; k < keys; ++k) {
                    f32 v[3];
                    cgltf_accessor_read_float(sampler.output, k, v, 3);
                    track.positions[k] = { v[0], v[1], v[2] };
                }
            } else if (channel.target_path ==
                       cgltf_animation_path_type_rotation) {
                track.rotationTimes = times;
                track.rotations.resize(keys);
                for (cgltf_size k = 0; k < keys; ++k) {
                    f32 v[4];
                    cgltf_accessor_read_float(sampler.output, k, v, 4);
                    Quat q;
                    q.x = v[0]; q.y = v[1]; q.z = v[2]; q.w = v[3];
                    track.rotations[k] = q;
                }
            } else if (channel.target_path ==
                       cgltf_animation_path_type_scale) {
                track.scaleTimes = times;
                track.scales.resize(keys);
                for (cgltf_size k = 0; k < keys; ++k) {
                    f32 v[3];
                    cgltf_accessor_read_float(sampler.output, k, v, 3);
                    track.scales[k] = { v[0], v[1], v[2] };
                }
            }
        }
        clips.push_back(std::move(named));
    }
    return clips;
}

// --- Skinned mesh ----------------------------------------------------------

namespace {

std::optional<render::SkinnedMeshData> buildSkinnedMesh(
    const cgltf_data& data, const str& label) {
    if (data.skins_count == 0) {
        LOG_ERROR("glTF '{}': no skin — not a skinned mesh", label);
        return std::nullopt;
    }
    const cgltf_skin& skin = data.skins[0];
    // The SAME reorder as loadGltfSkeleton: palette index j in the file
    // becomes order[j] in the engine skeleton.
    const vector<u32> order = topologicalJointOrder(skin);

    render::SkinnedMeshData mesh;
    for (cgltf_size n = 0; n < data.nodes_count; ++n) {
        const cgltf_node& node = data.nodes[n];
        if (!node.mesh || node.skin != &skin) {
            continue;
        }
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& primitive = node.mesh->primitives[p];
            const cgltf_accessor* positions =
                findAttribute(primitive, cgltf_attribute_type_position);
            const cgltf_accessor* joints =
                findAttribute(primitive, cgltf_attribute_type_joints);
            const cgltf_accessor* weights =
                findAttribute(primitive, cgltf_attribute_type_weights);
            if (!positions || !joints || !weights ||
                primitive.type != cgltf_primitive_type_triangles) {
                continue;
            }
            const cgltf_accessor* normals =
                findAttribute(primitive, cgltf_attribute_type_normal);
            const cgltf_accessor* uvs =
                findAttribute(primitive, cgltf_attribute_type_texcoord);
            const cgltf_accessor* colors =
                findAttribute(primitive, cgltf_attribute_type_color);

            Vec3 baseColor { 1.0f };
            if (primitive.material &&
                primitive.material->has_pbr_metallic_roughness) {
                const cgltf_float* factor = primitive.material
                                                ->pbr_metallic_roughness
                                                .base_color_factor;
                baseColor = { factor[0], factor[1], factor[2] };
            }

            const u32 firstVertex = static_cast<u32>(mesh.vertices.size());
            for (cgltf_size v = 0; v < positions->count; ++v) {
                render::SkinnedVertex vertex {};
                f32 buffer[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                cgltf_accessor_read_float(positions, v, buffer, 3);
                vertex.position = { buffer[0], buffer[1], buffer[2] };
                vertex.normal = { 0.0f, 1.0f, 0.0f };
                if (normals &&
                    cgltf_accessor_read_float(normals, v, buffer, 3)) {
                    vertex.normal = { buffer[0], buffer[1], buffer[2] };
                }
                if (uvs && cgltf_accessor_read_float(uvs, v, buffer, 2)) {
                    vertex.uv = { buffer[0], buffer[1] };
                }
                vertex.color = baseColor;
                if (colors) {
                    buffer[0] = buffer[1] = buffer[2] = 1.0f;
                    cgltf_accessor_read_float(colors, v, buffer, 4);
                    vertex.color *= Vec3(buffer[0], buffer[1], buffer[2]);
                }
                cgltf_uint jointIndices[4] = { 0, 0, 0, 0 };
                cgltf_accessor_read_uint(joints, v, jointIndices, 4);
                for (int j = 0; j < 4; ++j) {
                    const cgltf_uint remapped =
                        jointIndices[j] < skin.joints_count
                            ? order[jointIndices[j]]
                            : 0;
                    vertex.joints[j] = static_cast<f32>(remapped);
                }
                f32 w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                cgltf_accessor_read_float(weights, v, w, 4);
                const f32 sum = w[0] + w[1] + w[2] + w[3];
                const f32 norm = sum > 1e-6f ? 1.0f / sum : 0.0f;
                vertex.weights = { w[0] * norm, w[1] * norm, w[2] * norm,
                                   w[3] * norm };
                mesh.vertices.push_back(vertex);
            }
            if (primitive.indices) {
                for (cgltf_size i = 0; i < primitive.indices->count; ++i) {
                    mesh.indices.push_back(
                        firstVertex +
                        static_cast<u32>(cgltf_accessor_read_index(
                            primitive.indices, i)));
                }
            } else {
                for (cgltf_size i = 0; i < positions->count; ++i) {
                    mesh.indices.push_back(firstVertex + static_cast<u32>(i));
                }
            }
        }
    }
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        LOG_ERROR("glTF '{}': skin 0 has no skinned triangle geometry",
                  label);
        return std::nullopt;
    }
    return mesh;
}

} // namespace

std::optional<render::SkinnedMeshData> loadGltfSkinnedMesh(
    const std::filesystem::path& path) {
    ParsedGltf gltf;
    if (!parseWithBuffers(path, gltf)) {
        return std::nullopt;
    }
    return buildSkinnedMesh(*gltf.data, path.string());
}

std::optional<render::SkinnedMeshData> loadGltfSkinnedMeshFromMemory(
    const void* bytes, u64 byteCount) {
    cgltf_options options {};
    cgltf_data* raw = nullptr;
    if (cgltf_parse(&options, bytes, byteCount, &raw) !=
        cgltf_result_success) {
        LOG_ERROR("glTF parse failed (in-memory, skinned)");
        return std::nullopt;
    }
    std::unique_ptr<cgltf_data, GltfDeleter> data { raw };
    if (cgltf_load_buffers(&options, data.get(), nullptr) !=
        cgltf_result_success) {
        LOG_ERROR("glTF buffer load failed (in-memory, skinned)");
        return std::nullopt;
    }
    return buildSkinnedMesh(*data, "<memory>");
}

void normalizeMesh(render::MeshData& mesh, f32 targetSize) {
    if (mesh.vertices.empty()) {
        return;
    }
    Vec3 lo = mesh.vertices[0].position;
    Vec3 hi = lo;
    for (const render::MeshVertex& vertex : mesh.vertices) {
        lo = glm::min(lo, vertex.position);
        hi = glm::max(hi, vertex.position);
    }
    const Vec3 size = hi - lo;
    const f32 largest = std::max({ size.x, size.y, size.z });
    const f32 scale = largest > 1e-6f ? targetSize / largest : 1.0f;
    const Vec3 pivot { (lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f };
    for (render::MeshVertex& vertex : mesh.vertices) {
        vertex.position = (vertex.position - pivot) * scale;
    }
}

void groundMesh(render::MeshData& mesh) {
    if (mesh.vertices.empty()) {
        return;
    }
    Vec3 lo = mesh.vertices[0].position;
    Vec3 hi = lo;
    for (const render::MeshVertex& vertex : mesh.vertices) {
        lo = glm::min(lo, vertex.position);
        hi = glm::max(hi, vertex.position);
    }
    const Vec3 pivot { (lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f };
    for (render::MeshVertex& vertex : mesh.vertices) {
        vertex.position -= pivot;
    }
}

} // namespace assets
