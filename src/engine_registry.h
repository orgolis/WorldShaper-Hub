#pragma once
// ============================================================================
// Engine-version registry for the Hub.
//
// Installed engine versions live in `%LOCALAPPDATA%/GameWorldshaper/Engines/
// <version>/` (each a self-contained editor build). The in-repo development
// build is also registered as the "dev" version so the Hub works before any
// formal install exists. A project's manifest binds one version; opening the
// project launches that version's editor.exe with `--project <path>`.
// ============================================================================
#include <string>
#include <vector>

namespace schizo::project {

struct EngineVersion {
    std::string version;      // "0.1.0", or "dev" for the in-repo build
    std::string editor_exe;   // full path to editor.exe / GameWorldshaper editor
    std::string install_dir;  // folder containing it
    bool        is_dev = false;

    bool valid() const { return !editor_exe.empty(); }
};

class EngineRegistry {
public:
    // Scan the engines dir for installed versions. If `dev_editor_exe` is a real
    // file, also register it as the "dev" version (first in the list).
    void scan(const std::string& dev_editor_exe = "");

    const std::vector<EngineVersion>& versions() const { return versions_; }
    const EngineVersion* find(const std::string& version) const;
    // Best pick when a project's bound version is missing: highest real version,
    // else the dev build, else null.
    const EngineVersion* best() const;
    bool empty() const { return versions_.empty(); }

    // `%LOCALAPPDATA%/GameWorldshaper/Engines`
    static std::string engines_dir();

    // ---- install / update / remove (Phase 4) ----
    // Install (or overwrite = update) a version by copying an engine folder
    // (one that contains editor.exe) into engines_dir()/<version>/. Returns
    // false + fills `err` on failure. Call scan() afterwards to refresh.
    static bool install_version(const std::string& source_dir,
                                const std::string& version, std::string* err = nullptr);
    // Delete a user-installed version's folder (only under engines_dir()).
    static bool remove_installed_version(const std::string& version, std::string* err = nullptr);
    // True if this version lives under engines_dir() (user-installed → removable);
    // false for the dev build and versions bundled next to the Hub.
    static bool is_user_installed(const EngineVersion& v);

private:
    std::vector<EngineVersion> versions_;
};

// Launch an engine version's editor, opening `project_manifest_path`.
// Returns true if the process was spawned. Working dir = the editor's folder.
bool launch_editor(const EngineVersion& ev, const std::string& project_manifest_path);

// Full path of the currently-running executable (used to find the sibling
// editor.exe for the dev version).
std::string this_executable_path();

} // namespace schizo::project
