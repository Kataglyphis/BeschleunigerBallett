module;

#include <string>

export module kataglyphis.vulkan.app;

export namespace Kataglyphis {
/// What the command line hands the start.
struct AppOptions
{
    /// The model to start with, absolute or below Resources/; empty for KATAGLYPHIS_MODEL_OVERRIDE or the default.
    std::string startupModel;
};

class App
{
  public:
    App();

    static int run(const AppOptions &options = {});

    ~App();
};
}// namespace Kataglyphis
