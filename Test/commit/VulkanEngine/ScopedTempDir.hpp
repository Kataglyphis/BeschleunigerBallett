#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "GltfVariant.hpp"

namespace Kataglyphis::TestSupport {

/// An empty folder under the temp directory, per process and name, removed with everything in it.
class ScopedTempDir
{
  public:
    explicit ScopedTempDir(std::string_view name)
    {
        std::error_code error;
        const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
        if (error) { return; }
        path_ = temp / ("kataglyphis-" + std::to_string(GltfVariant::processId()) + "-" + std::string(name));
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        if (error) { path_.clear(); }
    }
    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;
    ~ScopedTempDir()
    {
        if (path_.empty()) { return; }
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    /// Empty when the folder could not be made.
    [[nodiscard]] const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

}// namespace Kataglyphis::TestSupport
