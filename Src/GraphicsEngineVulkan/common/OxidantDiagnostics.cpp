module;

#include <string>
#include <string_view>

#include "spdlog/spdlog.h"

#include "oxidant_bridge/diagnostics.h"

module kataglyphis.vulkan.oxidant_diagnostics;

namespace Kataglyphis {

namespace {
    namespace diagnostics = kataglyphis::oxidant::diagnostics;

    spdlog::level::level_enum spdlogLevel(diagnostics::LogLevel level)
    {
        switch (level) {
        case diagnostics::LogLevel::Error:
            return spdlog::level::err;
        case diagnostics::LogLevel::Warn:
            return spdlog::level::warn;
        case diagnostics::LogLevel::Info:
            return spdlog::level::info;
        case diagnostics::LogLevel::Debug:
            return spdlog::level::debug;
        default:
            return spdlog::level::trace;
        }
    }

    std::string_view view(const rust::String &text) { return { text.data(), text.size() }; }
}// namespace

std::string oxidantVersion()
{
    std::string version(diagnostics::oxidant_version());
    logOxidantRecords();
    return version;
}

void logOxidantRecords()
{
    for (const diagnostics::LogRecord &record : diagnostics::take_log_records()) {
        spdlog::log(spdlogLevel(record.level), "OxidANT {}: {}", view(record.target), view(record.message));
    }
}

}// namespace Kataglyphis
