module;

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

export module kataglyphis.shared.util.utf8_path;

export namespace Kataglyphis::Shared {

/// A path as the UTF-8 bytes OxidANT's bridge takes: a Windows std::string is ANSI, which Rust's from_utf8 rejects.
inline std::vector<std::uint8_t> utf8PathBytes(const std::filesystem::path &path)
{
    const std::u8string text = path.u8string();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

}// namespace Kataglyphis::Shared
