#pragma once
// ============================================================================
// Minimal HTTP(S) client (WinHTTP) for the Hub's remote update feed. Windows-
// native — no third-party dependency; validates TLS certs by default.
// ============================================================================
#include <functional>
#include <string>

namespace schizo::project {

// GET `url` into `out` (text). Returns false + fills `err` on any failure
// (bad URL, connect/send/receive error, or non-2xx status).
bool http_get_string(const std::string& url, std::string& out, std::string* err = nullptr);

// GET `url` and stream it to `dest_path` (binary). Optional `on_bytes(total)`
// is called with the running byte count for progress display.
bool http_download_file(const std::string& url, const std::string& dest_path,
                        std::string* err = nullptr,
                        const std::function<void(size_t)>& on_bytes = nullptr);

} // namespace schizo::project
