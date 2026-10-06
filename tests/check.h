// A minimal test harness: CHECK records a failure and carries on.
#pragma once

#include <cstdio>
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

inline int RunAll() {
    for (const Case &c : Cases()) {
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
