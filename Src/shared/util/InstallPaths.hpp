#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// A plain header, not a module, so scripts/linux/tests/test-install-paths.sh can compile it with any C++ compiler.
namespace Kataglyphis::Shared {

inline constexpr std::string_view kApplicationDirectoryName = "BeschleunigerBallett";

// The folder of the running program's file; a Linux package's launcher starts it, or its bundled loader, in lib/<name>/.
inline auto executableDirectory() -> std::optional<std::filesystem::path>
{
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    while (buffer.size() <= 32768) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) { return std::nullopt; }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::nullopt;
#elif defined(__linux__)
    std::error_code error;
    const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || executable.empty()) { return std::nullopt; }
    return executable.parent_path();
#else
    return std::nullopt;
#endif
}

// bin/.. for every package, the program's own folder for a flat copy, lib/<name>/../.. for a Linux bundle.
inline auto executableRootCandidates(const std::filesystem::path &executableDir) -> std::vector<std::filesystem::path>
{
    const std::filesystem::path parent = executableDir.parent_path();
    return { parent, executableDir, parent.parent_path() };
}

inline auto isExistingDirectory(const std::filesystem::path &path) -> bool
{
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

// The install roots beside the executable, nearest first; only a root holding Resources/ counts.
inline auto installRootsFromExecutable(const std::optional<std::filesystem::path> &executableDir)
  -> std::vector<std::filesystem::path>
{
    std::vector<std::filesystem::path> roots;
    if (!executableDir.has_value() || executableDir->empty()) { return roots; }
    for (const std::filesystem::path &candidate : executableRootCandidates(*executableDir)) {
        if (!candidate.empty() && isExistingDirectory(candidate / "Resources")) { roots.push_back(candidate); }
    }
    return roots;
}

// The installed tree beside the executable comes first; the working directory's roots only find a source tree.
inline auto orderedSearchRoots(const std::vector<std::filesystem::path> &executableRoots,
  const std::vector<std::filesystem::path> &workingDirectoryRoots) -> std::vector<std::filesystem::path>
{
    std::vector<std::filesystem::path> roots = executableRoots;
    roots.insert(roots.end(), workingDirectoryRoots.begin(), workingDirectoryRoots.end());
    return roots;
}

// The first root under which `relative` exists; a ".." in it is refused, so a fragment cannot leave the tree.
inline auto resolveUnderRoots(const std::vector<std::filesystem::path> &roots, const std::filesystem::path &relative)
  -> std::optional<std::filesystem::path>
{
    for (const auto &part : relative) {
        if (part == "..") { return std::nullopt; }
    }
    for (const std::filesystem::path &root : roots) {
        std::error_code error;
        const std::filesystem::path candidate = root / relative;
        if (std::filesystem::exists(candidate, error) && !error) { return candidate; }
    }
    return std::nullopt;
}

enum class UserDirectoryKind { Cache, Logs };

// The variables the per-user directories derive from; a test fills it by hand.
struct UserDirectoryEnvironment
{
    std::optional<std::filesystem::path> localAppData;
    std::optional<std::filesystem::path> xdgCacheHome;
    std::optional<std::filesystem::path> xdgStateHome;
    std::optional<std::filesystem::path> home;
};

// The XDG spec calls a relative value invalid, so it falls through to the default under HOME.
inline auto absoluteOrNothing(const std::optional<std::filesystem::path> &value) -> std::optional<std::filesystem::path>
{
    if (!value.has_value() || value->empty() || !value->is_absolute()) { return std::nullopt; }
    return value;
}

// %LOCALAPPDATA%\<app>\cache|logs on Windows; $XDG_CACHE_HOME/<app> and $XDG_STATE_HOME/<app> elsewhere.
inline auto perUserDirectory(UserDirectoryKind kind, const UserDirectoryEnvironment &environment, bool windows)
  -> std::optional<std::filesystem::path>
{
    const std::filesystem::path app{ std::string(kApplicationDirectoryName) };
    if (windows) {
        const auto base = absoluteOrNothing(environment.localAppData);
        if (!base.has_value()) { return std::nullopt; }
        return *base / app / (kind == UserDirectoryKind::Cache ? "cache" : "logs");
    }
    const auto xdg =
      absoluteOrNothing(kind == UserDirectoryKind::Cache ? environment.xdgCacheHome : environment.xdgStateHome);
    if (xdg.has_value()) { return *xdg / app; }
    const auto home = absoluteOrNothing(environment.home);
    if (!home.has_value()) { return std::nullopt; }
    return kind == UserDirectoryKind::Cache ? *home / ".cache" / app : *home / ".local" / "state" / app;
}

inline auto environmentPath(const char *name) -> std::optional<std::filesystem::path>
{
#if defined(_WIN32)
    // The wide variable: a user name outside the ANSI code page would not survive getenv.
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
    wchar_t *value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, wide_name.c_str()) != 0 || value == nullptr) { return std::nullopt; }
    std::filesystem::path result(value);
    std::free(value);
    return result;
#else
    const char *value = std::getenv(name);
    if (value == nullptr) { return std::nullopt; }
    return std::filesystem::path(value);
#endif
}

inline auto currentUserDirectoryEnvironment() -> UserDirectoryEnvironment
{
    return UserDirectoryEnvironment{ environmentPath("LOCALAPPDATA"),
        environmentPath("XDG_CACHE_HOME"),
        environmentPath("XDG_STATE_HOME"),
        environmentPath("HOME") };
}

inline auto userDirectory(UserDirectoryKind kind) -> std::optional<std::filesystem::path>
{
#if defined(_WIN32)
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    return perUserDirectory(kind, currentUserDirectoryEnvironment(), windows);
}

}// namespace Kataglyphis::Shared
