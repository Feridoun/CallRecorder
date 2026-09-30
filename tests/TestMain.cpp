#include "TestHarness.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace testing {

static int g_checks = 0;
static int g_failures = 0;
bool g_currentFailed = false;

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void RecordCheck() { ++g_checks; }

void RecordFailure(const char* file, int line, const std::string& message) {
    ++g_failures;
    g_currentFailed = true;
    std::printf("  FAIL %s(%d): %s\n", file, line, message.c_str());
}

}  // namespace testing

TempDir::TempDir() {
    wchar_t temp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    static int counter = 0;
    path_ = std::wstring(temp, n) + L"CallRecorderTests_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(++counter);
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void WriteRaw(const std::wstring& path, const std::string& bytes) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadRaw(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    std::string filter = argc > 1 ? argv[1] : "";  // optional substring of a test name
    int run = 0;
    int failedTests = 0;
    for (const auto& test : testing::Registry()) {
        if (!filter.empty() && std::string(test.name).find(filter) == std::string::npos) continue;
        ++run;
        testing::g_currentFailed = false;
        try {
            test.body();
        } catch (const std::exception& e) {
            testing::RecordFailure(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            testing::RecordFailure(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        if (testing::g_currentFailed) {
            ++failedTests;
            std::printf("[ FAILED ] %s\n", test.name);
        }
    }
    std::printf("\n%d tests, %d passed, %d failed (%d checks, %d failed)\n", run, run - failedTests, failedTests,
                testing::g_checks, testing::g_failures);
    return failedTests == 0 && run > 0 ? 0 : 1;
}
