#include "engine/assets/GltfMesh.hpp"

#include <algorithm>
#include <cstring>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

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

void appendPrimitive(render::MeshData& mesh, const cgltf_primitive& primitive,
                     const Mat4& world) {
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
                                          const str& label) {
    render::MeshData mesh;
    for (cgltf_size n = 0; n < data.nodes_count; ++n) {
        const cgltf_node& node = data.nodes[n];
        if (!node.mesh) {
            continue;
        }
        Mat4 world { 1.0f };
        cgltf_node_transform_world(&node, glm::value_ptr(world));
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            appendPrimitive(mesh, node.mesh->primitives[p], world);
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
    return buildMesh(*data, pathStr);
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
    return buildMesh(*data, "<memory>");
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

} // namespace assets
