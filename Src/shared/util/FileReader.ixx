module;

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

export module kataglyphis.shared.util.file_reader;

export namespace Kataglyphis::Shared {

// True for the DOS device names Windows resolves ANYWHERE in the filesystem:
// "con" is the console, "nul" the bit bucket, "com1" a serial port. Opening
// one does not open a file, and reading it does not end - `readTextFile("con")`
// blocked for 1 h 8 min inside the Windows CI container on 2026-08-05, which
// is how shader_file_reader_fuzz_test found this: the run had to be cancelled,
// and the seed that did it carries the comment "reserved device name on
// Windows". The suite's contract is that a hostile path must not crash OR
// HANG, and this is the hang.
//
// Windows resolves these by the name BEFORE the first dot, in any directory,
// case-insensitively, after stripping trailing spaces and dots - "con",
// "CON.txt", "shaders/con", and "con . " are all the console. So the whole
// family has to be recognised, not the bare word.
//
// The predicate is deliberately platform-INDEPENDENT so it can be tested
// everywhere; only the readers below act on it, and only on Windows, because
// "con" is an ordinary, legal filename on POSIX.
inline bool isWindowsReservedDeviceName(const std::string &file_location)
{
    const std::size_t last_separator = file_location.find_last_of("/\\");
    std::string name = last_separator == std::string::npos ? file_location : file_location.substr(last_separator + 1);

    if (const std::size_t dot = name.find('.'); dot != std::string::npos) { name.erase(dot); }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) { name.pop_back(); }
    std::transform(
      name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static constexpr std::array<std::string_view, 4> kDeviceNames = { "con", "prn", "aux", "nul" };
    if (std::find(kDeviceNames.begin(), kDeviceNames.end(), name) != kDeviceNames.end()) { return true; }

    // COM0-COM9 and LPT0-LPT9. (Windows also accepts the superscript digits
    // for these; they cannot appear in a narrow path here.)
    return name.size() == 4 && (name.compare(0, 3, "com") == 0 || name.compare(0, 3, "lpt") == 0) && name[3] >= '0'
           && name[3] <= '9';
}

// Every reader below refuses anything that is not a REGULAR file, for two
// separate reasons that happen to share one guard:
//
//   - a directory: on POSIX an ifstream opens one happily (see readBinaryFile);
//   - a device: a character device, FIFO or socket opens and then blocks the
//     read forever, /dev/tty being the POSIX twin of Windows' "con".
//
// std::filesystem::is_regular_file with the error_code overload answers both
// without throwing - and throwing is not an option here, see fileExists.
inline bool isReadableRegularFile(const std::string &file_location)
{
#ifdef _WIN32
    // Checked BEFORE touching the filesystem: a DOS device name must never be
    // handed to an open(), and what is_regular_file() reports for one is not
    // something to bet a hang on.
    if (isWindowsReservedDeviceName(file_location)) { return false; }
#endif
    std::error_code ec;
    return std::filesystem::is_regular_file(file_location, ec) && !ec;
}

// The error_code overload is REQUIRED, not stylistic. std::filesystem::exists
// without it throws on any error the OS reports for the query itself - most
// commonly permission denied, which Windows returns for paths such as
// C:\Windows\System32\config\SAM. Exceptions are disabled project-wide
// (/EHs-, -fno-exceptions), so that throw is a terminate, not something a
// caller could ever handle: a shader path pointing somewhere unreadable would
// take down the engine instead of logging "file does not exist".
//
// Found by shader_file_reader_fuzz_test on its first run.
//
// A path we cannot even query is, for every caller here, indistinguishable
// from one that is not there - they all fall back to "return empty and log".
inline bool fileExists(const std::string &file_location)
{
#ifdef _WIN32
    // "con" exists as far as Windows is concerned, but not as a file anything
    // here can read - and a caller that believes it exists goes on to open it.
    if (isWindowsReservedDeviceName(file_location)) { return false; }
#endif
    std::error_code ignored;
    return std::filesystem::exists(file_location, ignored);
}

inline std::string readTextFile(const std::string &file_location)
{
    // The same guard readBinaryFile below has always had. Without it this
    // function would open a directory or a device: "con" on Windows and
    // /dev/tty on POSIX both open cleanly and then never finish being read.
    if (!isReadableRegularFile(file_location)) { return {}; }

    std::string content;
    std::ifstream file_stream(file_location, std::ios::in);

    if (!file_stream.is_open()) { return {}; }

    std::string line;
    while (std::getline(file_stream, line)) { content.append(line).append("\n"); }

    return content;
}

inline std::vector<char> readBinaryFile(const std::string &file_location)
{
    // Reject anything that is not a regular file BEFORE opening it. On POSIX
    // an ifstream opens a directory happily: is_open() is true and, with
    // std::ios::ate, tellg() reports a nonzero size, so the read below
    // allocates that many bytes, fails, and hands the caller a buffer of
    // uninitialised garbage. Windows refuses the open and returns {}. That
    // divergence is not academic - this is the function the engine loads
    // SPIR-V through, and it kept FileReaderUnit.ReadBinaryFileEmptyForDirectoryPath
    // red on the Linux CI lane only. error_code overload for the same reason
    // documented above: exceptions are disabled project-wide. Shared with
    // readTextFile since 2026-08-06, and it now also refuses Windows device
    // names outright rather than trusting is_regular_file's answer for them.
    if (!isReadableRegularFile(file_location)) { return {}; }

    std::ifstream file(file_location, std::ios::binary | std::ios::ate);

    if (!file.is_open()) { return {}; }

    std::streampos const end_pos = file.tellg();
    if (end_pos < 0) { return {}; }

    auto const file_size = static_cast<size_t>(end_pos);
    std::vector<char> file_buffer(file_size);

    file.seekg(0);
    file.read(file_buffer.data(), static_cast<std::streamsize>(file_size));
    return file_buffer;
}

inline std::string getBaseDir(const std::string &file_location)
{
    if (file_location.find_last_of("/\\") != std::string::npos) {
        return file_location.substr(0, file_location.find_last_of("/\\"));
    }
    return {};
}

}// namespace Kataglyphis::Shared
