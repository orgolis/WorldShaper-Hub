#include "update_feed.h"

#include "http_client.h"
#include "engine_registry.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace schizo::project {
namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

fs::path config_dir() {
    if (const char* ad = std::getenv("APPDATA")) return fs::path(ad) / "GameWorldshaper";
    if (const char* hp = std::getenv("USERPROFILE")) return fs::path(hp) / ".gameworldshaper";
    return fs::path(".") / ".gameworldshaper";
}

// Run a hidden process to completion; returns false + err if it fails / exits nonzero.
bool run_process(const std::string& app, const std::string& cmdline, std::string* err) {
#ifdef _WIN32
    std::vector<char> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back('\0');
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(app.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (err) *err = "could not run " + app; return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (code != 0) { if (err) *err = app + " exited " + std::to_string(code); return false; }
    return true;
#else
    (void)app; (void)cmdline; if (err) *err = "unsupported"; return false;
#endif
}

// Extract a .zip into `dest` using the OS bsdtar (Win10 1803+ ships tar.exe).
bool extract_archive(const std::string& zip, const std::string& dest, std::string* err) {
    std::error_code ec;
    fs::create_directories(dest, ec);
#ifdef _WIN32
    std::string tar = "C:/Windows/System32/tar.exe";
#else
    std::string tar = "tar";
#endif
    std::string cmd = "\"" + tar + "\" -xf \"" + zip + "\" -C \"" + dest + "\"";
    return run_process(tar, cmd, err);
}

// Locate the folder containing editor.exe within an extracted tree (shallow).
fs::path find_engine_dir(const fs::path& root) {
    std::error_code ec;
    for (const char* n : {"editor.exe", "GameWorldshaperEditor.exe"})
        if (fs::exists(root / n, ec)) return root;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec) && it->path().filename() == "editor.exe")
            return it->path().parent_path();
    }
    return {};
}

} // namespace

void parse_feed(const std::string& body, std::vector<RemoteVersion>& out) {
    out.clear();
    std::stringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        std::vector<std::string> cols;
        size_t start = 0;
        for (size_t i = 0; i <= t.size(); ++i)
            if (i == t.size() || t[i] == '\t') { cols.push_back(t.substr(start, i - start)); start = i + 1; }
        if (cols.size() < 2) continue;
        RemoteVersion rv;
        rv.version = trim(cols[0]);
        rv.url     = trim(cols[1]);
        rv.notes   = cols.size() > 2 ? trim(cols[2]) : "";
        if (!rv.version.empty() && !rv.url.empty()) out.push_back(std::move(rv));
    }
}

bool fetch_feed(const std::string& feed_url_, std::vector<RemoteVersion>& out, std::string* err) {
    std::string body;
    if (!http_get_string(feed_url_, body, err)) return false;
    parse_feed(body, out);
    return true;
}

bool download_and_install(const RemoteVersion& rv, std::string* err,
                          const std::function<void(const std::string&)>& progress) {
    auto note = [&](const std::string& m) { if (progress) progress(m); spdlog::info("[update] {}", m); };
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path() / ("gws_update_" + rv.version);
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    fs::path zip = tmp / "package.zip";
    note("Downloading " + rv.version + "...");
    if (!http_download_file(rv.url, zip.string(), err)) { fs::remove_all(tmp, ec); return false; }

    note("Extracting...");
    fs::path ex = tmp / "extract";
    if (!extract_archive(zip.string(), ex.string(), err)) { fs::remove_all(tmp, ec); return false; }

    fs::path engine = find_engine_dir(ex);
    if (engine.empty()) {
        if (err) *err = "no editor.exe in the downloaded package";
        fs::remove_all(tmp, ec);
        return false;
    }

    note("Installing " + rv.version + "...");
    bool ok = EngineRegistry::install_version(engine.string(), rv.version, err);
    fs::remove_all(tmp, ec);
    if (ok) note("Installed " + rv.version + ".");
    return ok;
}

std::string feed_url() {
    std::ifstream in(config_dir() / "update_feed.txt");
    if (in) {
        std::string url;
        std::getline(in, url);
        url = trim(url);
        if (!url.empty() && url[0] != '#') return url;
    }
    // Placeholder default — the user sets a real feed URL in the Hub.
    return "https://example.com/gameworldshaper/feed.txt";
}

void set_feed_url(const std::string& url) {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream(config_dir() / "update_feed.txt", std::ios::trunc) << url << "\n";
}

} // namespace schizo::project
