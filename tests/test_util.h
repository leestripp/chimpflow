// Minimal test harness — zero dependencies by design (project rule: ask
// before adding any dependency).
#pragma once
#include <cmath>
#include <cstdio>

namespace test {
inline int g_pass = 0;
inline int g_fail = 0;
inline void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL %s:%d: %s\n", file, line, expr);
}
inline int summary(const char* suite) {
    std::printf("%s: %d passed, %d failed\n", suite, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
}  // namespace test

#define CHECK(cond) ::test::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::test::check((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) ::test::check(std::fabs((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)
