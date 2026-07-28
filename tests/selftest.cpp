// hub_selftest — verifies the GitHub-Releases update path against a mock API.
//   hub_selftest <api_base>   e.g. http://127.0.0.1:8100
// The mock must serve GET <api_base>/repos/me/engine/releases (a GitHub-shaped
// JSON array) whose asset browser_download_url points at a small engine .zip.
#include "github_releases.h"
#include "update_feed.h"
#include "engine_registry.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace schizo::project;

static int g_fail = 0;
static void check(const char* what, bool ok) {
    std::cout << (ok ? "  [ OK ] " : "  [FAIL] ") << what << "\n";
    if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::warn);
    if (argc < 2) { std::cout << "usage: hub_selftest <api_base_url>\n"; return 2; }
    const std::string base = argv[1];

    // Exercise the TOKEN (private-repo) path: the mock requires this exact token
    // on every request (401 otherwise), so a success proves the auth header is
    // sent on both the API call and the asset download. Save + restore the real
    // token so the test doesn't clobber the user's config.
    const std::string saved_token = github_token();
    set_github_token("testtoken");

    std::string err;
    std::vector<RemoteVersion> v;
    bool ok = fetch_github_releases("me", "engine", v, &err, ".zip", base);
    check("fetch_github_releases succeeds (auth header sent)", ok);
    if (!ok) std::cout << "  (" << err << ")\n";
    check(">= 1 release with an engine asset", ok && !v.empty());

    if (ok && !v.empty()) {
        check("tag 'v0.9.9' parsed to version '0.9.9'", v[0].version == "0.9.9");
        check("asset download URL present", !v[0].url.empty());

        bool inst = download_and_install(v[0], &err, nullptr, github_download_headers());
        check("download + extract + install (auth'd asset)", inst);
        if (!inst) std::cout << "  (" << err << ")\n";

        fs::path dest = fs::path(EngineRegistry::engines_dir()) / v[0].version;
        std::error_code ec;
        check("installed editor.exe present", fs::exists(dest / "editor.exe", ec));

        EngineRegistry::remove_installed_version(v[0].version, &err);
        check("cleanup removed it", !fs::exists(dest, ec));
    }

    set_github_token(saved_token);   // restore the real config

    if (g_fail == 0) { std::cout << "hub_selftest: ALL OK\n"; return 0; }
    std::cout << "hub_selftest: " << g_fail << " FAILED\n";
    return 1;
}
