#pragma once
// ============================================================================
// Remote update feed (Phase 5). The Hub fetches a small text feed listing
// available engine versions, and can download + install any of them via the
// Phase-4 install mechanics.
//
// Feed format — one version per line, TAB-separated (comments start with #):
//     <version> \t <package-url> \t <optional notes>
// e.g.  0.2.0    https://example.com/gws/engine-0.2.0.zip    Bug fixes
// The package is a .zip whose contents include editor.exe (the engine folder).
// ============================================================================
#include "http_client.h"   // HttpHeaders

#include <functional>
#include <string>
#include <vector>

namespace schizo::project {

struct RemoteVersion {
    std::string version;
    std::string url;
    std::string notes;
};

// Parse a feed body (no network). Exposed for headless testing.
void parse_feed(const std::string& body, std::vector<RemoteVersion>& out);

// GET the feed at `feed_url` and parse it. Returns false + `err` on failure.
bool fetch_feed(const std::string& feed_url, std::vector<RemoteVersion>& out,
                std::string* err = nullptr);

// Download the package, extract it (bsdtar), locate the engine folder, and
// install it as `rv.version`. `progress` receives human-readable step messages.
// `dl_headers` are extra request headers for the download (e.g. GitHub auth for
// a private release asset).
bool download_and_install(const RemoteVersion& rv, std::string* err = nullptr,
                          const std::function<void(const std::string&)>& progress = nullptr,
                          const HttpHeaders& dl_headers = {});

// The configured feed URL (persisted in the user config dir), or a default.
std::string feed_url();
void        set_feed_url(const std::string& url);

// Extract a .zip into `dest` (OS bsdtar, else PowerShell Expand-Archive). Exposed
// so the self-updater can reuse the same robust extraction.
bool extract_zip(const std::string& zip, const std::string& dest, std::string* err = nullptr);

} // namespace schizo::project
