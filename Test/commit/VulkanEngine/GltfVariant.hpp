#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Kataglyphis::TestSupport {

// A temp copy of a glTF fixture with an edit applied, for a control the repo need not track.
class GltfVariant
{
  public:
    // Named per process, so two suites on one host never truncate each other's copy; nullopt if unreadable.
    static std::optional<GltfVariant> create(const std::filesystem::path &fixture,
      std::string_view fileName,
      const std::function<void(nlohmann::json &)> &edit)
    {
        std::ifstream in(fixture);
        if (!in.is_open()) { return std::nullopt; }
        std::stringstream text;
        text << in.rdbuf();
        nlohmann::json gltf = nlohmann::json::parse(text.str(), nullptr, false);
        if (gltf.is_discarded() || !gltf.contains("materials") || gltf["materials"].empty()) { return std::nullopt; }
        edit(gltf);

        // The error_code overload: with exceptions off, a throwing one would abort the whole suite.
        std::error_code temp_error;
        const std::filesystem::path temp = std::filesystem::temp_directory_path(temp_error);
        if (temp_error) { return std::nullopt; }
        const std::filesystem::path out =
          temp / ("kataglyphis-" + std::to_string(processId()) + "-" + std::string(fileName));
        std::ofstream file(out, std::ios::trunc);
        file << gltf.dump();
        file.close();
        GltfVariant variant(out);
        if (!file) { return std::nullopt; }
        return variant;
    }

    GltfVariant(const GltfVariant &) = delete;
    GltfVariant &operator=(const GltfVariant &) = delete;
    GltfVariant(GltfVariant &&other) noexcept : path_(std::exchange(other.path_, {})) {}
    GltfVariant &operator=(GltfVariant &&other) noexcept
    {
        if (this != &other) {
            remove();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }
    ~GltfVariant() { remove(); }

    [[nodiscard]] const std::filesystem::path &path() const { return path_; }
    [[nodiscard]] std::string string() const { return path_.string(); }

    // Names a temp file per process, so two suites on one host never share it.
    static long long processId()
    {
#ifdef _WIN32
        return static_cast<long long>(_getpid());
#else
        return static_cast<long long>(getpid());
#endif
    }

  private:
    explicit GltfVariant(std::filesystem::path path) : path_(std::move(path)) {}

    void remove()
    {
        if (path_.empty()) { return; }
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        path_.clear();
    }

    std::filesystem::path path_;
};

}// namespace Kataglyphis::TestSupport
