// Must precede fuzztest.h, which friend-declares them unincluded; a force-include would break module BMI synthesis.
#include "absl/random/internal/distribution_caller.h"// IWYU pragma: keep
#include "absl/random/internal/mock_helpers.h"// IWYU pragma: keep

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

TEST(MyTestSuite, OnePlustTwoIsTwoPlusOne) { EXPECT_EQ(1 + 2, 2 + 1); }

void IntegerAdditionCommutes(int a, int b) { EXPECT_EQ(a + b, b + a); }
FUZZ_TEST(MyTestSuite, IntegerAdditionCommutes);