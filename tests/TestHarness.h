#pragma once

// A tiny self-contained test harness: TEST(name) { CHECK(...); } registers a
// test, CHECK/CHECK_EQ record failures without stopping the test, and main()
// (TestMain.cpp) prints a summary and returns non-zero if anything failed.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

std::vector<TestCase>& Registry();
void RecordFailure(const char* file, int line, const std::string& message);
void RecordCheck();

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { Registry().push_back({name, std::move(body)}); }
};

inline std::string Show(const std::string& v) { return "\"" + v + "\""; }
inline std::string Show(const char* v) { return "\"" + std::string(v) + "\""; }
inline std::string Show(const std::wstring& v) {
    std::string out = "L\"";
    for (wchar_t c : v) {
        if (c >= 32 && c < 127) {
            out += static_cast<char>(c);
        } else {
            char buf[16];
            sprintf_s(buf, "\\x%04x", static_cast<unsigned>(c));
            out += buf;
        }
    }
    return out + "\"";
}
inline std::string Show(const wchar_t* v) { return Show(std::wstring(v)); }
inline std::string Show(bool v) { return v ? "true" : "false"; }
template <typename T>
std::string Show(const T& v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

template <typename A, typename B>
void CheckEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    RecordCheck();
    if (!(a == b)) {
        RecordFailure(file, line,
                      std::string(ea) + " == " + eb + "\n      actual:   " + Show(a) + "\n      expected: " + Show(b));
    }
}

}  // namespace testing

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name)                                                                                    \
    static void TEST_CONCAT(TestBody_, name)();                                                       \
    static const testing::Registrar TEST_CONCAT(TestReg_, name)(#name, TEST_CONCAT(TestBody_, name)); \
    static void TEST_CONCAT(TestBody_, name)()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        testing::RecordCheck();                                                      \
        if (!(cond)) testing::RecordFailure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)
#define CHECK_EQ(actual, expected) testing::CheckEq((actual), (expected), #actual, #expected, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected, eps) CHECK(std::abs((actual) - (expected)) <= (eps))
// Fails the test if the expression throws (the app's decode paths must never).
#define CHECK_NOTHROW(expr)                                                         \
    do {                                                                            \
        testing::RecordCheck();                                                     \
        try {                                                                       \
            (void)(expr);                                                           \
        } catch (...) {                                                             \
            testing::RecordFailure(__FILE__, __LINE__, "threw: " #expr);            \
        }                                                                           \
    } while (0)

// A fresh empty directory under %TEMP%, removed (with its contents) on scope exit.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::wstring& Path() const { return path_; }
    std::wstring File(const std::wstring& name) const { return path_ + L"\\" + name; }

private:
    std::wstring path_;
};

bool FileExists(const std::wstring& path);
void WriteRaw(const std::wstring& path, const std::string& bytes);
std::string ReadRaw(const std::wstring& path);
