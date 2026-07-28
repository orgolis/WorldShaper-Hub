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
} // namespace

bool fetch_github_releases(const std::string& owner, const std::string& repo,
                           std::vector<RemoteVersion>& out, std::string* err,
                           const std::string& asset_match, const std::string& api_base) {
    out.clear();
    if (owner.empty() || repo.empty()) { if (err) *err = "set owner/repo first"; return false; }

    const std::string url = api_base + "/repos/" + owner + "/" + repo + "/releases";
    std::string body;
    if (!http_get_string(url, body, err)) return false;

    json j;
    try { j = json::parse(body); }
    catch (const std::exception& e) { if (err) *err = std::string("bad JSON from API: ") + e.what(); return false; }
    if (!j.is_array()) { if (err) *err = "unexpected API response (not a release array)"; return false; }

    for (const auto& rel : j) {
        const std::string tag = rel.value("tag_name", std::string());
        if (tag.empty() || rel.value("draft", false)) continue;
        if (!rel.contains("assets") || !rel["assets"].is_array()) continue;

        for (const auto& a : rel["assets"]) {
            const std::string an   = a.value("name", std::string());
            const std::string durl = a.value("browser_download_url", std::string());
            if (durl.empty() || an.find(asset_match) == std::string::npos) continue;

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
    return "your-org/GameWorldshaper-Engine";  // placeholder — set in the Hub
}

void set_github_repo(const std::string& owner_slash_repo) {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream(config_dir() / "github_repo.txt", std::ios::trunc) << owner_slash_repo << "\n";
}

} // namespace schizo::project
