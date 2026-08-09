#include "self_update.h"

#include "github_releases.h"   // fetch_github_releases, github_download_headers
#include "engine_registry.h"   // this_executable_path
#include "http_client.h"       // http_download_file
#include "update_feed.h"       // extract_zip

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>   // ShellExecuteA (elevated swap when Program Files)
#endif

namespace fs = std::filesystem;

namespace schizo::project {
namespace {

fs::path config_dir() {
    if (const char* ad = std::getenv("APPDATA")) return fs::path(ad) / "GameWorldshaper";
    if (const char* hp = std::getenv("USERPROFILE")) return fs::path(hp) / ".gameworldshaper";
    return fs::path(".") / ".gameworldshaper";
}

// Parse a dotted version's numeric leading components (ignores any trailing
// pre-release suffix): "1.2.3-rc1" -> [1,2,3].
std::vector<long> version_parts(const std::string& v) {
    std::vector<long> parts;
    size_t i = 0;
    while (i < v.size()) {
        long n = 0; bool any = false;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') { n = n * 10 + (v[i]-'0'); ++i; any = true; }
        if (any) parts.push_back(n); else break;
        if (i < v.size() && v[i] == '.') ++i; else break;
    }
    return parts;
}

#ifdef _WIN32
// Can we create files in `dir`? (Program Files is not writable without admin.)
bool dir_writable(const fs::path& dir) {
    std::error_code ec;
    const fs::path probe = dir / ".gws_write_test.tmp";
    std::ofstream f(probe);
    const bool ok = f.is_open();
    f.close();
    if (ok) fs::remove(probe, ec);
    return ok;
}

bool spawn_detached(const std::string& cmdline) {
    std::string cmd = cmdline;                      // CreateProcessA may modify it
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// Find GameWorldshaperHub.exe within an extracted package (shallow recurse).
fs::path find_hub_exe(const fs::path& root) {
    std::error_code ec;
    if (fs::exists(root / "GameWorldshaperHub.exe", ec)) return root / "GameWorldshaperHub.exe";
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec) && it->path().filename() == "GameWorldshaperHub.exe")
            return it->path();
    }
    return {};
}
#endif

} // namespace

std::string hub_version() {
#ifdef HUB_VERSION
    return HUB_VERSION;
#else
    return "0.0.0";
#endif
}

std::string hub_repo() {
    std::ifstream in(config_dir() / "hub_repo.txt");
    if (in) {
        std::string s; std::getline(in, s);
        while (!s.empty() && (s.back()=='\r' || s.back()=='\n' || s.back()==' ')) s.pop_back();
        if (!s.empty() && s[0] != '#') return s;
    }
    return "orgolis/WorldShaper-Hub";
}

void set_hub_repo(const std::string& owner_slash_repo) {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream(config_dir() / "hub_repo.txt", std::ios::trunc) << owner_slash_repo << "\n";
}

bool version_is_newer(const std::string& candidate, const std::string& current) {
    const std::vector<long> a = version_parts(candidate);
    const std::vector<long> b = version_parts(current);
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const long ca = i < a.size() ? a[i] : 0;
        const long cb = i < b.size() ? b[i] : 0;
        if (ca != cb) return ca > cb;
    }
    return false;   // equal
}

bool check_hub_update(RemoteVersion& newer, bool& found, std::string* err) {
    found = false;
    const std::string spec = hub_repo();
    const size_t slash = spec.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= spec.size()) {
        if (err) *err = "hub repo must be owner/repo";
        return false;
    }
    std::vector<RemoteVersion> rels;
    // "-win64.zip", not ".zip" — asset matching takes the FIRST hit in GitHub's
    // name-sorted list, so a second .zip on a release (a symbols archive, say)
    // shadows the real package. That exact bug hit the engine feed at v0.2.0.
    // The Hub's releases carry only one .zip today; this keeps it safe by
    // construction. See the note in github_releases.h.
    if (!fetch_github_releases(spec.substr(0, slash), spec.substr(slash + 1), rels, err, "-win64.zip"))
        return false;

    const std::string cur = hub_version();
    for (const auto& rv : rels) {
        if (version_is_newer(rv.version, cur) &&
            (!found || version_is_newer(rv.version, newer.version))) {
            newer = rv;
            found = true;
        }
    }
    return true;
}

bool apply_hub_update(const RemoteVersion& rv, std::string* err,
                      const std::function<void(const std::string&)>& progress) {
    auto note = [&](const std::string& m) { if (progress) progress(m); spdlog::info("[hub-update] {}", m); };
#ifdef _WIN32
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / ("gws_hub_update_" + rv.version);
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    const fs::path zip = tmp / "hub.zip";
    note("Downloading Hub " + rv.version + "...");
    if (!http_download_file(rv.url, zip.string(), err, nullptr, github_download_headers())) {
        fs::remove_all(tmp, ec); return false;
    }

    note("Extracting...");
    const fs::path ex = tmp / "extract";
    if (!extract_zip(zip.string(), ex.string(), err)) { fs::remove_all(tmp, ec); return false; }

    const fs::path newexe = find_hub_exe(ex);
    if (newexe.empty()) {
        if (err) *err = "no GameWorldshaperHub.exe in the downloaded package";
        fs::remove_all(tmp, ec); return false;
    }
    const fs::path newdir  = newexe.parent_path();
    const fs::path install = fs::path(this_executable_path()).parent_path();
    const fs::path dstexe  = install / "GameWorldshaperHub.exe";

    // Helper batch (in TEMP, outside the dirs it touches): wait until the running
    // Hub releases its exe (the copy fails while it's locked), copy the new files
    // over the install, relaunch, then clean up staging and self-delete.
    const char* t = std::getenv("TEMP"); if (!t) t = std::getenv("TMP");
    const fs::path bat = fs::path(t ? t : ".") / "gws_hub_selfupdate.bat";
    std::ofstream b(bat, std::ios::binary);
    if (!b) { if (err) *err = "could not write the update helper"; fs::remove_all(tmp, ec); return false; }
    b << "@echo off\r\n:copyexe\r\n";
    b << "copy /y \"" << newexe.string() << "\" \"" << dstexe.string() << "\" >nul 2>&1\r\n";
    b << "if errorlevel 1 ( ping -n 2 127.0.0.1 >nul & goto copyexe )\r\n";
    b << "xcopy /y /e \"" << newdir.string() << "\\*\" \"" << install.string() << "\\\" >nul 2>&1\r\n";
    // Relaunch via explorer so the updated Hub runs at normal integrity even when
    // this helper was elevated (a Program Files swap), i.e. never left running as admin.
    b << "explorer.exe \"" << dstexe.string() << "\"\r\n";
    b << "rmdir /s /q \"" << tmp.string() << "\" >nul 2>&1\r\n";
    b << "del \"%~f0\" >nul 2>&1\r\n";
    b.close();

    note("Restarting to apply the update...");
    const std::string batcmd = "cmd.exe /c \"" + bat.string() + "\"";
    bool launched = false;
    if (dir_writable(install)) {
        launched = spawn_detached(batcmd);
    } else {
        // Protected location (e.g. Program Files): the swap needs admin, so run the
        // helper elevated via ShellExecute "runas" (a UAC prompt appears).
        const std::string params = "/c \"" + bat.string() + "\"";
        HINSTANCE h = ShellExecuteA(nullptr, "runas", "cmd.exe", params.c_str(), nullptr, SW_HIDE);
        launched = reinterpret_cast<INT_PTR>(h) > 32;
        if (!launched && err) *err = "update needs administrator (elevation was declined)";
    }
    if (!launched) {
        if (err && err->empty()) *err = "could not launch the update helper";
        fs::remove_all(tmp, ec);
        return false;
    }
    return true;   // caller closes the window; the helper waits, swaps, relaunches
#else
    (void)rv; (void)note;
    if (err) *err = "self-update is only implemented on Windows";
    return false;
#endif
}

} // namespace schizo::project
