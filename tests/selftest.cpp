// hub_selftest — verifies the GitHub-Releases update path against a mock API.
//   hub_selftest <api_base>   e.g. http://127.0.0.1:8100
// The mock must serve GET <api_base>/repos/me/engine/releases (a GitHub-shaped
// JSON array) whose asset browser_download_url points at a small engine .zip.
#include "github_releases.h"
#include "update_feed.h"
#include "engine_registry.h"
#include "self_update.h"
#include "project.h"
#include <fstream>
#include "git_scaffold.h"

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

// std::filesystem::remove_all cannot delete git's object files on Windows:
// git marks them read-only, and the delete fails partway, leaving a half-built
// .git behind. The next run then starts dirty and fails — so the test passed
// once and failed on every re-run. Clear the read-only bit first.
static void force_remove_all(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return;
    for (auto it = fs::recursive_directory_iterator(
                       p, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        ec.clear();
    }
    fs::remove_all(p, ec);
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

    // ---- git + LFS scaffolding (issue #2) --------------------------------
    // The rule this protects: LFS must be configured BEFORE the first commit,
    // or already-committed binaries stay in history forever.
    {
        fs::path g = fs::temp_directory_path(ec) / "gws_hub_selftest_git";
        force_remove_all(g);
        fs::create_directories(g, ec);
        std::string mp;
        FeatureSet fx;
        bool made = create_project(g.string(), "GitGame", fx, mp, "0.2.0");
        check("create_project succeeds", made);

        const fs::path proj = g / "GitGame";
        check("wrote .gitattributes", fs::exists(proj / ".gitattributes", ec));
        check("wrote .gitignore",     fs::exists(proj / ".gitignore", ec));

        std::ifstream ga(proj / ".gitattributes");
        std::string attrs((std::istreambuf_iterator<char>(ga)), std::istreambuf_iterator<char>());
        check("LFS tracks textures", attrs.find("*.png   filter=lfs") != std::string::npos);
        check("LFS tracks models",   attrs.find("*.fbx   filter=lfs") != std::string::npos);
        check("LFS tracks audio",    attrs.find("*.wav   filter=lfs") != std::string::npos);
        // The distinction that matters: engine text formats must NOT be in LFS,
        // or scene history stops being readable.
        check("scenes stay text",    attrs.find("*.scene    text") != std::string::npos);
        check("gameplay stays text", attrs.find("*.gameplay text") != std::string::npos);
        check("scenes NOT in LFS",   attrs.find("*.scene  filter=lfs") == std::string::npos);

        std::ifstream gi(proj / ".gitignore");
        std::string ign((std::istreambuf_iterator<char>(gi)), std::istreambuf_iterator<char>());
        check("ignores build output", ign.find("build/") != std::string::npos);
        check("ignores editor.ini",   ign.find("editor.ini") != std::string::npos);

        // git itself is optional: absence must not fail project creation.
        if (git_available()) {
            check("git repo initialised", fs::exists(proj / ".git", ec));

            // THE property this feature exists for: the LFS rules must be in the
            // FIRST commit. If .gitattributes arrives after binaries are already
            // committed, those binaries are in history forever and the repo is
            // permanently bloated. Asserting the file exists on disk is not
            // enough — it has to be tracked.
            const std::string q =
                "\"cd /d \"" + proj.string() + "\" && git ls-files --error-unmatch "
                ".gitattributes >nul 2>&1\"";
            check("LFS rules are in the first commit", std::system(q.c_str()) == 0);

            const std::string q2 =
                "\"cd /d \"" + proj.string() + "\" && git ls-files --error-unmatch "
                "project.schizo >nul 2>&1\"";
            check("manifest committed too", std::system(q2.c_str()) == 0);
        } else {
            std::cout << "  [note] git not on PATH - repo-creation checks skipped" << std::endl;
            check("project still created without git", fs::exists(mp, ec));
        }
        force_remove_all(g);
    }

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
