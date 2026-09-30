// Pure parts of HttpClient.cpp. No HttpClient is constructed, so nothing here
// opens a socket.
#include "TestHarness.h"

#include "HttpClient.h"

static bool PublicHttp(const wchar_t* url) { return IsUnencryptedPublicUrl(url); }

TEST(IsUnencryptedPublicUrl_HttpsIsNeverFlagged) {
    CHECK(!PublicHttp(L"https://speakr.example.com"));
    CHECK(!PublicHttp(L"https://8.8.8.8"));
    CHECK(!PublicHttp(L"https://speakr.example.com:8443/api/v1"));
    CHECK(!PublicHttp(L"HTTPS://EXAMPLE.COM"));
}

TEST(IsUnencryptedPublicUrl_PublicHttpHostsAreFlagged) {
    CHECK(PublicHttp(L"http://speakr.example.com"));
    CHECK(PublicHttp(L"http://SPEAKR.Example.COM"));
    CHECK(PublicHttp(L"HTTP://example.com"));
    CHECK(PublicHttp(L"http://8.8.8.8"));
    CHECK(PublicHttp(L"http://8.8.8.8:8899/path"));
    CHECK(PublicHttp(L"http://example.com:8080/path?x=1#frag"));
    CHECK(PublicHttp(L"http://example.com."));  // trailing dot
    CHECK(PublicHttp(L"http://user:pw@example.com/"));
    CHECK(PublicHttp(L"http://locallan.com"));     // suffix needs the dot
    CHECK(PublicHttp(L"http://evil-ts.net"));      // not .ts.net
    CHECK(PublicHttp(L"http://foo.notlocal"));
    CHECK(PublicHttp(L"http://a.b.c.example.org"));
}

TEST(IsUnencryptedPublicUrl_LoopbackAndLocalhost) {
    CHECK(!PublicHttp(L"http://localhost"));
    CHECK(!PublicHttp(L"http://LOCALHOST:8899"));
    CHECK(!PublicHttp(L"http://localhost./"));
    CHECK(!PublicHttp(L"http://foo.localhost"));
    CHECK(!PublicHttp(L"http://127.0.0.1"));
    CHECK(!PublicHttp(L"http://127.1.2.3:8899"));
    CHECK(!PublicHttp(L"http://[::1]"));
    CHECK(!PublicHttp(L"http://[::1]:8899"));
    CHECK(!PublicHttp(L"http://[::1]:8899/x"));
    CHECK(!PublicHttp(L"http://0.0.0.0"));
    CHECK(!PublicHttp(L"http://[::]"));
}

TEST(IsUnencryptedPublicUrl_Ipv4PrivateRanges) {
    CHECK(!PublicHttp(L"http://10.0.0.1"));
    CHECK(!PublicHttp(L"http://10.255.255.255"));
    CHECK(PublicHttp(L"http://11.0.0.1"));
    CHECK(PublicHttp(L"http://172.15.255.255"));
    CHECK(!PublicHttp(L"http://172.16.0.1"));
    CHECK(!PublicHttp(L"http://172.31.255.255"));
    CHECK(PublicHttp(L"http://172.32.0.1"));
    CHECK(!PublicHttp(L"http://192.168.0.1:8899"));
    CHECK(PublicHttp(L"http://192.167.0.1"));
    CHECK(PublicHttp(L"http://192.169.0.1"));
    CHECK(!PublicHttp(L"http://169.254.10.10"));
    CHECK(PublicHttp(L"http://169.253.10.10"));
    CHECK(PublicHttp(L"http://100.63.255.255"));
    CHECK(!PublicHttp(L"http://100.64.0.1"));
    CHECK(!PublicHttp(L"http://100.100.100.100"));
    CHECK(!PublicHttp(L"http://100.127.255.255"));
    CHECK(PublicHttp(L"http://100.128.0.1"));
}

TEST(IsUnencryptedPublicUrl_Ipv6Ranges) {
    CHECK(!PublicHttp(L"http://[fc00::1]"));
    CHECK(!PublicHttp(L"http://[fd12:3456:789a::1]"));
    CHECK(!PublicHttp(L"http://[fdff:ffff::1]:8899"));
    CHECK(PublicHttp(L"http://[fe00::1]"));
    CHECK(!PublicHttp(L"http://[fe80::1]"));
    CHECK(!PublicHttp(L"http://[fe80::1234:5678:9abc:def0]"));
    CHECK(!PublicHttp(L"http://[febf::1]"));  // still fe80::/10
    CHECK(PublicHttp(L"http://[fec0::1]"));
    CHECK(PublicHttp(L"http://[2001:db8::1]"));
    CHECK(PublicHttp(L"http://[2606:4700:4700::1111]:8080"));
    CHECK(!PublicHttp(L"http://[::ffff:192.168.1.1]"));
    CHECK(!PublicHttp(L"http://[::ffff:127.0.0.1]"));
    CHECK(!PublicHttp(L"http://[::ffff:10.1.2.3]"));
    CHECK(PublicHttp(L"http://[::ffff:8.8.8.8]"));
    CHECK(!PublicHttp(L"http://[0:0:0:0:0:0:0:1]"));
    CHECK(PublicHttp(L"http://[2001:db8:0:0:0:0:0:1]"));
}

TEST(IsUnencryptedPublicUrl_IntranetNames) {
    CHECK(!PublicHttp(L"http://speakr"));
    CHECK(!PublicHttp(L"http://nas:8899"));
    CHECK(!PublicHttp(L"http://NAS/"));
    CHECK(!PublicHttp(L"http://nas.local"));
    CHECK(!PublicHttp(L"http://nas.local:8899"));
    CHECK(!PublicHttp(L"http://box.lan"));
    CHECK(!PublicHttp(L"http://box.home.arpa"));
    CHECK(!PublicHttp(L"http://svc.internal"));
    CHECK(!PublicHttp(L"http://laptop.tailnet-name.ts.net"));
    CHECK(!PublicHttp(L"http://LAPTOP.TAILNET.TS.NET:8899"));
    CHECK(!PublicHttp(L"http://nas.local."));
}

TEST(IsUnencryptedPublicUrl_MalformedInputIsNotFlagged) {
    CHECK(!PublicHttp(L""));
    CHECK(!PublicHttp(L"   "));
    CHECK(!PublicHttp(L"not a url"));
    CHECK(!PublicHttp(L"example.com"));      // no scheme
    CHECK(!PublicHttp(L"//example.com"));
    CHECK(!PublicHttp(L"http://"));
    CHECK(!PublicHttp(L"http:///path"));
    CHECK(!PublicHttp(L"ftp://example.com"));
    CHECK(!PublicHttp(L"file:///C:/x"));
    CHECK(!PublicHttp(L"http://::1"));        // unbracketed IPv6 is not a valid URL
    CHECK(!PublicHttp(L"http://[::1"));
    CHECK(!PublicHttp(L"http://example.com:99999"));
    CHECK(!PublicHttp(L"http://example.com:abc"));
}

TEST(IsUnencryptedPublicUrl_OddButHostLikeInputs) {
    // Not valid IPv4, so treated as hostnames: public.
    CHECK(PublicHttp(L"http://256.1.1.1"));
    CHECK(PublicHttp(L"http://1.2.3"));
    // Malformed IPv6 literals that WinHTTP still cracks: not parseable, so
    // conservatively flagged as public.
    CHECK(PublicHttp(L"http://[1:2:3:4:5:6:7:8:9]"));
    CHECK(PublicHttp(L"http://[gggg::1]"));
}

TEST(HttpResponse_OkCoversOnly2xx) {
    HttpResponse r;
    for (int status : {199, 300, 301, 401, 404, 500, 0}) {
        r.status = status;
        CHECK(!r.Ok());
    }
    for (int status : {200, 201, 204, 299}) {
        r.status = status;
        CHECK(r.Ok());
    }
}

static std::wstring Describe(int status, DWORD error, const std::string& body) {
    HttpResponse r;
    r.status = status;
    r.error = error;
    r.body = body;
    return r.Describe();
}

TEST(HttpResponseDescribe_UsesErrorFieldFromJsonBody) {
    CHECK_EQ(Describe(401, 0, R"({"error":"Invalid token"})"), L"Invalid token (HTTP 401)");
    CHECK_EQ(Describe(413, 0, R"({"error":"Too large","other":1})"), L"Too large (HTTP 413)");
    CHECK_EQ(Describe(500, 0, "{\"error\":\"Z\\u00fcrich\"}"), L"Z\u00fcrich (HTTP 500)");
}

TEST(HttpResponseDescribe_MalformedBodiesFallBackToStatus) {
    for (const char* body : {"", "not json", "<html>Bad Gateway</html>", "{", "null", "[]", "5", "\"error\"",
                             R"({"error":null})", R"({"error":5})", R"({"error":""})", R"({"error":[]})",
                             R"({"error":{"message":"x"}})", R"({"message":"x"})", R"([{"error":"x"}])"}) {
        std::wstring text;
        CHECK_NOTHROW(text = Describe(502, 0, body));
        CHECK_EQ(text, L"HTTP 502");
    }
}

TEST(HttpResponseDescribe_ErrorFieldIgnoredWhenNoStatus) {
    // status 0: the WinHTTP error decides, never the body.
    CHECK_EQ(Describe(0, ERROR_WINHTTP_TIMEOUT, R"({"error":"x"})"), L"The server took too long to respond");
}

TEST(HttpResponseDescribe_TranslatesTransportErrors) {
    CHECK_EQ(Describe(0, ERROR_WINHTTP_NAME_NOT_RESOLVED, ""), L"Server name not found");
    CHECK_EQ(Describe(0, ERROR_WINHTTP_CANNOT_CONNECT, ""), L"Couldn't connect to the server");
    CHECK_EQ(Describe(0, ERROR_WINHTTP_TIMEOUT, ""), L"The server took too long to respond");
    CHECK_EQ(Describe(0, ERROR_WINHTTP_SECURE_FAILURE, ""), L"Secure connection failed (certificate problem)");
    CHECK_EQ(Describe(0, ERROR_WINHTTP_INVALID_URL, ""), L"The Speakr address isn't a valid web address");
    CHECK_EQ(Describe(0, ERROR_WINHTTP_OPERATION_CANCELLED, ""), L"Cancelled");
    CHECK_EQ(Describe(0, ERROR_WINHTTP_CONNECTION_ERROR, ""), L"The connection was interrupted");
    CHECK_EQ(Describe(0, ERROR_FILE_NOT_FOUND, ""), L"The audio file is missing");
    CHECK_EQ(Describe(0, ERROR_FILE_TOO_LARGE, ""), L"The recording is too large to upload");
    CHECK_EQ(Describe(0, 0, ""), L"Network error 0");
    CHECK_EQ(Describe(0, 12345, ""), L"Network error 12345");
}
