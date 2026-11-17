// OxidANT's kataglyphis_cxx_bridge, which every build links: a call that crosses into Rust and back.

#include <gtest/gtest.h>

#include "oxidant_bridge/lib.h"

TEST(RustBridgeUnit, CallsIntoRust) { EXPECT_EQ(kataglyphis::oxidant::rusty_cxxbridge_integer(), 42); }
