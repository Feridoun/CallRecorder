#include "HttpClient.h"

#include "Util.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <random>

namespace {

constexpr size_t kChunkSize = 64 * 1024;

HttpResponse Failure() {
    HttpResponse response;
    response.error = GetLastError();
    return response;
}

}  // namespace

std::wstring HttpResponse::Describe() const {
    if (status == 0) {
        switch (error) {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"Server name not found";
            case ERROR_WINHTTP_CANNOT_CONNECT: return L"Couldn't connect to the server";
            case ERROR_WINHTTP_TIMEOUT: return L"The server took too long to respond";
            case ERROR_WINHTTP_SECURE_FAILURE: return L"Secure connection failed (certificate problem)";
            default: return L"Network error " + std::to_wstring(error);
        }
    }
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_string()) {
        return FromUtf8(parsed["error"].get<std::string>()) + L" (HTTP " + std::to_wstring(status) + L")";
    }
    return L"HTTP " + std::to_wstring(status);
}

HttpClient::HttpClient(const std::wstring& baseUrl, std::string bearerToken) : token_(std::move(bearerToken)) {
    URL_COMPONENTS parts{sizeof parts};
    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(baseUrl.c_str(), 0, 0, &parts)) return;
    secure_ = parts.nScheme == INTERNET_SCHEME_HTTPS;
    basePath_ = path;
    while (!basePath_.empty() && basePath_.back() == L'/') basePath_.pop_back();

    session_ = WinHttpOpen(L"CallRecorder/" CALLRECORDER_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) return;
    // resolve, connect, send, receive. Receive is long because Speakr
    // converts the audio before answering an upload.
    WinHttpSetTimeouts(session_, 15'000, 15'000, 120'000, 600'000);
    connection_ = WinHttpConnect(session_, host, parts.nPort, 0);
}

HttpClient::~HttpClient() {
    if (connection_) WinHttpCloseHandle(connection_);
    if (session_) WinHttpCloseHandle(session_);
}

HttpResponse HttpClient::Get(const std::wstring& path) {
    return Send(L"GET", path, L"", "", L"", "");
}

HttpResponse HttpClient::SendJson(const wchar_t* method, const std::wstring& path, const std::string& json) {
    return Send(method, path, L"application/json", json, L"", "");
}

HttpResponse HttpClient::PostMultipart(const std::wstring& path, const Fields& fields, const std::string& fileField,
                                       const std::wstring& filePath, const std::string& fileName,
                                       const std::string& fileType) {
    std::random_device random;
    std::string boundary = "----CallRecorder" + std::to_string(random()) + std::to_string(random());

    std::string head;
    for (const auto& [name, value] : fields) {
        head += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
    }
    head += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + fileField + "\"; filename=\"" +
            fileName + "\"\r\nContent-Type: " + fileType + "\r\n\r\n";
    std::string tail = "\r\n--" + boundary + "--\r\n";

    return Send(L"POST", path, L"multipart/form-data; boundary=" + FromUtf8(boundary), head, filePath, tail);
}

HttpResponse HttpClient::Send(const wchar_t* method, const std::wstring& path, const std::wstring& contentType,
                              const std::string& head, const std::wstring& filePath, const std::string& tail) {
    if (!connection_) return Failure();

    std::ifstream file;
    uint64_t fileSize = 0;
    if (!filePath.empty()) {
        file.open(filePath, std::ios::binary | std::ios::ate);
        if (!file) {
            HttpResponse response;
            response.error = ERROR_FILE_NOT_FOUND;
            return response;
        }
        fileSize = static_cast<uint64_t>(file.tellg());
        file.seekg(0);
    }
    uint64_t total = head.size() + fileSize + tail.size();
    if (total > MAXDWORD) {
        HttpResponse response;
        response.error = ERROR_FILE_TOO_LARGE;
        return response;
    }

    HINTERNET request = WinHttpOpenRequest(connection_, method, (basePath_ + path).c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure_ ? WINHTTP_FLAG_SECURE : 0);
    if (!request) return Failure();
    if (!BeginRequest(request)) {
        WinHttpCloseHandle(request);
        HttpResponse response;
        response.error = ERROR_WINHTTP_OPERATION_CANCELLED;
        return response;
    }
    struct Cleanup {
        HttpClient* client;
        HINTERNET request;
        ~Cleanup() { client->EndRequest(request); }
    } cleanup{this, request};

    DWORD noRedirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirects, sizeof noRedirects);

    std::wstring headers = L"Authorization: Bearer " + FromUtf8(token_) + L"\r\nAccept: application/json\r\n";
    if (!contentType.empty()) headers += L"Content-Type: " + contentType + L"\r\n";

    if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0,
                            static_cast<DWORD>(total), 0)) {
        return Failure();
    }

    auto write = [&](const char* data, size_t size) {
        DWORD written = 0;
        return size == 0 || WinHttpWriteData(request, data, static_cast<DWORD>(size), &written);
    };
    if (!write(head.data(), head.size())) return Failure();
    if (file.is_open()) {
        std::vector<char> chunk(kChunkSize);
        while (file) {
            file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            if (!write(chunk.data(), static_cast<size_t>(file.gcount()))) return Failure();
        }
    }
    if (!write(tail.data(), tail.size())) return Failure();

    if (!WinHttpReceiveResponse(request, nullptr)) return Failure();

    HttpResponse response;
    DWORD status = 0;
    DWORD statusSize = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);

    for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available > 0;) {
        size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, response.body.data() + offset, available, &read)) break;
        response.body.resize(offset + read);
    }
    return response;
}

bool HttpClient::BeginRequest(HINTERNET request) {
    std::lock_guard lock(requestMutex_);
    if (aborted_) return false;
    activeRequest_ = request;
    return true;
}

void HttpClient::EndRequest(HINTERNET request) {
    std::lock_guard lock(requestMutex_);
    if (activeRequest_ == request) {  // otherwise Abort() already closed it
        WinHttpCloseHandle(request);
        activeRequest_ = nullptr;
    }
}

void HttpClient::Abort() {
    std::lock_guard lock(requestMutex_);
    aborted_ = true;
    if (activeRequest_) {
        WinHttpCloseHandle(activeRequest_);
        activeRequest_ = nullptr;
    }
}
