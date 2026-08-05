#include "engine_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace schizo::project {

std::string this_executable_path() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return (n > 0) ? std::string(buf, n) : std::string();
#else
    return {};
#endif
}

std::string EngineRegistry::engines_dir() {
    if (const char* la = std::getenv("LOCALAPPDATA"))
        return (fs::path(la) / "GameWorldshaper" / "Engines").string();
    if (const char* home = std::getenv("USERPROFILE"))
        return (fs::path(home) / ".gameworldshaper" / "Engines").string();
    return (fs::path(".") / "engines").string();
}

// The editor binary lives next to editor.exe in an install/build; probe common
// names so this survives an OUTPUT_NAME rename of the editor target.
static std::string find_editor_in(const fs::path& dir) {
    for (const char* name : {"editor.exe", "GameWorldshaperEditor.exe", "editor"}) {
        fs::path p = dir / name;
        std::error_code ec;
        if (fs::exists(p, ec)) return p.string();
    }
    return {};
}

std::string EngineRegistry::history_file() {
    return (fs::path(engines_dir()) / "installed_versions.txt").string();
}

// TSV: one record per line, `version \t last_install_dir` (# comments).
std::vector<InstalledRecord> EngineRegistry::load_history() {
    std::vector<InstalledRecord> out;
    std::ifstream in(history_file());
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back()=='\r' || line.back()=='\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        InstalledRecord r;
        r.version = tab == std::string::npos ? line : line.substr(0, tab);
        r.dir     = tab == std::string::npos ? ""   : line.substr(tab + 1);
        if (!r.version.empty()) out.push_back(std::move(r));
    }
    return out;
}

void EngineRegistry::save_history(const std::vector<InstalledRecord>& h) {
    std::error_code ec;
    fs::create_directories(fs::path(engines_dir()), ec);
    std::ofstream out(history_file(), std::ios::trunc);
    out << "# GameWorldshaper engine-version install history: version <TAB> last install dir\n";
    for (const auto& r : h) out << r.version << '\t' << r.dir << '\n';
}

void EngineRegistry::record_installed(const std::string& version, const std::string& dir) {
    if (version.empty()) return;
    auto h = load_history();
    auto it = std::find_if(h.begin(), h.end(), [&](const InstalledRecord& r){ return r.version == version; });
    if (it != h.end()) it->dir = dir;
    else               h.push_back({version, dir, true});
    save_history(h);
}

void EngineRegistry::forget_version(const std::string& version) {
    auto h = load_history();
    h.erase(std::remove_if(h.begin(), h.end(),
                           [&](const InstalledRecord& r){ return r.version == version; }), h.end());
    save_history(h);
}

std::vector<InstalledRecord> EngineRegistry::previously_installed() const {
    std::vector<InstalledRecord> out;
    for (const auto& r : history_) if (!r.present) out.push_back(r);
    return out;
}

void EngineRegistry::scan(const std::string& dev_editor_exe) {
    versions_.clear();

    // 1) The in-repo / sibling dev build.
    if (!dev_editor_exe.empty()) {
        std::error_code ec;
        if (fs::exists(dev_editor_exe, ec)) {
            EngineVersion dev;
            dev.version     = "dev";
            dev.editor_exe  = dev_editor_exe;
            dev.install_dir = fs::path(dev_editor_exe).parent_path().string();
            dev.is_dev      = true;
            versions_.push_back(dev);
        }
    }

    // 2) Installed versions. Scan two roots (dedup by exe): versions bundled
    //    next to the Hub (installer layout: <hub>/Engines/<ver>/) and versions
    //    the user installed/updated under %LOCALAPPDATA%/.../Engines/<ver>/.
    auto already = [&](const std::string& exe) {
        for (const auto& v : versions_) if (v.editor_exe == exe) return true;
        return false;
    };
    auto scan_root = [&](const fs::path& root) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (!e.is_directory()) continue;
            std::string exe = find_editor_in(e.path());
            if (exe.empty()) exe = find_editor_in(e.path() / "bin");
            if (exe.empty() || already(exe)) continue;
            EngineVersion v;
            v.version     = e.path().filename().string();
            v.editor_exe  = exe;
            v.install_dir = e.path().string();
            versions_.push_back(v);
        }
    };
    fs::path exe_dir = fs::path(this_executable_path()).parent_path();
    scan_root(exe_dir / "Engines");
    scan_root(fs::path(engines_dir()));

    // 3) Reconcile the install history: capture any present (non-dev) version not
    //    yet recorded, then mark each record present/absent by what's on disk.
    history_ = load_history();
    bool changed = false;
    for (const auto& v : versions_) {
        if (v.is_dev) continue;
        auto it = std::find_if(history_.begin(), history_.end(),
                               [&](const InstalledRecord& r){ return r.version == v.version; });
        if (it == history_.end()) { history_.push_back({v.version, v.install_dir, true}); changed = true; }
        else if (it->dir != v.install_dir) { it->dir = v.install_dir; changed = true; }
    }
    size_t present = 0;
    for (auto& r : history_) {
        const EngineVersion* live = find(r.version);
        r.present = (live != nullptr && !live->is_dev);
        if (r.present) ++present;
    }
    if (changed) save_history(history_);

    spdlog::info("[engines] {} version(s) available; history: {} installed, {} previously installed",
                 versions_.size(), present, history_.size() - present);
}

bool EngineRegistry::is_user_installed(const EngineVersion& v) {
    if (v.is_dev) return false;
    std::error_code ec;
    fs::path root(engines_dir());
    // install_dir is engines_dir()/<version> for user-installed versions.
    return !v.install_dir.empty() &&
           fs::weakly_canonical(fs::path(v.install_dir).parent_path(), ec) ==
           fs::weakly_canonical(root, ec);
}

bool EngineRegistry::install_version(const std::string& source_dir,
                                     const std::string& version, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; spdlog::error("[engines] install: {}", m); return false; };
    if (version.empty()) return fail("empty version name");

    fs::path src(source_dir);
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) return fail("source folder not found");
    if (find_editor_in(src).empty()) return fail("source has no editor.exe");

    fs::path dest = fs::path(engines_dir()) / version;
    if (fs::weakly_canonical(src, ec) == fs::weakly_canonical(dest, ec))
        return fail("source is already the installed version");

    fs::create_directories(fs::path(engines_dir()), ec);
    if (fs::exists(dest, ec)) fs::remove_all(dest, ec);   // overwrite = update
    fs::copy(src, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return fail("copy failed: " + ec.message());

    record_installed(version, dest.string());   // remember it in the history
    spdlog::info("[engines] installed version '{}' -> {}", version, dest.string());
    return true;
}

bool EngineRegistry::remove_installed_version(const std::string& version, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; spdlog::error("[engines] remove: {}", m); return false; };
    fs::path dest = fs::path(engines_dir()) / version;
    std::error_code ec;
    if (!fs::exists(dest, ec)) return fail("version not installed");
    // Safety: only ever delete inside engines_dir().
    if (fs::weakly_canonical(dest.parent_path(), ec) != fs::weakly_canonical(fs::path(engines_dir()), ec))
        return fail("refusing to delete outside the engines dir");
    fs::remove_all(dest, ec);
    if (ec) return fail("delete failed: " + ec.message());
    spdlog::info("[engines] removed version '{}'", version);
    return true;
}

const EngineVersion* EngineRegistry::find(const std::string& version) const {
    for (const auto& v : versions_)
        if (v.version == version) return &v;
    return nullptr;
}

const EngineVersion* EngineRegistry::best() const {
    const EngineVersion* pick = nullptr;
    for (const auto& v : versions_) {
        if (v.is_dev) { if (!pick) pick = &v; continue; }
        if (!pick || pick->is_dev || v.version > pick->version) pick = &v;  // lexicographic-ish
    }
    return pick;
}

bool launch_editor(const EngineVersion& ev, const std::string& project_manifest_path) {
    if (!ev.valid()) { spdlog::error("[engines] launch: invalid version"); return false; }
#ifdef _WIN32
    std::string cmd = "\"" + ev.editor_exe + "\" --project \"" + project_manifest_path + "\"";
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::string workdir = ev.install_dir;

    BOOL ok = CreateProcessA(
        ev.editor_exe.c_str(),           // application
        mutable_cmd.data(),              // command line
        nullptr, nullptr, FALSE,
        CREATE_NEW_PROCESS_GROUP,
        nullptr,
        workdir.empty() ? nullptr : workdir.c_str(),
        &si, &pi);
    if (!ok) { spdlog::error("[engines] CreateProcess failed ({})", GetLastError()); return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    spdlog::info("[engines] launched {} ({}) -> {}", ev.version, ev.editor_exe, project_manifest_path);
    return true;
#else
    (void)project_manifest_path;
    return false;
#endif
}

} // namespace schizo::project
