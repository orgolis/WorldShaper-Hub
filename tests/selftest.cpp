// hub_selftest — verifies the GitHub-Releases update path against a mock API.
//   hub_selftest <api_base>   e.g. http://127.0.0.1:8100
// The mock must serve GET <api_base>/repos/me/engine/releases (a GitHub-shaped
// JSON array) whose asset browser_download_url points at a small engine .zip.
#include "github_releases.h"
#include "update_feed.h"
#include "engine_registry.h"
#include "self_update.h"
#include "project.h"

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

// ---- local, network-free checks: registry uniqueness / remove safety / import ----
static int run_local_checks() {
    std::cout << "  mode: local (no network)\n";

    // Registry operates purely in memory here (add()/remove() never touch disk;
    // only the UI calls save()), so the user's real recent_projects.txt is safe.
    ProjectsRegistry reg;
    reg.add(RecentProject{"Alpha", "C:/proj/Alpha/project.schizo"});
    reg.add(RecentProject{"Beta",  "C:/proj/Beta/project.schizo"});
    check("name_exists finds a present name",        reg.name_exists("Alpha"));
    check("name_exists rejects an absent name",     !reg.name_exists("Gamma"));
    check("unique_name passes a free name through",  reg.unique_name("Gamma") == "Gamma");
    check("unique_name suffixes a taken name",       reg.unique_name("Alpha") == "Alpha (2)");

    // remove() by value must be safe even when the argument aliases a string that
    // lives inside the vector being erased (this used to crash the whole Hub).
    const std::string& aliased = reg.items().front().manifest_path;  // -> "Beta"'s path (most recent)
    reg.remove(aliased);
    check("remove(aliased-into-vector) does not crash", true);
    check("remove actually removed the row",            reg.items().size() == 1);
    check("the surviving row is the other project",     reg.items().front().name == "Alpha");

    // import_existing_project: adopt a bare folder, creating the standard layout.
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec) / "gws_hub_selftest_import";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "MyGame", ec);       // an existing folder with NO layout
    std::string mpath, name;
    bool imp = import_existing_project((tmp / "MyGame").string(), "0.1.7", mpath, name);
    check("import_existing_project succeeds", imp);
    check("import derived name from folder",  name == "MyGame");
    check("import wrote a manifest",          fs::exists(mpath, ec));
    check("import created scenes/",           fs::is_directory(tmp / "MyGame" / "scenes", ec));
    check("import created assets/",           fs::is_directory(tmp / "MyGame" / "assets", ec));
    check("import created assets/scripts/",   fs::is_directory(tmp / "MyGame" / "assets" / "scripts", ec));
    check("import created assets/skies/",     fs::is_directory(tmp / "MyGame" / "assets" / "skies", ec));

    // Re-importing the same folder must adopt the existing manifest (idempotent).
    std::string mpath2, name2;
    bool imp2 = import_existing_project((tmp / "MyGame").string(), "9.9.9", mpath2, name2);
    check("re-import adopts existing manifest", imp2 && name2 == "MyGame" && mpath2 == mpath);
    fs::remove_all(tmp, ec);

    if (g_fail == 0) { std::cout << "hub_selftest (local): ALL OK\n"; return 0; }
    std::cout << "hub_selftest (local): " << g_fail << " FAILED\n";
    return 1;
}

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);
    if (argc >= 2 && std::string(argv[1]) == "--local") return run_local_checks();
    if (argc < 2) { std::cout << "usage: hub_selftest <api_base_url> [owner] [repo]\n"
                              << "       hub_selftest --local   (network-free registry/import checks)\n"; return 2; }
    const std::string base  = argv[1];
    const bool        real  = (argc >= 4);           // real GitHub run: use config token
    const std::string owner = real ? argv[2] : "me";
    const std::string repo  = real ? argv[3] : "engine";

    const std::string saved_token = github_token();
    if (!real) set_github_token("testtoken");        // mock mode requires this exact token

    const bool token_free = github_token().empty();
    std::cout << "  mode: " << (real ? "real GitHub" : "mock")
              << (token_free ? " (public, no token)" : " (authenticated)") << "\n";

    // ---- Hub self-update: version comparison (pure logic) ----
    check("hub_version() is set at build time", !hub_version().empty() && hub_version() != "0.0.0");
    check("version_is_newer: 0.2.0 > 0.1.9",   version_is_newer("0.2.0", "0.1.9"));
    check("version_is_newer: 0.1.10 > 0.1.9",  version_is_newer("0.1.10", "0.1.9"));
    check("version_is_newer: equal is not newer", !version_is_newer("0.1.0", "0.1.0"));
    check("version_is_newer: older is not newer", !version_is_newer("0.1.0", "0.2.0"));
    check("version_is_newer: strips pre-release suffix", !version_is_newer("1.0.0-rc1", "1.0.0"));

    if (real) {
        // Live check against the actual Hub repo — must not error (found may be
        // true or false depending on whether a Hub release is published yet).
        RemoteVersion hv; bool hfound = false; std::string herr;
        const bool hok = check_hub_update(hv, hfound, &herr);
        check("check_hub_update queries the Hub repo without error", hok);
        if (!hok) std::cout << "  (" << herr << ")\n";
        std::cout << "  hub self-update: " << (hfound ? ("newer Hub " + hv.version + " available")
                                                      : "up to date (or no Hub release yet)") << "\n";
    }

    std::string err;
    std::vector<RemoteVersion> v;
    bool ok = fetch_github_releases(owner, repo, v, &err, ".zip", base);
    check("fetch_github_releases succeeds", ok);
    if (!ok) std::cout << "  (" << err << ")\n";
    check(">= 1 release with an engine asset", ok && !v.empty());

    if (ok && !v.empty()) {
        if (!real) check("tag 'v0.9.9' parsed to version '0.9.9'", v[0].version == "0.9.9");
        check("asset download URL present", !v[0].url.empty());

        bool inst = download_and_install(v[0], &err, nullptr, github_download_headers());
        check("download + extract + install", inst);
        if (!inst) std::cout << "  (" << err << ")\n";

        fs::path dest = fs::path(EngineRegistry::engines_dir()) / v[0].version;
        std::error_code ec;
        check("installed editor.exe present", fs::exists(dest / "editor.exe", ec));

        // Install history: the install was recorded; it survives removal (so it
        // shows as "previously installed"); forget clears it.
        auto in_history = [&](const std::string& ver) {
            auto h = EngineRegistry::load_history();
            for (const auto& r : h) if (r.version == ver) return true;
            return false;
        };
        check("install recorded in history", in_history(v[0].version));

        EngineRegistry::remove_installed_version(v[0].version, &err);
        check("cleanup removed it", !fs::exists(dest, ec));
        check("history keeps the removed version (was-installed)", in_history(v[0].version));
        EngineRegistry::forget_version(v[0].version);
        check("forget_version drops it from history", !in_history(v[0].version));
    }

    // Real runs never touch the token; only mock mode set a fake one — undo that
    // so a token-free machine stays token-free (no lingering token file).
    if (!real) set_github_token(saved_token);

    if (g_fail == 0) { std::cout << "hub_selftest: ALL OK\n"; return 0; }
    std::cout << "hub_selftest: " << g_fail << " FAILED\n";
    return 1;
}
