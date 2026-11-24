module;

#include <glm/glm.hpp>

#include <array>
#include <vector>

#include "oxidant_bridge/gltf.h"

module kataglyphis.vulkan.scene_framing;

namespace Kataglyphis {

namespace {
    namespace ox = kataglyphis::oxidant::gltf;

    std::array<float, 3> toArray(const glm::vec3 &value) { return { value.x, value.y, value.z }; }

    glm::vec3 toVec3(const std::array<float, 3> &value) { return { value[0], value[1], value[2] }; }

    ox::CameraPose toBridge(const CameraPose &pose)
    {
        return ox::CameraPose{ .eye = toArray(pose.eye),
            .yaw_deg = pose.yaw,
            .pitch_deg = pose.pitch,
            .fov_y_deg = pose.fov,
            .near_plane = pose.nearPlane,
            .far_plane = pose.farPlane,
            .fly_speed = pose.movementSpeed,
            .shadow_distance = pose.shadowDistance };
    }

    CameraPose fromBridge(const ox::CameraPose &pose)
    {
        return CameraPose{ .eye = toVec3(pose.eye),
            .yaw = pose.yaw_deg,
            .pitch = pose.pitch_deg,
            .fov = pose.fov_y_deg,
            .nearPlane = pose.near_plane,
            .farPlane = pose.far_plane,
            .movementSpeed = pose.fly_speed,
            .shadowDistance = pose.shadow_distance };
    }
}// namespace

SceneFraming frameScene(const SceneMetadata &metadata, const CameraPose &fixed)
{
    std::vector<ox::GltfCamera> cameras;
    cameras.reserve(metadata.cameras.size());
    for (const SceneCamera &camera : metadata.cameras) {
        cameras.push_back(ox::GltfCamera{ .name = rust::String(camera.name),
          .eye = toArray(camera.eye),
          .forward = toArray(camera.forward),
          .yfov_rad = camera.yfovRadians });
    }
    const ox::GltfBounds bounds{
        .valid = metadata.bounds.valid, .min = toArray(metadata.bounds.min), .max = toArray(metadata.bounds.max)
    };
    const ox::SceneFraming framing = ox::frame_scene(rust::Slice<const ox::GltfCamera>(cameras.data(), cameras.size()),
      metadata.activeCamera,
      bounds,
      toBridge(fixed));
    return SceneFraming{
        .framed = framing.framed, .source = static_cast<FramingSource>(framing.source), .pose = fromBridge(framing.pose)
    };
}

}// namespace Kataglyphis
