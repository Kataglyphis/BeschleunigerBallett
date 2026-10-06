// Must precede fuzztest.h, which friend-declares them unincluded; a force-include would break module BMI synthesis.
#include "absl/random/internal/distribution_caller.h"// IWYU pragma: keep
#include "absl/random/internal/mock_helpers.h"// IWYU pragma: keep

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include <string>

// An example function we want to fuzz test
bool IsPalindrome(const std::string &s)
{
    if (s.empty()) return true;
    int left = 0;
    int right = s.size() - 1;
    while (left < right) {
        if (s[left] != s[right]) return false;
        left++;
        right--;
    }
    return true;
}

// Property: Reversing a string and appending it to itself should always produce a palindrome
void ReversingAndAppendingCreatesPalindrome(const std::string &input)
{
    std::string reversed(input.rbegin(), input.rend());
    std::string candidate = input + reversed;
    EXPECT_TRUE(IsPalindrome(candidate));
}

FUZZ_TEST(PalindromeFuzzTest, ReversingAndAppendingCreatesPalindrome);
