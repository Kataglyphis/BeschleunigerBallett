#pragma once

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>

namespace Kataglyphis {

// The variable's value, nullopt when unset; Windows reads it through getenv_s, as its CRT deprecates getenv.
[[nodiscard]] inline auto environmentVariable(const char *name) -> std::optional<std::string>
{
#if defined(_WIN32)
    std::size_t required_size = 0;
    if (getenv_s(&required_size, nullptr, 0, name) != 0 || required_size == 0) { return std::nullopt; }

    std::string value(required_size, '\0');
    if (getenv_s(&required_size, value.data(), value.size(), name) != 0 || required_size == 0) { return std::nullopt; }

    value.resize(required_size - 1);
    return value;
#else
    const char *value = std::getenv(name);
    if (value == nullptr) { return std::nullopt; }
    return std::string(value);
#endif
}

// Empty counts as unset, as for every engine knob and OxidANT's load_options_from_env.
[[nodiscard]] inline auto environmentFlag(const char *name) -> bool
{
    const std::optional<std::string> value = environmentVariable(name);
    return value.has_value() && !value->empty();
}

}// namespace Kataglyphis
