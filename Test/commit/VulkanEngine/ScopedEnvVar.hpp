#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>

namespace Kataglyphis::TestSupport {

// Sets one environment variable for a scope and restores what was there; the engine reads its knobs at use.
class ScopedEnvVar
{
  public:
    // An empty value unsets the variable: the engine treats empty and unset alike.
    ScopedEnvVar(const char *name, const char *value) : name_(name)
    {
        if (const char *previous = std::getenv(name); previous != nullptr) { previous_ = previous; }
        set(name_.c_str(), value);
    }
    ScopedEnvVar(const ScopedEnvVar &) = delete;
    ScopedEnvVar &operator=(const ScopedEnvVar &) = delete;
    ScopedEnvVar(ScopedEnvVar &&) = delete;
    ScopedEnvVar &operator=(ScopedEnvVar &&) = delete;
    ~ScopedEnvVar() { set(name_.c_str(), previous_.has_value() ? previous_->c_str() : ""); }

  private:
    static void set(const char *name, const char *value)
    {
#ifdef _WIN32
        std::ignore = _putenv_s(name, value);
#else
        if (*value == '\0') {
            std::ignore = unsetenv(name);
        } else {
            std::ignore = setenv(name, value, 1);
        }
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

}// namespace Kataglyphis::TestSupport
