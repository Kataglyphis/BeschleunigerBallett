module;

#include <ostream>

export module kataglyphis.vulkan.installed_resources;

export namespace Kataglyphis {

// --check_resources: resolves and reads what a start needs, without a window or a GPU; 0 when all of it is there.
auto checkInstalledResources(std::ostream &out) -> int;

}// namespace Kataglyphis
