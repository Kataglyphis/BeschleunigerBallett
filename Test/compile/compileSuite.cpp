#include <gtest/gtest.h>

// Demonstrate some basic assertions.
TEST(HelloTestCompile, BasicAssertions)
{

    // Expect two strings not to be equal.
    EXPECT_STRNE("hello", "world");
    // Expect equality.
    EXPECT_EQ(7 * 6, 42);
}

TEST(HelloTestCompile, blob)
{

    int c = 0;

    // Test that counter 0 returns 0
    EXPECT_EQ(0, c);

    // EXPECT_EQ() evaluates its arguments exactly once, so they can have side effects.

    EXPECT_EQ(0, c++);
    EXPECT_EQ(1, c++);
    EXPECT_EQ(2, c++);

    EXPECT_EQ(3, c++);
}