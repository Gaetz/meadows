#include "engine/anim/GltfSkeleton.hpp"

#include <string>

#include <cgltf.h>

#include <glm/gtc/type_ptr.hpp>

#include "engine/assets/GltfParse.hpp"

namespace anim {

std::optional<Skeleton> loadGltfSkeleton(const std::filesystem::path& path) {
    assets::ParsedGltf gltf;
    if (!assets::parseWithBuffers(path, gltf) || gltf.data->skins_count == 0) {
        return std::nullopt;
    }
    const cgltf_skin& skin = gltf.data->skins[0];
    const vector<u32> order = assets::topologicalJointOrder(skin);

    Skeleton skeleton;
    skeleton.joints.resize(skin.joints_count);
    for (size_t i = 0; i < skin.joints_count; ++i) {
        const cgltf_node* node = skin.joints[i];
        Joint& joint = skeleton.joints[order[i]];
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
                                    const Skeleton& skeleton) {
    vector<GltfClip> clips;
    assets::ParsedGltf gltf;
    if (!assets::parseWithBuffers(path, gltf)) {
        return clips;
    }
    for (cgltf_size a = 0; a < gltf.data->animations_count; ++a) {
        const cgltf_animation& animation = gltf.data->animations[a];
        GltfClip named;
        named.name = animation.name ? animation.name : "";
        AnimClip& clip = named.clip;
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
            JointTrack& track = clip.tracks[static_cast<size_t>(joint)];
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

} // namespace anim
