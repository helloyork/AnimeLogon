// A minimal test harness: CHECK records a failure and carries on.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

namespace check {

struct Case {
    const char *name;
    void (*run)();
};

inline std::vector<Case> &Cases() {
    static std::vector<Case> cases;
    return cases;
}

inline int &Failures() {
    static int failures = 0;
    return failures;
}

struct Register {
    Register(const char *name, void (*run)()) { Cases().push_back({name, run}); }
};

// ANIMELOGON_TESTS=<text> runs only the tests whose names contain it.
inline int RunAll() {
    const char *only = std::getenv("ANIMELOGON_TESTS");
    for (const Case &c : Cases()) {
        if (only && *only && !std::strstr(c.name, only)) continue;
        const int before = Failures();
        c.run();
        std::printf("%s  %s\n", Failures() == before ? "ok  " : "FAIL", c.name);
    }
    std::printf("%d failure(s)\n", Failures());
    return Failures() ? 1 : 0;
}

}  // namespace check

#define TEST(name)                                        \
    static void name();                                   \
    static check::Register name##_register(#name, name);  \
    static void name()

#define CHECK(expr)                                                                   \
    do {                                                                              \
        if (!(expr)) {                                                                \
            std::printf("  %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #expr);           \
            ++check::Failures();                                                      \
        }                                                                             \
    } while (0)
