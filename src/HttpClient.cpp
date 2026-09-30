#include "HttpClient.h"

#include "Json.h"
#include "Util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <random>

namespace {

constexpr size_t kChunkSize = 64 * 1024;

HttpResponse Failure(DWORD error) {
    HttpResponse response;
    response.error = error;
    return response;
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

bool EndsWith(const std::wstring& text, const std::wstring& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// "a.b.c.d", each part 0-255 decimal.
bool ParseIpv4(const std::wstring& text, unsigned& address) {
    address = 0;
    size_t pos = 0;
    for (int part = 0; part < 4; ++part) {
        size_t end = text.find(L'.', pos);
        if ((part < 3) == (end == std::wstring::npos)) return false;
        std::wstring token = text.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        if (token.empty() || token.size() > 3) return false;
        unsigned value = 0;
        for (wchar_t c : token) {
            if (c < L'0' || c > L'9') return false;
            value = value * 10 + static_cast<unsigned>(c - L'0');
        }
        if (value > 255) return false;
        address = (address << 8) | value;
        pos = end + 1;
    }
    return true;
}

bool ParseHexGroups(const std::wstring& part, std::vector<unsigned>& out) {
    if (part.empty()) return true;
    size_t pos = 0;
    for (;;) {
        size_t end = part.find(L':', pos);
        std::wstring token = part.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        if (token.empty()) return false;
        if (token.find(L'.') != std::wstring::npos) {  // ::ffff:1.2.3.4
            unsigned v4;
            if (end != std::wstring::npos || !ParseIpv4(token, v4)) return false;
            out.push_back(v4 >> 16);
            out.push_back(v4 & 0xffff);
            return true;
        }
        if (token.size() > 4) return false;
        unsigned value = 0;
        for (wchar_t c : token) {
            unsigned digit;
            if (c >= L'0' && c <= L'9') digit = static_cast<unsigned>(c - L'0');
            else if (c >= L'a' && c <= L'f') digit = static_cast<unsigned>(c - L'a') + 10;
            else return false;
            value = value * 16 + digit;
        }
        out.push_back(value);
        if (end == std::wstring::npos) return true;
        pos = end + 1;
    }
}

bool ParseIpv6(const std::wstring& text, unsigned groups[8]) {
    size_t gap = text.find(L"::");
    std::vector<unsigned> head, tail;
    if (gap == std::wstring::npos) {
        if (!ParseHexGroups(text, head) || head.size() != 8) return false;
    } else {
        if (text.find(L"::", gap + 1) != std::wstring::npos) return false;
        if (!ParseHexGroups(text.substr(0, gap), head) || !ParseHexGroups(text.substr(gap + 2), tail)) return false;
        if (head.size() + tail.size() > 7) return false;
        head.resize(8 - tail.size(), 0);
        head.insert(head.end(), tail.begin(), tail.end());
    }
    std::copy(head.begin(), head.end(), groups);
    return true;
}

bool IsPrivateIpv4(unsigned address) {
    unsigned a = address >> 24, b = (address >> 16) & 255;
    return a == 0 || a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127);
}

bool IsPrivateIpv6(const unsigned g[8]) {
    bool loopbackOrUnspecified = true;
    for (int i = 0; i < 7; ++i) loopbackOrUnspecified = loopbackOrUnspecified && g[i] == 0;
    if (loopbackOrUnspecified && g[7] <= 1) return true;
    if ((g[0] & 0xfe00) == 0xfc00) return true;  // fc00::/7
    if ((g[0] & 0xffc0) == 0xfe80) return true;  // fe80::/10
    bool mapped = g[5] == 0xffff;
    for (int i = 0; i < 5; ++i) mapped = mapped && g[i] == 0;
    return mapped && IsPrivateIpv4((g[6] << 16) | g[7]);
}

}  // namespace

bool IsUnencryptedPublicUrl(const std::wstring& url) {
    URL_COMPONENTS parts{sizeof parts};
    wchar_t host[256] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTP) return false;

    std::wstring name = Lower(host);
    if (name.size() >= 2 && name.front() == L'[' && name.back() == L']') name = name.substr(1, name.size() - 2);
    if (size_t zone = name.find(L'%'); zone != std::wstring::npos) name.resize(zone);
    while (!name.empty() && name.back() == L'.') name.pop_back();
    if (name.empty()) return false;

    unsigned v4;
    unsigned v6[8];
    if (ParseIpv4(name, v4)) return !IsPrivateIpv4(v4);
    if (name.find(L':') != std::wstring::npos) return !ParseIpv6(name, v6) || !IsPrivateIpv6(v6);

    if (name.find(L'.') == std::wstring::npos) return false;  // single label: an intranet name
    for (const wchar_t* suffix : {L".localhost", L".local", L".lan", L".home.arpa", L".internal", L".ts.net"}) {
        if (EndsWith(name, suffix)) return false;
    }
    return true;
}

std::wstring HttpResponse::Describe() const {
    if (status == 0) {
        switch (error) {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"Server name not found";
            case ERROR_WINHTTP_CANNOT_CONNECT: return L"Couldn't connect to the server";
            case ERROR_WINHTTP_TIMEOUT: return L"The server took too long to respond";
            case ERROR_WINHTTP_SECURE_FAILURE: return L"Secure connection failed (certificate problem)";
            case ERROR_WINHTTP_INVALID_URL: return L"The Speakr address isn't a valid web address";
            case ERROR_WINHTTP_OPERATION_CANCELLED: return L"Cancelled";
            case ERROR_WINHTTP_CONNECTION_ERROR: return L"The connection was interrupted";
            case ERROR_FILE_NOT_FOUND: return L"The audio file is missing";
            case ERROR_FILE_TOO_LARGE: return L"The recording is too large to upload";
            default: return L"Network error " + std::to_wstring(error);
        }
    }
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (std::string message = JsonGet(parsed, "error", ""); !message.empty()) {
        return FromUtf8(message) + L" (HTTP " + std::to_wstring(status) + L")";
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
    if (!WinHttpCrackUrl(baseUrl.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
        connectError_ = ERROR_WINHTTP_INVALID_URL;
        return;
    }
    secure_ = parts.nScheme == INTERNET_SCHEME_HTTPS;
    basePath_ = path;
    while (!basePath_.empty() && basePath_.back() == L'/') basePath_.pop_back();

    session_ = WinHttpOpen(L"MeetingRecorder/" MEETINGRECORDER_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) {
        connectError_ = GetLastError();
        return;
    }
    // resolve, connect, send, receive. Receive is long because Speakr
    // converts the audio before answering an upload.
    WinHttpSetTimeouts(session_, 15'000, 15'000, 120'000, 600'000);
    connection_ = WinHttpConnect(session_, host, parts.nPort, 0);
    if (!connection_) connectError_ = GetLastError();
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
    std::string boundary = "----MeetingRecorder" + std::to_string(random()) + std::to_string(random());

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
    if (!connection_) return Failure(connectError_ ? connectError_ : ERROR_WINHTTP_INVALID_URL);

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
    if (!request) return Fail(GetLastError());
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
        return Fail(GetLastError());
    }

    auto write = [&](const char* data, size_t size) {
        DWORD written = 0;
        return size == 0 || WinHttpWriteData(request, data, static_cast<DWORD>(size), &written);
    };
    if (!write(head.data(), head.size())) return Fail(GetLastError());
    if (file.is_open()) {
        std::vector<char> chunk(kChunkSize);
        while (file) {
            file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            if (!write(chunk.data(), static_cast<size_t>(file.gcount()))) return Fail(GetLastError());
        }
    }
    if (!write(tail.data(), tail.size())) return Fail(GetLastError());

    if (!WinHttpReceiveResponse(request, nullptr)) return Fail(GetLastError());

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

HttpResponse HttpClient::Fail(DWORD error) {
    std::lock_guard lock(requestMutex_);
    return Failure(aborted_ ? ERROR_WINHTTP_OPERATION_CANCELLED : error);
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
