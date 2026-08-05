#include "github_releases.h"

#include "http_client.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace schizo::project {
namespace {
fs::path config_dir() {
    if (const char* ad = std::getenv("APPDATA")) return fs::path(ad) / "GameWorldshaper";
    if (const char* hp = std::getenv("USERPROFILE")) return fs::path(hp) / ".gameworldshaper";
    return fs::path(".") / ".gameworldshaper";
}

// Headers for GitHub API JSON calls (+ Authorization when a token is configured).
HttpHeaders api_json_headers() {
    HttpHeaders h{ "Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28" };
    std::string t = github_token();
    if (!t.empty()) h.push_back("Authorization: Bearer " + t);
    return h;
}
} // namespace

bool fetch_github_releases(const std::string& owner, const std::string& repo,
                           std::vector<RemoteVersion>& out, std::string* err,
                           const std::string& asset_match, const std::string& api_base) {
    out.clear();
    if (owner.empty() || repo.empty()) { if (err) *err = "set owner/repo first"; return false; }

    const std::string url = api_base + "/repos/" + owner + "/" + repo + "/releases";
    std::string body;
    if (!http_get_string(url, body, err, api_json_headers())) return false;
    const bool have_token = !github_token().empty();

    json j;
    try { j = json::parse(body); }
    catch (const std::exception& e) { if (err) *err = std::string("bad JSON from API: ") + e.what(); return false; }
    if (!j.is_array()) { if (err) *err = "unexpected API response (not a release array)"; return false; }

    for (const auto& rel : j) {
        const std::string tag = rel.value("tag_name", std::string());
        if (tag.empty() || rel.value("draft", false)) continue;
        if (!rel.contains("assets") || !rel["assets"].is_array()) continue;

        for (const auto& a : rel["assets"]) {
            const std::string an = a.value("name", std::string());
            if (an.find(asset_match) == std::string::npos) continue;
            // Private repos: download via the auth'd asset API url (redirects to a
            // signed URL). Public repos: the direct browser_download_url.
            const std::string durl = have_token ? a.value("url", std::string())
                                                : a.value("browser_download_url", std::string());
            if (durl.empty()) continue;

            RemoteVersion rv;
            rv.version = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
            rv.url     = durl;
            rv.notes   = rel.value("name", tag);
            if (rel.value("prerelease", false)) rv.notes += " (pre-release)";
            out.push_back(std::move(rv));
            break;  // first matching asset per release
        }
    }
    spdlog::info("[github] {}/{}: {} installable release(s)", owner, repo, out.size());
    return true;
}

std::string github_repo() {
    std::ifstream in(config_dir() / "github_repo.txt");
    if (in) {
        std::string s; std::getline(in, s);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (!s.empty() && s[0] != '#') return s;
    }
    return "orgolis/c-Engine-Game";  // the public engine repo — works with no token
}

void set_github_repo(const std::string& owner_slash_repo) {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream(config_dir() / "github_repo.txt", std::ios::trunc) << owner_slash_repo << "\n";
}

std::string github_token() {
    std::ifstream in(config_dir() / "github_token.txt");
    if (in) {
        std::string s; std::getline(in, s);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (!s.empty() && s[0] != '#') return s;
    }
    return {};
}

void set_github_token(const std::string& token) {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream(config_dir() / "github_token.txt", std::ios::trunc) << token << "\n";
}

HttpHeaders github_download_headers() {
    HttpHeaders h;
    std::string t = github_token();
    if (!t.empty()) {
        h.push_back("Authorization: Bearer " + t);
        h.push_back("Accept: application/octet-stream");  // asset API returns the binary
    }
    return h;
}

} // namespace schizo::project
