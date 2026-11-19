module;

#include <string>

export module kataglyphis.vulkan.oxidant_diagnostics;

// The C++ face of OxidANT's `diagnostics` bridge; the generated header stays in the .cpp, out of every interface.
export namespace Kataglyphis {

/// OxidANT's version as its bridge reports it, such as "3.0.0"; empty only if the call panicked.
std::string oxidantVersion();

/// Logs through spdlog what this thread's bridged calls logged; call it on that thread after each call.
void logOxidantRecords();

}// namespace Kataglyphis
