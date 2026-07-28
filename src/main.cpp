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
#include <cstring>
#include <filesystem>
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
    char       install_src[512] = {0};   // engine folder to install
    char       install_ver[64]  = {0};   // version name to install as
    std::vector<RemoteVersion> remote_versions;  // last "Check for Updates" result
    char       repo_buf[256] = {0};              // GitHub "owner/repo" to pull from
    { std::string r = github_repo(); std::snprintf(repo_buf, sizeof(repo_buf), "%s", r.c_str()); }

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
                ImGui::Separator();
                if (engines.versions().empty())
                    ImGui::TextDisabled("No engine versions found.");
                for (const auto& v : engines.versions()) {
                    ImGui::PushID(v.editor_exe.c_str());
                    ImGui::BulletText("%s%s", v.version.c_str(), v.is_dev ? "   (dev build)" : "");
                    ImGui::SameLine(); ImGui::TextDisabled("   %s", v.editor_exe.c_str());
                    if (EngineRegistry::is_user_installed(v)) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            std::string err;
                            if (EngineRegistry::remove_installed_version(v.version, &err)) {
                                status = "Removed engine " + v.version + ".";
                                engines.scan(dev_editor);
                            } else status = "Remove failed: " + err;
                        }
                    }
                    ImGui::PopID();
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
                ImGui::TextDisabled("Pulls engine versions from a GitHub repo's releases.");
                ImGui::SetNextItemWidth(340);
                ImGui::InputText("owner/repo", repo_buf, sizeof(repo_buf));
                ImGui::SameLine();
                if (ImGui::Button("Check for Updates")) {
                    set_github_repo(repo_buf);
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
                            if (download_and_install(rv, &e)) {
                                status = "Installed " + rv.version + ".";
                                engines.scan(dev_editor);
                            } else status = "Update failed: " + e;
                        }
                        ImGui::PopID();
                    }
                }

                ImGui::Dummy(ImVec2(0, 8));
                if (ImGui::Button("Rescan")) engines.scan(dev_editor);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

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
