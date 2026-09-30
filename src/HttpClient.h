#pragma once

#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct HttpResponse {
    int status = 0;     // 0: never got a response (DNS, connect, TLS, timeout)
    DWORD error = 0;    // WinHTTP error code when status == 0
    std::string body;

    bool Ok() const { return status >= 200 && status < 300; }
    std::wstring Describe() const;  // for notifications and upload_error
};

// Minimal synchronous HTTPS client for the Speakr API. Redirects are disabled
// so an auth problem surfaces as a status code, not a login page.
class HttpClient {
public:
    using Fields = std::vector<std::pair<std::string, std::string>>;

    HttpClient(const std::wstring& baseUrl, std::string bearerToken);
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse Get(const std::wstring& path);
    HttpResponse SendJson(const wchar_t* method, const std::wstring& path, const std::string& json);
    // multipart/form-data with text fields plus one file, streamed from disk.
    HttpResponse PostMultipart(const std::wstring& path, const Fields& fields, const std::string& fileField,
                               const std::wstring& filePath, const std::string& fileName,
                               const std::string& fileType);

    // Cancels the request in progress (callable from another thread); it
    // fails with ERROR_WINHTTP_OPERATION_CANCELLED, as do later requests.
    void Abort();

private:
    bool BeginRequest(HINTERNET request);
    void EndRequest(HINTERNET request);

    HttpResponse Send(const wchar_t* method, const std::wstring& path, const std::wstring& contentType,
                      const std::string& head, const std::wstring& filePath, const std::string& tail);

    HINTERNET session_ = nullptr;
    HINTERNET connection_ = nullptr;
    bool secure_ = true;
    std::wstring basePath_;
    std::string token_;

    std::mutex requestMutex_;
    HINTERNET activeRequest_ = nullptr;
    bool aborted_ = false;
};
