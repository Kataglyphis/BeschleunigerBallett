module;
#include <optional>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>
#include <vulkan/vulkan.hpp>

#include "spdlog/spdlog.h"

module kataglyphis.vulkan.scene;

import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.model;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.object_description;
import kataglyphis.vulkan.scene_config;

namespace Kataglyphis {


Scene::Scene() = default;

Scene::Scene(GltfLoadOptions gltfLoadOptions) : gltfLoadOptions(gltfLoadOptions) {}

void Scene::loadModel(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool)
{
    std::string const modelFileName = sceneConfig::getModelFile();
    spdlog::info("Loading model: {}", modelFileName);
    GltfLoader loader(device, commandPool);
    std::shared_ptr<Model> const new_model = loader.loadModel(modelFileName, gltfLoadOptions);

    if (new_model) {
        const std::optional<uint32_t> model_index = add_model(new_model);
        spdlog::info("Model added successfully.");
        if (model_index) {
            glm::mat4 const modelMatrix = sceneConfig::getModelMatrix();
            update_model_matrix(modelMatrix, *model_index);
            loadedModelPath = sceneConfig::startupModelRelativePath();
        }
    } else {
        spdlog::error("Failed to load model: {}", modelFileName);
        return;
    }
}

std::optional<uint32_t> Scene::loadAdditionalModel(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool,
  const std::string &modelPath,
  const glm::mat4 &modelMatrix)
{
    // Paths are relative to Resources/, and the app and tests run from different working directories.
    const std::string resolved = sceneConfig::resolveModelPath(modelPath);
    spdlog::info("Loading additional model: {}", resolved);
    GltfLoader loader(device, commandPool);
    std::shared_ptr<Model> const new_model = loader.loadModel(resolved, gltfLoadOptions);
    if (!new_model) {
        spdlog::error("Failed to load additional model: {}", modelPath);
        return std::nullopt;
    }

    const std::optional<uint32_t> model_index = add_model(new_model);
    if (!model_index) { return std::nullopt; }
    update_model_matrix(modelMatrix, *model_index);
    spdlog::info("Additional model added at index {}.", *model_index);
    return model_index;
}

void Scene::beginModelLoadAsync()
{
    // Kept relative, as the library lists it: the editor marks the loaded model by this path once it arrives.
    pendingModelPath = sceneConfig::startupModelRelativePath();
    const std::string modelFileName = sceneConfig::resolveModelPath(pendingModelPath);
    spdlog::info("Loading model asynchronously: {}", modelFileName);
    pendingModelParse.start(modelFileName, gltfLoadOptions);
    modelLoadPending = true;
}

bool Scene::isModelLoadPending() const { return modelLoadPending; }

bool Scene::pollModelLoad(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool)
{
    if (!modelLoadPending || !pendingModelParse.isFinished()) { return false; }

    modelLoadPending = false;

    // Only the GPU upload runs here, on the device thread; the worker already parsed.
    std::shared_ptr<Model> new_model;
    std::unique_ptr<GltfLoader> parsed = pendingModelParse.takeResult();
    if (!parsed) {
        spdlog::error("Asynchronous model parse failed; the scene stays empty.");
        return false;
    }
    GltfLoader uploader(device, commandPool);
    uploader.adoptParsed(std::move(*parsed));
    new_model = uploader.uploadParsed();
    if (!new_model) {
        spdlog::error("Uploading the parsed model failed.");
        return false;
    }

    const std::optional<uint32_t> model_index = add_model(new_model);
    if (model_index) {
        update_model_matrix(sceneConfig::getModelMatrix(), *model_index);
        loadedModelPath = pendingModelPath;
    }
    spdlog::info("Model added successfully (async).");
    return true;
}

std::optional<uint32_t> Scene::add_model(const std::shared_ptr<Model> &model)
{
    // Loaders return nullptr for malformed assets.
    if (!model) {
        spdlog::error("add_model called with a null model - a load failed upstream; scene unchanged.");
        return std::nullopt;
    }
    model_list.push_back(model);
    // One object description per mesh, flattened across models; objectIndex indexes this list.
    for (uint32_t k = 0; k < model->getMeshCount(); ++k) {
        object_descriptions.push_back(model->getMesh(k)->getObjectDescription());
    }
    return getModelCount() - 1U;
}

void Scene::update_model_matrix(glm::mat4 model_matrix, uint32_t model_id)
{
    Model *model = findModel(model_id);
    if (model == nullptr) {
        spdlog::error("Wrong model id value! model_id: {}, model_count: {}", model_id, getModelCount());
        return;
    }

    model->set_model(model_matrix);
}

void Scene::cancelPendingModelLoad()
{
    if (!modelLoadPending) { return; }

    pendingModelParse.waitForCompletion();
    pendingModelParse.takeResult();
    modelLoadPending = false;
    spdlog::info("Discarded an in-flight asynchronous model parse; the scene is being replaced.");
}

void Scene::reloadModel(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool,
  const std::string &modelPath)
{
    cancelPendingModelLoad();
    cleanUp();
    model_list.clear();
    object_descriptions.clear();
    loadedModelPath.clear();

    // Paths are relative to Resources/, and the app and tests run from different working directories.
    const std::string resolved = sceneConfig::resolveModelPath(modelPath);

    GltfLoader loader(device, commandPool);
    std::shared_ptr<Model> const new_model = loader.loadModel(resolved, gltfLoadOptions);
    const std::optional<uint32_t> model_index = add_model(new_model);
    if (!model_index) {
        spdlog::error("reloadModel: '{}' (resolved: '{}') failed to load; scene is now empty.", modelPath, resolved);
        return;
    }

    glm::mat4 const modelMatrix = sceneConfig::getModelMatrix();
    update_model_matrix(modelMatrix, *model_index);
    loadedModelPath = modelPath;
}

void Scene::cleanUp()
{
    for (std::shared_ptr<Model> const &model : model_list) { model->cleanUp(); }
}

Scene::~Scene() { cleanUp(); }

}// namespace Kataglyphis
