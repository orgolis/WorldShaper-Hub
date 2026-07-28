#include "engine_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

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

    spdlog::info("[engines] {} version(s) available", versions_.size());
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
