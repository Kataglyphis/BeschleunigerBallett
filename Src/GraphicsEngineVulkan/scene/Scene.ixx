module;
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.scene;

import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.model;
import kataglyphis.vulkan.obj_loader;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.mesh;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.async_model_parse;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.object_description;
import kataglyphis.vulkan.scene_config;

export namespace Kataglyphis {
class Scene
{
  public:
    Scene();

    void update_model_matrix(glm::mat4 model_matrix, uint32_t model_id);

    // The same `model_index >= model_list.size()` rule was spelled out by
    // hand in eleven places across three files: ten Scene.ixx accessors and
    // Scene.cpp's update_model_matrix, plus the mesh half repeated per-call
    // in MeshDrawRecorder.cpp's and CascadedShadowMap.cpp's per-mesh record
    // loops (four Scene calls per mesh, each re-deriving the same model and
    // mesh pointer). findModel()/findMesh() are the one definition; every
    // fallback value stays byte-identical to what it replaced - in
    // particular getMeshBounds()'s inverted box, which isVisible() treats as
    // visible, so a missing bound must never skip a draw.
    //
    // Public, not private: MeshDrawRecorder and CascadedShadowMap are
    // separate translation units and hoist their own Mesh* with these.
    [[nodiscard]] Model *findModel(uint32_t model_index) const
    { return model_index < model_list.size() ? model_list[static_cast<size_t>(model_index)].get() : nullptr; };
    /// findModel() chained through Model::getMesh(), which is already
    /// nullptr out of range - so the mesh half of the rule is one null
    /// test, not a second bounds check.
    [[nodiscard]] Mesh *findMesh(uint32_t model_index, uint32_t mesh_index) const
    {
        Model *model = findModel(model_index);
        return model != nullptr ? model->getMesh(static_cast<size_t>(mesh_index)) : nullptr;
    };

    std::vector<Texture> &getTextures(uint32_t model_index)
    {
        Model *model = findModel(model_index);
        if (model == nullptr) {
            static std::vector<Texture> empty;
            return empty;
        }
        return model->getTextures();
    };
    std::vector<vk::Sampler> &getTextureSampler(uint32_t model_index)
    {
        Model *model = findModel(model_index);
        if (model == nullptr) {
            static std::vector<vk::Sampler> empty;
            return empty;
        }
        return model->getTextureSamplers();
    };
    uint32_t getTextureCount(uint32_t model_index) const
    {
        Model *model = findModel(model_index);
        return model != nullptr ? model->getTextureCount() : 0;
    };
    uint32_t getModelCount() const { return static_cast<uint32_t>(model_list.size()); };
    glm::mat4 getModelMatrix(uint32_t model_index) const
    {
        Model *model = findModel(model_index);
        return model != nullptr ? model->getModel() : glm::mat4(1.0f);
    };
    uint32_t getMeshCount(uint32_t model_index) const
    {
        Model *model = findModel(model_index);
        return model != nullptr ? static_cast<uint32_t>(model->getMeshCount()) : 0;
    };
    // This ordering is the contract assignTextureOffsets and
    // planFlattenedTextureSlots are both indexed by; consumers must use this
    // accessor rather than re-deriving the per-model vector themselves.
    std::vector<uint32_t> getTextureCountPerModel() const
    {
        std::vector<uint32_t> counts;
        counts.reserve(model_list.size());
        for (uint32_t i = 0; i < static_cast<uint32_t>(model_list.size()); ++i) {
            counts.push_back(getTextureCount(i));
        }
        return counts;
    };
    std::vector<uint32_t> getMeshCountPerModel() const
    {
        std::vector<uint32_t> counts;
        counts.reserve(model_list.size());
        for (uint32_t i = 0; i < static_cast<uint32_t>(model_list.size()); ++i) { counts.push_back(getMeshCount(i)); }
        return counts;
    };
    vk::Buffer getVertexBuffer(uint32_t model_index, uint32_t mesh_index) const
    {
        Mesh *mesh = findMesh(model_index, mesh_index);
        return mesh != nullptr ? mesh->getVertexBuffer() : vk::Buffer{};
    };
    vk::Buffer getIndexBuffer(uint32_t model_index, uint32_t mesh_index) const
    {
        Mesh *mesh = findMesh(model_index, mesh_index);
        return mesh != nullptr ? mesh->getIndexBuffer() : vk::Buffer{};
    };
    uint32_t getIndexCount(uint32_t model_index, uint32_t mesh_index) const
    {
        Mesh *mesh = findMesh(model_index, mesh_index);
        return mesh != nullptr ? mesh->getIndexCount() : 0;
    };
    /// glTF material.doubleSided for a mesh, so the raster pass can disable
    /// back-face culling for it. Out-of-range defaults to single-sided.
    bool isMeshDoubleSided(uint32_t model_index, uint32_t mesh_index) const
    {
        Mesh *mesh = findMesh(model_index, mesh_index);
        return mesh != nullptr && mesh->isDoubleSided();
    };
    /// Object-space bounds of a mesh, for frustum culling. An invalid box is
    /// returned for an out-of-range index, which isVisible() treats as
    /// visible - a missing bound must never be a reason to skip a draw.
    const AABB &getMeshBounds(uint32_t model_index, uint32_t mesh_index) const
    {
        static const AABB unknown{ glm::vec3(1.0F), glm::vec3(-1.0F) };
        Mesh *mesh = findMesh(model_index, mesh_index);
        return mesh != nullptr ? mesh->getBounds() : unknown;
    };
    std::vector<ObjectDescription> getObjectDescriptions() const { return object_descriptions; };
    std::vector<std::shared_ptr<Model>> const &get_model_list() const { return model_list; };

    void loadModel(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool);

    /// Starts parsing the configured model on a worker thread and returns
    /// immediately. The scene has NO model until pollModelLoad() reports one.
    ///
    /// The parse is ~2800 ms of the ~2815 ms load (measured on the bundled
    /// 27 MB model), and the window froze for all of it.
    void beginModelLoadAsync();

    /// True between beginModelLoadAsync() and the model being installed.
    [[nodiscard]] bool isModelLoadPending() const;

    /// Finishes a completed async load: uploads on THIS thread (the one that
    /// owns the device) and adds the model. Returns true on the frame the
    /// model becomes available, so the caller can rebuild whatever depends on
    /// scene contents. Cheap to call every frame.
    bool pollModelLoad(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool);

    /// Joins and discards a still-running beginModelLoadAsync() parse. A
    /// no-op when no parse is pending. reloadModel() calls this first so a
    /// reload issued mid-parse cannot let the startup load land in the scene
    /// alongside the reloaded model.
    void cancelPendingModelLoad();

    /// Replaces the scene with a single model. modelPath is relative to
    /// Resources/ and is resolved here, like loadAdditionalModel()'s
    /// contract. The previous scene is torn down before the new load is
    /// attempted, so a failed reload leaves the scene empty by design.
    void reloadModel(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandPool commandPool,
      const std::string &modelPath);

    /// Loads an ADDITIONAL model, leaving existing ones in place, and returns
    /// its model index (the value the raster paths push as objectIndex) or
    /// std::nullopt if loading failed.
    ///
    /// loadModel() deliberately loads exactly one model from SceneConfig and
    /// reloadModel() replaces the scene, so until now nothing could produce a
    /// scene with two models - which meant the per-draw objectIndex could not
    /// be exercised even though the shaders index with it.
    std::optional<uint32_t> loadAdditionalModel(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandPool commandPool,
      const std::string &modelPath,
      const glm::mat4 &modelMatrix);

    /// Appends model to the scene and returns the index it landed at, or
    /// std::nullopt for the null-model guard (a failed load upstream) - the
    /// caller no longer has to re-derive "which model did I just add".
    std::optional<uint32_t> add_model(const std::shared_ptr<Model> &model);

    void cleanUp();
    ~Scene();

  private:
    AsyncModelParse pendingModelParse;
    bool modelLoadPending{ false };
    std::vector<ObjectDescription> object_descriptions;
    std::vector<std::shared_ptr<Model>> model_list;
};
}// namespace Kataglyphis
