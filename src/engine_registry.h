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

// A version the Hub has installed at some point. Persisted so the Hub knows what
// "was installed" even after a version's folder is removed (present=false) or the
// Hub itself is reinstalled while the engines were kept.
struct InstalledRecord {
    std::string version;
    std::string dir;            // last known install folder
    bool        present = false;  // its folder currently exists on disk
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

    // Install history (versions ever installed, with a `present` flag set by the
    // last scan()). Reconciled with what's actually on disk each scan.
    const std::vector<InstalledRecord>& history() const { return history_; }
    // History entries whose folder is no longer present ("were installed").
    std::vector<InstalledRecord> previously_installed() const;

    // `%LOCALAPPDATA%/GameWorldshaper/Engines`
    static std::string engines_dir();

    // ---- install history (persisted in engines_dir()/installed_versions.txt) ----
    static std::string                  history_file();
    static std::vector<InstalledRecord> load_history();
    static void                         save_history(const std::vector<InstalledRecord>& h);
    static void record_installed(const std::string& version, const std::string& dir);
    static void forget_version(const std::string& version);   // drop a history entry

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
    std::vector<EngineVersion>   versions_;
    std::vector<InstalledRecord> history_;
};

// Launch an engine version's editor, opening `project_manifest_path`.
// Returns true if the process was spawned. Working dir = the editor's folder.
bool launch_editor(const EngineVersion& ev, const std::string& project_manifest_path);

// Full path of the currently-running executable (used to find the sibling
// editor.exe for the dev version).
std::string this_executable_path();

} // namespace schizo::project
