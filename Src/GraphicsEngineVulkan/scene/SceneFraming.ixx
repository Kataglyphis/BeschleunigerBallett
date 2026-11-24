module;

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

export module kataglyphis.vulkan.scene_framing;

export namespace Kataglyphis {

/// A camera the loaded file authors, at its node's world pose.
struct SceneCamera
{
    std::string name;
    glm::vec3 eye{ 0.0F };
    /// The way it looks, a unit vector.
    glm::vec3 forward{ 0.0F, 0.0F, -1.0F };
    /// 0 for an orthographic camera.
    float yfovRadians = 0.0F;
};

/// The loaded model's world-space box; `valid` false while it has no vertices.
struct SceneBounds
{
    bool valid = false;
    glm::vec3 min{ 0.0F };
    glm::vec3 max{ 0.0F };
};

/// What a loaded file says about itself beyond its geometry.
struct SceneMetadata
{
    std::vector<SceneCamera> cameras;
    /// The camera `scenes[scene].extras.active_camera` names, an index into `cameras`, or -1.
    int activeCamera = -1;
    /// `asset.copyright`, else the `ATTRIBUTION.txt` beside the model; empty for none.
    std::string credit;
    SceneBounds bounds;
};

/// Camera's pose in its own convention: degrees, front = (cos pitch cos yaw, sin pitch, cos pitch sin yaw).
struct CameraPose
{
    glm::vec3 eye{ 0.0F };
    float yaw = -90.0F;
    float pitch = 0.0F;
    float fov = 45.0F;
    float nearPlane = 0.1F;
    float farPlane = 100.0F;
    float movementSpeed = 10.0F;
    float shadowDistance = 60.0F;
};

/// Which framing a load got.
enum class FramingSource : int {
    FixedPose = 0,
    ActiveCamera = 1,
    FirstCamera = 2,
    Bounds = 3,
};

struct SceneFraming
{
    bool framed = false;
    FramingSource source = FramingSource::FixedPose;
    /// `fixed` itself when `framed` is false.
    CameraPose pose;
};

/// OxidANT's framing: at the scene's own camera, at its bounds past `fixed`'s depth range, else `fixed` (owner rule).
SceneFraming frameScene(const SceneMetadata &metadata, const CameraPose &fixed);

/// The pose a newly loaded scene starts at: its framing, else `fixed` after a framed scene, else none (the camera stays).
std::optional<CameraPose> startPose(const SceneFraming &framing, const CameraPose &fixed, bool previousFramed)
{
    if (framing.framed) { return framing.pose; }
    if (previousFramed) { return fixed; }
    return std::nullopt;
}

}// namespace Kataglyphis
