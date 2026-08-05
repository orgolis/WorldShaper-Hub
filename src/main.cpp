// ============================================================================
// GameWorldshaper Hub — a standalone launcher that manages engine versions and
// projects (Unity-Hub style). Separate from any single editor build because it
// launches *different* engine versions. Lightweight GLFW + legacy-GL + ImGui.
//
// Lists/creates/opens projects (each bound to an engine version) and launches
// the bound editor with `--project`. Installs engine versions from a local
// folder or from a GitHub repo's Releases. A sibling editor build is registered
// as the "dev" version so this works before any formal install.
//
// Standalone repo: deps (GLFW, ImGui, spdlog, nlohmann/json) come via CMake
// FetchContent; it does not depend on the engine source tree.
// ============================================================================
#include "project.h"
#include "engine_registry.h"
#include "update_feed.h"
#include "github_releases.h"
#include "self_update.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl2.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace schizo::project;

// ---- native folder picker (Windows) ----
static std::string browse_folder(const char* title) {
#ifdef _WIN32
    char path[MAX_PATH] = {0};
    BROWSEINFOA bi{};
    bi.lpszTitle = title;
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl) { SHGetPathFromIDListA(pidl, path); CoTaskMemFree(pidl); return path; }
#endif
    (void)title;
    return {};
}

// ---- inline feature checklist (with dependency handling) ----
static void draw_feature_checklist(FeatureSet& features) {
    for (const auto& fi : feature_table()) {
        const char* forcer = nullptr;
        for (const auto& g : feature_table())
            if (g.depends_on == fi.id && features.has(g.id)) { forcer = g.name; break; }
        bool on = features.has(fi.id);
        ImGui::PushID((int)fi.id);
        if (forcer) {
            bool t = true;
            ImGui::BeginDisabled(); ImGui::Checkbox(fi.name, &t); ImGui::EndDisabled();
        } else if (ImGui::Checkbox(fi.name, &on)) {
            features.set(fi.id, on);
            if (on) features.resolve_dependencies();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", fi.desc);
        ImGui::PopID();
    }
}

static void glfw_error(int e, const char* d) { std::fprintf(stderr, "GLFW %d: %s\n", e, d); }

// ---- Hub uninstall ---------------------------------------------------------
// %APPDATA%/GameWorldshaper — where the Hub persists repo/token/recent-projects.
static fs::path hub_config_dir() {
#ifdef _WIN32
    if (const char* ad = std::getenv("APPDATA")) return fs::path(ad) / "GameWorldshaper";
#endif
    if (const char* hp = std::getenv("USERPROFILE")) return fs::path(hp) / ".gameworldshaper";
    return fs::path(".") / ".gameworldshaper";
}

// Delete the Hub's persisted config, and — only if `remove_engines` — every
// installed engine version too. Keeping the engines lets a reinstalled Hub
// rediscover them (and their install history). (The Hub's own program files are
// handled separately below.)
static void remove_hub_data(bool remove_engines) {
    std::error_code ec;
    if (remove_engines) {
        const fs::path engines = EngineRegistry::engines_dir();  // %LOCALAPPDATA%/.../Engines
        fs::remove_all(engines, ec);
        fs::remove(engines.parent_path(), ec);                   // %LOCALAPPDATA%/GameWorldshaper if empty
    }
    fs::remove_all(hub_config_dir(), ec);                        // %APPDATA%/GameWorldshaper (Hub config)
}

#ifdef _WIN32
// NSIS (CPack) drops an uninstaller next to the Hub when installed; portable ZIP
// runs have none.
static fs::path find_uninstaller() {
    const fs::path dir = fs::path(this_executable_path()).parent_path();
    std::error_code ec;
    for (const char* n : { "Uninstall.exe", "uninstall.exe" })
        if (fs::exists(dir / n, ec)) return dir / n;
    return {};
}

static bool run_detached(std::string command_line, DWORD flags) {
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// A running exe can't delete itself, so for a portable install spawn a detached
// cmd that waits for this process to exit, then deletes the Hub exe + its bundled
// MinGW DLLs and removes the folder if it becomes empty.
static bool spawn_portable_selfdelete(std::string& err) {
    const fs::path exe = this_executable_path();
    const fs::path dir = exe.parent_path();
    const char* tmp = std::getenv("TEMP"); if (!tmp) tmp = std::getenv("TMP");
    const fs::path bat = fs::path(tmp ? tmp : ".") / "gws_hub_uninstall.bat";
    std::ofstream b(bat, std::ios::binary);
    if (!b) { err = "could not write the uninstall helper"; return false; }
    auto del = [&](const fs::path& p) { b << "del \"" << p.string() << "\" >nul 2>&1\r\n"; };
    b << "@echo off\r\n:wait\r\n";
    b << "del \"" << exe.string() << "\" >nul 2>&1\r\n";
    b << "if exist \"" << exe.string() << "\" ( ping -n 2 127.0.0.1 >nul & goto wait )\r\n";
    del(dir / "libgcc_s_seh-1.dll");
    del(dir / "libstdc++-6.dll");
    del(dir / "libwinpthread-1.dll");
    b << "rmdir \"" << dir.string() << "\" >nul 2>&1\r\n";
    b << "del \"%~f0\" >nul 2>&1\r\n";
    b.close();
    if (!run_detached("cmd.exe /c \"" + bat.string() + "\"", CREATE_NO_WINDOW | DETACHED_PROCESS)) {
        err = "could not launch the uninstall helper";
        return false;
    }
    return true;
}

// Uninstall the Hub: remove its data (optionally the engine versions too), then
// run the NSIS uninstaller (installed) or self-delete (portable). On success the
// caller closes the window so the exe unlocks and the deletion can complete.
static bool uninstall_hub(std::string& msg, bool remove_engines) {
    remove_hub_data(remove_engines);
    const fs::path un = find_uninstaller();
    if (!un.empty()) {
        if (!run_detached("\"" + un.string() + "\"", 0)) { msg = "Could not launch the uninstaller."; return false; }
        return true;
    }
    std::string err;
    if (!spawn_portable_selfdelete(err)) { msg = err; return false; }
    return true;
}
#else
static bool uninstall_hub(std::string& msg, bool remove_engines) {
    remove_hub_data(remove_engines);
    msg = "Removed Hub data. Delete the Hub folder manually on this platform.";
    return true;
}
#endif

int main() {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) return 1;
    GLFWwindow* win = glfwCreateWindow(1000, 660, "GameWorldshaper Hub", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // the hub doesn't persist window layout
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL2_Init();

    ProjectsRegistry projects; projects.load();
    EngineRegistry   engines;
    const std::string dev_editor =
        (fs::path(this_executable_path()).parent_path() / "editor.exe").string();
    engines.scan(dev_editor);

    // New-project form state.
    char       new_name[128] = "MyGame";
    char       new_loc[512]  = {0};
    { std::string d = default_projects_dir(); std::snprintf(new_loc, sizeof(new_loc), "%s", d.c_str()); }
    FeatureSet new_features = FeatureSet::defaults();
    int        new_engine_idx = 0;
    int        sel_project = -1;
    std::string status;
    {   // On launch, report what engine versions are installed / were installed.
        size_t inst = 0; for (const auto& v : engines.versions()) if (!v.is_dev) ++inst;
        const size_t prev = engines.previously_installed().size();
        status = "Detected " + std::to_string(inst) + " installed engine version(s)"
               + (prev ? ", " + std::to_string(prev) + " previously installed." : ".");
    }
    char       install_src[512] = {0};   // engine folder to install
    char       install_ver[64]  = {0};   // version name to install as
    std::vector<RemoteVersion> remote_versions;  // last "Check for Updates" result
    char       repo_buf[256] = {0};              // GitHub "owner/repo" to pull from
    { std::string r = github_repo(); std::snprintf(repo_buf, sizeof(repo_buf), "%s", r.c_str()); }
    char       token_buf[256] = {0};             // GitHub token for private repos
    { std::string t = github_token(); std::snprintf(token_buf, sizeof(token_buf), "%s", t.c_str()); }
    std::string engine_to_uninstall;             // pending engine-version uninstall (confirm modal)
    bool        open_engine_uninstall = false;
    bool        open_hub_uninstall    = false;
    bool        also_remove_engines   = false;   // Hub-uninstall option: also wipe engine versions
    bool        should_close       = false;  // set once uninstall is launched -> close the window
    // Hub self-update state.
    bool          hub_update_checked = false;    // has "Check" run this session
    bool          hub_update_found   = false;    // a newer Hub release exists
    RemoteVersion hub_update;                    // the newer release (when found)
    std::string   hub_update_msg;                // inline status for the Hub-update row

    auto open_project = [&](const std::string& manifest_path) {
        ProjectManifest pm;
        if (!ProjectManifest::load(manifest_path, pm)) { status = "Failed to read project."; return; }
        const EngineVersion* ev = engines.find(pm.engine_version);
        if (!ev) ev = engines.best();
        if (!ev) { status = "No engine version available."; return; }
        if (launch_editor(*ev, manifest_path)) {
            projects.add(RecentProject{pm.name, manifest_path}); projects.save();
            status = "Launched '" + pm.name + "' in engine " + ev->version + ".";
        } else status = "Failed to launch the editor.";
    };

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##hub", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::SetWindowFontScale(1.7f);
        ImGui::TextUnformatted("GameWorldshaper Hub");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("Manage your projects and engine versions");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));

        if (ImGui::BeginTabBar("hub_tabs")) {
            // ---------------- Projects ----------------
            if (ImGui::BeginTabItem("Projects")) {
                ImGui::TextDisabled("Double-click a project to open it in its engine version.");
                ImGui::BeginChild("projlist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), true);
                const auto& items = projects.items();
                for (int i = 0; i < (int)items.size(); ++i) {
                    const auto& it = items[i];
                    const bool exists = fs::exists(it.manifest_path);
                    std::string label = (it.name.empty() ? it.manifest_path : it.name);
                    if (!exists) label += "   (missing)";
                    ImGui::PushID(i);
                    if (ImGui::Selectable(label.c_str(), sel_project == i,
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        sel_project = i;
                        if (ImGui::IsMouseDoubleClicked(0) && exists) open_project(it.manifest_path);
                    }
                    ProjectManifest pm;
                    if (exists && ProjectManifest::load(it.manifest_path, pm)) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("   engine %s", pm.engine_version.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();

                const bool can = sel_project >= 0 && sel_project < (int)items.size() &&
                                 fs::exists(items[sel_project].manifest_path);
                ImGui::BeginDisabled(!can);
                if (ImGui::Button("Open", ImVec2(110, 0)) && can) open_project(items[sel_project].manifest_path);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Remove from list", ImVec2(150, 0)) &&
                    sel_project >= 0 && sel_project < (int)items.size()) {
                    projects.remove(items[sel_project].manifest_path); projects.save(); sel_project = -1;
                }
                ImGui::EndTabItem();
            }

            // ---------------- New Project ----------------
            if (ImGui::BeginTabItem("New Project")) {
                ImGui::SetNextItemWidth(340); ImGui::InputText("Name", new_name, sizeof(new_name));
                ImGui::SetNextItemWidth(340); ImGui::InputText("Location", new_loc, sizeof(new_loc));
                ImGui::SameLine();
                if (ImGui::Button("Browse...")) {
                    std::string p = browse_folder("Choose where to create the project");
                    if (!p.empty()) std::snprintf(new_loc, sizeof(new_loc), "%s", p.c_str());
                }

                const auto& evs = engines.versions();
                std::string preview = evs.empty() ? "(no engine installed)"
                                                  : evs[std::min(new_engine_idx, (int)evs.size()-1)].version;
                ImGui::SetNextItemWidth(340);
                if (ImGui::BeginCombo("Engine version", preview.c_str())) {
                    for (int i = 0; i < (int)evs.size(); ++i) {
                        std::string lbl = evs[i].version + (evs[i].is_dev ? "  (dev)" : "");
                        if (ImGui::Selectable(lbl.c_str(), new_engine_idx == i)) new_engine_idx = i;
                    }
                    ImGui::EndCombo();
                }

                ImGui::Dummy(ImVec2(0, 4));
                ImGui::TextUnformatted("Features");
                ImGui::TextDisabled("Core systems (Rendering, ECS, Assets, Editor) are always included.");
                ImGui::BeginChild("feat", ImVec2(0, 190), true);
                draw_feature_checklist(new_features);
                ImGui::EndChild();

                const bool can = std::strlen(new_name) > 0 && std::strlen(new_loc) > 0 && !evs.empty();
                ImGui::BeginDisabled(!can);
                if (ImGui::Button("Create & Open", ImVec2(150, 0)) && can) {
                    std::string ver = evs[std::min(new_engine_idx, (int)evs.size()-1)].version;
                    std::string mpath;
                    if (create_project(new_loc, new_name, new_features, mpath, ver)) open_project(mpath);
                    else status = "Could not create the project (name taken or path not writable?).";
                }
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }

            // ---------------- Engine Versions ----------------
            if (ImGui::BeginTabItem("Engine Versions")) {
                ImGui::TextDisabled("Installed under: %s", EngineRegistry::engines_dir().c_str());
                const auto prev = engines.previously_installed();
                {
                    size_t inst = 0; for (const auto& v : engines.versions()) if (!v.is_dev) ++inst;
                    ImGui::TextDisabled("%zu installed, %zu previously installed.", inst, prev.size());
                }
                ImGui::Separator();
                ImGui::TextUnformatted("Installed");
                if (engines.versions().empty())
                    ImGui::TextDisabled("No engine versions found.");
                for (const auto& v : engines.versions()) {
                    ImGui::PushID(v.editor_exe.c_str());
                    ImGui::BulletText("%s%s", v.version.c_str(), v.is_dev ? "   (dev build)" : "");
                    ImGui::SameLine(); ImGui::TextDisabled("   %s", v.editor_exe.c_str());
                    if (EngineRegistry::is_user_installed(v)) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Uninstall")) {
                            engine_to_uninstall  = v.version;
                            open_engine_uninstall = true;   // confirm before deleting
                        }
                    }
                    ImGui::PopID();
                }

                // Versions the Hub installed before but that aren't present now
                // (removed, or an engine folder deleted outside the Hub).
                if (!prev.empty()) {
                    ImGui::Dummy(ImVec2(0, 6));
                    ImGui::TextUnformatted("Previously installed");
                    ImGui::TextDisabled("Recorded as installed once, not present now.");
                    for (const auto& r : prev) {
                        ImGui::PushID(("prev" + r.version).c_str());
                        ImGui::BulletText("%s", r.version.c_str());
                        if (!r.dir.empty()) { ImGui::SameLine(); ImGui::TextDisabled("   was: %s", r.dir.c_str()); }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Forget")) {
                            EngineRegistry::forget_version(r.version);
                            engines.scan(dev_editor);
                            status = "Forgot previously-installed engine " + r.version + ".";
                        }
                        ImGui::PopID();
                    }
                }

                ImGui::Dummy(ImVec2(0, 10));
                ImGui::Separator();
                ImGui::TextUnformatted("Install / Update a version");
                ImGui::TextDisabled("Point to an engine folder (contains editor.exe) — e.g. an unpacked "
                                    "package's Engines/<version> dir. Same version name = update.");
                ImGui::SetNextItemWidth(340);
                ImGui::InputText("Folder", install_src, sizeof(install_src));
                ImGui::SameLine();
                if (ImGui::Button("Browse...##inst")) {
                    std::string p = browse_folder("Choose an engine folder (with editor.exe)");
                    if (!p.empty()) {
                        std::snprintf(install_src, sizeof(install_src), "%s", p.c_str());
                        std::string base = fs::path(p).filename().string();
                        if (base.empty()) base = fs::path(p).parent_path().filename().string();
                        std::snprintf(install_ver, sizeof(install_ver), "%s", base.c_str());
                    }
                }
                ImGui::SetNextItemWidth(340);
                ImGui::InputText("Version name", install_ver, sizeof(install_ver));
                const bool can_install = std::strlen(install_src) > 0 && std::strlen(install_ver) > 0;
                ImGui::BeginDisabled(!can_install);
                if (ImGui::Button("Install / Update", ImVec2(160, 0)) && can_install) {
                    std::string err;
                    if (EngineRegistry::install_version(install_src, install_ver, &err)) {
                        status = "Installed engine " + std::string(install_ver) + ".";
                        engines.scan(dev_editor);
                        install_src[0] = '\0'; install_ver[0] = '\0';
                    } else status = "Install failed: " + err;
                }
                ImGui::EndDisabled();

                ImGui::Dummy(ImVec2(0, 10));
                ImGui::Separator();
                ImGui::TextUnformatted("Remote updates (GitHub Releases)");
                ImGui::TextDisabled("Pulls engine versions from a public GitHub repo's releases — no token needed.");
                ImGui::SetNextItemWidth(340);
                ImGui::InputText("owner/repo", repo_buf, sizeof(repo_buf));
                // Token is OPTIONAL — only for private engine repos — so it lives in
                // a collapsed "Advanced" section and the default flow stays token-free.
                if (ImGui::TreeNode("Advanced: private-repo token (optional)")) {
                    ImGui::SetNextItemWidth(340);
                    ImGui::InputText("token", token_buf, sizeof(token_buf), ImGuiInputTextFlags_Password);
                    ImGui::SameLine(); ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Only needed if the engine repo is PRIVATE.\n"
                                          "Leave empty for public repos. Saved locally.");
                    ImGui::TreePop();
                }
                if (ImGui::Button("Check for Updates")) {
                    set_github_repo(repo_buf);
                    set_github_token(token_buf);
                    std::string spec = repo_buf, e;
                    size_t slash = spec.find('/');
                    if (slash == std::string::npos || slash == 0 || slash + 1 >= spec.size()) {
                        status = "Enter the repo as owner/repo.";
                    } else if (fetch_github_releases(spec.substr(0, slash), spec.substr(slash + 1),
                                                     remote_versions, &e)) {
                        status = std::to_string(remote_versions.size()) + " release(s) available.";
                    } else {
                        status = "GitHub error: " + e;
                    }
                }
                for (const auto& rv : remote_versions) {
                    const bool installed = engines.find(rv.version) != nullptr;
                    ImGui::BulletText("%s%s", rv.version.c_str(), installed ? "   (installed)" : "");
                    if (!rv.notes.empty()) { ImGui::SameLine(); ImGui::TextDisabled("   %s", rv.notes.c_str()); }
                    if (!installed) {
                        ImGui::SameLine();
                        ImGui::PushID(rv.url.c_str());
                        if (ImGui::SmallButton("Download & Install")) {
                            std::string e;
                            status = "Downloading " + rv.version + "...";
                            if (download_and_install(rv, &e, nullptr, github_download_headers())) {
                                status = "Installed " + rv.version + ".";
                                engines.scan(dev_editor);
                            } else status = "Update failed: " + e;
                        }
                        ImGui::PopID();
                    }
                }

                ImGui::Dummy(ImVec2(0, 8));
                if (ImGui::Button("Rescan")) engines.scan(dev_editor);

                // Confirm modal for uninstalling an engine version.
                if (open_engine_uninstall) { ImGui::OpenPopup("Uninstall engine version?"); open_engine_uninstall = false; }
                if (ImGui::BeginPopupModal("Uninstall engine version?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Permanently delete engine version \"%s\"?", engine_to_uninstall.c_str());
                    ImGui::TextDisabled("Removes its files under %s.", EngineRegistry::engines_dir().c_str());
                    ImGui::TextDisabled("Projects bound to it will fall back to another installed version.");
                    ImGui::Separator();
                    if (ImGui::Button("Uninstall", ImVec2(120, 0))) {
                        std::string err;
                        if (EngineRegistry::remove_installed_version(engine_to_uninstall, &err)) {
                            status = "Uninstalled engine " + engine_to_uninstall + ".";
                            engines.scan(dev_editor);
                        } else status = "Uninstall failed: " + err;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::EndTabItem();
            }

            // ---------------- Settings / Maintenance ----------------
            if (ImGui::BeginTabItem("Settings")) {
                // ---- Hub self-update ----
                ImGui::TextUnformatted("Hub");
                ImGui::BulletText("This Hub: version %s   (%s)", hub_version().c_str(), hub_repo().c_str());
                if (ImGui::Button("Check for Hub Updates")) {
                    std::string e;
                    hub_update_checked = true;
                    if (check_hub_update(hub_update, hub_update_found, &e))
                        hub_update_msg = hub_update_found ? ("Hub " + hub_update.version + " is available.")
                                                          : "The Hub is up to date.";
                    else { hub_update_found = false; hub_update_msg = "Hub update check failed: " + e; }
                }
                if (hub_update_found) {
                    ImGui::SameLine();
                    if (ImGui::Button(("Update to " + hub_update.version + " & Restart").c_str())) {
                        std::string e;
                        if (apply_hub_update(hub_update, &e,
                                             [&](const std::string& m){ hub_update_msg = m; }))
                            should_close = true;   // reuse the close-the-window flag; helper relaunches
                        else hub_update_msg = "Update failed: " + e;
                    }
                }
                if (hub_update_checked && !hub_update_msg.empty())
                    ImGui::TextDisabled("%s", hub_update_msg.c_str());

                ImGui::Dummy(ImVec2(0, 12));
                ImGui::Separator();
                ImGui::TextUnformatted("Locations");
                ImGui::BulletText("Engine versions: %s", EngineRegistry::engines_dir().c_str());
                ImGui::BulletText("Hub config:      %s", hub_config_dir().string().c_str());

                ImGui::Dummy(ImVec2(0, 12));
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "Danger zone");
                ImGui::TextDisabled("Uninstall removes the Hub and its config. It asks whether to also");
                ImGui::TextDisabled("delete installed engine versions. Your project folders are NOT touched.");
                ImGui::Dummy(ImVec2(0, 4));
                if (ImGui::Button("Uninstall the Hub...", ImVec2(200, 0))) {
                    also_remove_engines = false;   // default: keep the engines
                    open_hub_uninstall  = true;
                }

                if (open_hub_uninstall) { ImGui::OpenPopup("Uninstall the Hub?"); open_hub_uninstall = false; }
                if (ImGui::BeginPopupModal("Uninstall the Hub?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    size_t engine_count = 0;
                    for (const auto& v : engines.versions()) if (!v.is_dev) ++engine_count;

                    ImGui::TextUnformatted("This will:");
                    ImGui::BulletText("delete the Hub's saved config (repo, recent projects)");
                    ImGui::BulletText("%s", find_uninstaller().empty()
                                          ? "delete the Hub program files, then close"
                                          : "run the Hub uninstaller, then close");
                    ImGui::Dummy(ImVec2(0, 4));
                    ImGui::Checkbox("Also uninstall all installed engine versions", &also_remove_engines);
                    if (also_remove_engines)
                        ImGui::TextDisabled("   %zu installed engine version(s) + install history will be deleted.",
                                            engine_count);
                    else
                        ImGui::TextDisabled("   %zu installed engine version(s) will be KEPT; a reinstalled Hub finds them.",
                                            engine_count);
                    ImGui::TextDisabled("Your game projects on disk are left alone.");
                    ImGui::Separator();
                    if (ImGui::Button("Uninstall", ImVec2(120, 0))) {
                        std::string msg;
                        if (uninstall_hub(msg, also_remove_engines)) { should_close = true; ImGui::CloseCurrentPopup(); }
                        else { status = msg; ImGui::CloseCurrentPopup(); }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        // A launched Hub uninstall closes the app so the exe unlocks and the
        // uninstaller/self-delete helper can finish removing it.
        if (should_close) glfwSetWindowShouldClose(win, 1);

        if (!status.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", status.c_str());
        }

        ImGui::End();

        ImGui::Render();
        int fbw, fbh; glfwGetFramebufferSize(win, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
