module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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

export module kataglyphis.shared.util.utf8_path;

export namespace Kataglyphis::Shared {

/// A path as the UTF-8 bytes OxidANT's bridge takes: a Windows std::string is ANSI, which Rust's from_utf8 rejects.
inline std::vector<std::uint8_t> utf8PathBytes(const std::filesystem::path &path)
{
    const std::u8string text = path.u8string();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

/// A path as UTF-8 text, for a log line about a path narrowPath() cannot name.
inline std::string utf8PathText(const std::filesystem::path &path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

/// The narrow name the loaders take, the ANSI code page on Windows; none when that code page cannot hold the path,
/// where MSVC's path::string() throws, which ends this build without exceptions.
inline std::optional<std::string> narrowPath(const std::filesystem::path &path)
{
#if defined(_WIN32)
    const std::wstring &wide = path.native();
    if (wide.empty()) { return std::string(); }
    // As the STL converts: no best-fit stand-ins, which would name another file; UTF-8 takes neither flag nor default.
    const bool utf8 = GetACP() == CP_UTF8;
    const auto flags = static_cast<DWORD>(utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS);
    BOOL lost = FALSE;
    BOOL *const lost_out = utf8 ? nullptr : &lost;
    const int wide_length = static_cast<int>(wide.size());
    const int length = WideCharToMultiByte(CP_ACP, flags, wide.data(), wide_length, nullptr, 0, nullptr, lost_out);
    if (length <= 0 || lost != FALSE) { return std::nullopt; }
    std::string narrow(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_ACP, flags, wide.data(), wide_length, narrow.data(), length, nullptr, lost_out) != length
        || lost != FALSE) {
        return std::nullopt;
    }
    return narrow;
#else
    return path.string();
#endif
}

}// namespace Kataglyphis::Shared
