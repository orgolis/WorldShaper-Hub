#include "http_client.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace schizo::project {

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Perform a GET, feeding each received chunk to `sink`. Returns false + `err`.
// Redirects are followed MANUALLY (auto-redirect disabled) so that request
// headers — notably Authorization — are NOT re-sent across origins: a GitHub
// asset API 302s to a pre-signed AWS URL that rejects an extra Authorization
// header. We therefore follow the Location with EMPTY headers.
bool http_get(const std::string& url,
              const std::function<bool(const char*, size_t)>& sink, std::string* err,
              const HttpHeaders& headers, int redirects_left = 5) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[4096] = {0};
    uc.lpszHostName    = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath     = path; uc.dwUrlPathLength  = 4095;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return fail("bad URL");
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hS = WinHttpOpen(L"GameWorldshaperHub/1.0",
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return fail("WinHttpOpen failed");

    auto close_all = [&](HINTERNET a, HINTERNET b, HINTERNET c) {
        if (a) WinHttpCloseHandle(a);
        if (b) WinHttpCloseHandle(b);
        if (c) WinHttpCloseHandle(c);
    };

    HINTERNET hC = WinHttpConnect(hS, host, uc.nPort, 0);
    if (!hC) { close_all(hS, nullptr, nullptr); return fail("WinHttpConnect failed"); }

    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!hR) { close_all(hS, hC, nullptr); return fail("WinHttpOpenRequest failed"); }

    // Disable auto-redirect so we can follow it ourselves without leaking headers.
    { DWORD feat = WINHTTP_DISABLE_REDIRECTS;
      WinHttpSetOption(hR, WINHTTP_OPTION_DISABLE_FEATURE, &feat, sizeof(feat)); }

    std::wstring wheaders;
    for (const auto& h : headers) { wheaders += widen(h); wheaders += L"\r\n"; }
    LPCWSTR hdr_ptr = wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str();
    DWORD   hdr_len = wheaders.empty() ? 0 : static_cast<DWORD>(-1L);

    if (!WinHttpSendRequest(hR, hdr_ptr, hdr_len,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hR, nullptr)) {
        close_all(hS, hC, hR); return fail("request failed (offline / unreachable host?)");
    }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    // Follow redirects manually, dropping headers (see note above).
    if (status >= 300 && status < 400 && redirects_left > 0) {
        wchar_t loc[8192] = {0}; DWORD llen = sizeof(loc);
        BOOL got = WinHttpQueryHeaders(hR, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                       loc, &llen, WINHTTP_NO_HEADER_INDEX);
        close_all(hS, hC, hR);
        if (!got) return fail("redirect without Location");
        return http_get(narrow(std::wstring(loc)), sink, err, {}, redirects_left - 1);
    }
    if (status < 200 || status >= 300) {
        close_all(hS, hC, hR); return fail("HTTP status " + std::to_string(status));
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hR, &avail)) { close_all(hS, hC, hR); return fail("read (query) failed"); }
        if (avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hR, buf.data(), avail, &read)) { close_all(hS, hC, hR); return fail("read failed"); }
        if (read == 0) break;
        if (!sink(buf.data(), read)) { close_all(hS, hC, hR); return fail("write/abort"); }
    }

    close_all(hS, hC, hR);
    return true;
}

} // namespace

bool http_get_string(const std::string& url, std::string& out, std::string* err,
                     const HttpHeaders& headers) {
    out.clear();
    return http_get(url, [&](const char* d, size_t n) { out.append(d, n); return true; }, err, headers);
}

bool http_download_file(const std::string& url, const std::string& dest_path,
                        std::string* err, const std::function<void(size_t)>& on_bytes,
                        const HttpHeaders& headers) {
    std::ofstream f(dest_path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open destination file"; return false; }
    size_t total = 0;
    bool ok = http_get(url, [&](const char* d, size_t n) {
        f.write(d, (std::streamsize)n);
        total += n;
        if (on_bytes) on_bytes(total);
        return static_cast<bool>(f);
    }, err, headers);
    f.close();
    return ok;
}

#else  // non-Windows stub
bool http_get_string(const std::string&, std::string&, std::string* err, const HttpHeaders&) {
    if (err) *err = "HTTP not supported on this platform"; return false;
}
bool http_download_file(const std::string&, const std::string&, std::string* err,
                        const std::function<void(size_t)>&, const HttpHeaders&) {
    if (err) *err = "HTTP not supported on this platform"; return false;
}
#endif

} // namespace schizo::project
