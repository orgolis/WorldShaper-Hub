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
#include <shellapi.h>   // ShellExecuteA (elevated uninstaller launch)
#include <shobjidl.h>   // IFileOpenDialog (modern native folder picker)
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace schizo::project;

// ---- native folder picker (modern Windows Explorer dialog) ----
static std::string browse_folder(const char* title) {
#ifdef _WIN32
    std::string result;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool did_init = SUCCEEDED(co);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg))) && dlg) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (title) {
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
            if (wlen > 0) { std::wstring wt(static_cast<size_t>(wlen), L'\0'); MultiByteToWideChar(CP_UTF8, 0, title, -1, wt.data(), wlen); dlg->SetTitle(wt.c_str()); }
        }
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR pw = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pw)) && pw) {
                    const int n = WideCharToMultiByte(CP_UTF8, 0, pw, -1, nullptr, 0, nullptr, nullptr);
                    if (n > 1) { std::string s(static_cast<size_t>(n - 1), '\0'); WideCharToMultiByte(CP_UTF8, 0, pw, -1, s.data(), n, nullptr, nullptr); result = s; }
                    CoTaskMemFree(pw);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (did_init) CoUninitialize();
    return result;
#else
    (void)title;
    return {};
#endif
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

namespace hub_ui {

constexpr ImVec4 kAccent      {0.10f, 0.68f, 0.82f, 1.00f};
constexpr ImVec4 kAccentHot   {0.16f, 0.78f, 0.91f, 1.00f};
constexpr ImVec4 kMuted       {0.52f, 0.59f, 0.67f, 1.00f};
constexpr ImVec4 kWarning     {0.98f, 0.70f, 0.25f, 1.00f};
constexpr ImVec4 kDanger      {0.94f, 0.34f, 0.38f, 1.00f};

static void apply_theme(GLFWwindow* window) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding     = ImVec2(14.0f, 12.0f);
    style.FramePadding      = ImVec2(10.0f, 7.0f);
    style.CellPadding       = ImVec2(8.0f, 6.0f);
    style.ItemSpacing       = ImVec2(9.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(7.0f, 5.0f);
    style.ScrollbarSize     = 13.0f;
    style.GrabMinSize       = 11.0f;
    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 7.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 7.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;

    constexpr ImVec4 text   {0.91f, 0.94f, 0.97f, 1.00f};
    constexpr ImVec4 canvas {0.030f, 0.041f, 0.058f, 1.00f};
    constexpr ImVec4 panel  {0.050f, 0.066f, 0.090f, 1.00f};
    constexpr ImVec4 raised {0.075f, 0.098f, 0.132f, 1.00f};
    constexpr ImVec4 border {0.14f, 0.19f, 0.26f, 1.00f};

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                  = text;
    c[ImGuiCol_TextDisabled]          = kMuted;
    c[ImGuiCol_WindowBg]              = canvas;
    c[ImGuiCol_ChildBg]               = panel;
    c[ImGuiCol_PopupBg]               = ImVec4(0.043f, 0.057f, 0.078f, 0.98f);
    c[ImGuiCol_Border]                = border;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = raised;
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.11f, 0.17f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.12f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_TitleBg]               = panel;
    c[ImGuiCol_TitleBgActive]         = raised;
    c[ImGuiCol_TitleBgCollapsed]      = panel;
    c[ImGuiCol_MenuBarBg]             = panel;
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.02f, 0.03f, 0.04f, 0.75f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.20f, 0.26f, 0.33f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.27f, 0.35f, 0.43f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.34f, 0.44f, 0.52f, 1.00f);
    c[ImGuiCol_CheckMark]             = kAccentHot;
    c[ImGuiCol_SliderGrab]            = kAccent;
    c[ImGuiCol_SliderGrabActive]      = kAccentHot;
    c[ImGuiCol_Button]                = raised;
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.09f, 0.42f, 0.51f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.08f, 0.58f, 0.69f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.09f, 0.31f, 0.38f, 0.82f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.11f, 0.43f, 0.52f, 0.92f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.10f, 0.55f, 0.66f, 1.00f);
    c[ImGuiCol_Separator]             = border;
    c[ImGuiCol_SeparatorHovered]      = kAccent;
    c[ImGuiCol_SeparatorActive]       = kAccentHot;
    c[ImGuiCol_ResizeGrip]            = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.20f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.67f);
    c[ImGuiCol_ResizeGripActive]      = kAccentHot;
    c[ImGuiCol_Tab]                   = raised;
    c[ImGuiCol_TabHovered]            = ImVec4(0.10f, 0.42f, 0.51f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.08f, 0.30f, 0.37f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = panel;
    c[ImGuiCol_TabUnfocusedActive]    = raised;
    c[ImGuiCol_TextSelectedBg]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.34f);
    c[ImGuiCol_DragDropTarget]        = kWarning;
    c[ImGuiCol_NavHighlight]          = kAccentHot;
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.01f, 0.02f, 0.03f, 0.72f);

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    const float ui_scale = std::clamp(std::max(xscale, yscale), 1.0f, 1.75f);
    if (ui_scale > 1.01f) style.ScaleAllSizes(ui_scale);

    ImGuiIO& io = ImGui::GetIO();
    const char* font_candidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/TTF/Inter-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
    };
    for (const char* candidate : font_candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec)) continue;
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(candidate, 15.5f * ui_scale)) {
            io.FontDefault = font;
            break;
        }
    }
}

static void page_header(const char* title, const char* description) {
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(title);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("%s", description);
    ImGui::Dummy(ImVec2(0, 3));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 5));
}

static bool nav_item(const char* label, bool selected) {
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.08f, 0.5f));
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.08f, 0.34f, 0.42f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccentHot);
    }
    const bool clicked = ImGui::Selectable(label, selected, 0, ImVec2(0, 42.0f));
    if (selected) ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    return clicked;
}

static bool primary_button(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.08f, 0.47f, 0.57f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.62f, 0.73f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.08f, 0.70f, 0.82f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

} // namespace hub_ui

// Locate a development editor without baking one operating system's filename
// or one launch directory into the Hub. GWS_DEV_EDITOR is the explicit escape
// hatch; the remaining candidates cover packaged siblings and the usual
// side-by-side WorldShaper-Hub/c-Engine-Game source checkout.
static std::string find_dev_editor() {
    std::vector<fs::path> candidates;
    if (const char* configured = std::getenv("GWS_DEV_EDITOR"))
        candidates.emplace_back(configured);

    const fs::path hub_exe = this_executable_path();
    const fs::path bin_dir = hub_exe.parent_path();
#ifdef _WIN32
    candidates.push_back(bin_dir / "editor.exe");
    candidates.push_back(bin_dir / "GameWorldshaperEditor.exe");
#else
    candidates.push_back(bin_dir / "editor");
    // <workspace>/WorldShaper-Hub/build-linux/bin/GameWorldshaperHub
    // <workspace>/c-Engine-Game/build/linux-{debug,release}/bin/editor
    const fs::path hub_root = bin_dir.parent_path().parent_path();
    const fs::path workspace = hub_root.parent_path();
    candidates.push_back(workspace / "c-Engine-Game" / "build" / "linux-debug" / "bin" / "editor");
    candidates.push_back(workspace / "c-Engine-Game" / "build" / "linux-release" / "bin" / "editor");
#endif

    std::error_code ec;
    for (const fs::path& candidate : candidates) {
        if (!candidate.empty() && fs::is_regular_file(candidate, ec))
            return fs::absolute(candidate, ec).string();
        ec.clear();
    }
    return {};
}

// ---- Hub uninstall ---------------------------------------------------------
// %APPDATA%/GameWorldshaper — where the Hub persists repo/token/recent-projects.
static fs::path hub_config_dir() {
#ifdef _WIN32
    if (const char* ad = std::getenv("APPDATA")) return fs::path(ad) / "GameWorldshaper";
    if (const char* hp = std::getenv("USERPROFILE")) return fs::path(hp) / ".gameworldshaper";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) return fs::path(xdg) / "gameworldshaper";
    if (const char* hp = std::getenv("HOME")) return fs::path(hp) / ".config" / "gameworldshaper";
#endif
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

// Uninstall the Hub: run the NSIS uninstaller (installed) or self-delete
// (portable), then remove its data. On success the caller closes the window so
// the exe unlocks and the deletion can complete.
static bool uninstall_hub(std::string& msg, bool remove_engines) {
    const fs::path un = find_uninstaller();
    if (!un.empty()) {
        // The NSIS uninstaller is manifested requireAdministrator (the Hub installs
        // to Program Files), so it must be launched via ShellExecute with "runas"
        // to trigger UAC elevation. CreateProcess can't elevate — it would fail
        // with ERROR_ELEVATION_REQUIRED, which is why the button did nothing.
        const std::string exe = un.string();
        const std::string dir = un.parent_path().string();
        HINSTANCE h = ShellExecuteA(nullptr, "runas", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(h) <= 32) {
            msg = "Uninstaller was not started (elevation cancelled). Nothing was removed.";
            return false;   // user declined UAC -> leave everything intact
        }
        remove_hub_data(remove_engines);   // user-writable dirs; only after elevation succeeded
        return true;
    }
    // Portable (no installer): self-delete the files in place.
    remove_hub_data(remove_engines);
    std::string err;
    if (!spawn_portable_selfdelete(err)) { msg = err; return false; }
    return true;
}
#else
// Linux packages do not use the Hub's Windows/NSIS uninstaller. Returning an
// empty path keeps the shared confirmation UI platform-neutral: it displays
// the manual-removal behaviour implemented by uninstall_hub() below.
static fs::path find_uninstaller() {
    return {};
}

static bool uninstall_hub(std::string& msg, bool remove_engines) {
    remove_hub_data(remove_engines);
    msg = "Removed Hub data. Delete the Hub folder manually on this platform.";
    return true;
}
#endif

int main() {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) return 1;
    GLFWwindow* win = glfwCreateWindow(1180, 760, "World Shaper Hub", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwSetWindowSizeLimits(win, 940, 620, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // the hub doesn't persist window layout
    ImGui::StyleColorsDark();
    hub_ui::apply_theme(win);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL2_Init();

    ProjectsRegistry projects; projects.load();
    EngineRegistry   engines;
    const std::string dev_editor = find_dev_editor();
    engines.scan(dev_editor);

    // New-project form state.
    char       new_name[128] = "MyGame";
    char       new_loc[512]  = {0};
    { std::string d = default_projects_dir(); std::snprintf(new_loc, sizeof(new_loc), "%s", d.c_str()); }
    FeatureSet new_features = FeatureSet::defaults();
    int        new_engine_idx = 0;
    int        sel_project = -1;
    int        active_page = 0;  // Projects, New Project, Engines, Settings
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
        // A project opens ONLY in its bound engine version — no silent fallback to
        // the newest installed one (a scene authored for one version can break in
        // another). If it isn't installed, tell the user to install it or change
        // the project's engine version (both available in the UI).
        const EngineVersion* ev = engines.find(pm.engine_version);
        if (!ev) {
            status = "Project '" + pm.name + "' needs engine version " + pm.engine_version +
                     ", which is not installed. Install it in Engine Versions, or change the "
                     "project's engine version below.";
            return;
        }
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

        size_t installed_count = 0;
        for (const auto& version : engines.versions()) if (!version.is_dev) ++installed_count;

        // Brand header and live workspace summary. This replaces the oversized
        // title + tab strip with a stable application shell: navigation stays in
        // one place while counts remain visible on every page.
        ImGui::BeginChild("hub_header", ImVec2(0, 72.0f), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleColor(ImGuiCol_Text, hub_ui::kAccentHot);
        ImGui::SetWindowFontScale(1.55f);
        ImGui::TextUnformatted("WORLD SHAPER");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Project and engine workspace");

        const std::string summary = std::to_string(projects.items().size()) + " projects   |   " +
                                    std::to_string(installed_count) + " engines   |   Hub " +
                                    hub_version();
        const float summary_x = ImGui::GetWindowWidth() -
                                ImGui::CalcTextSize(summary.c_str()).x -
                                ImGui::GetStyle().WindowPadding.x;
        ImGui::SetCursorPos(ImVec2(std::max(ImGui::GetCursorPosX(), summary_x), 27.0f));
        ImGui::TextColored(hub_ui::kMuted, "%s", summary.c_str());
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0, 2));

        constexpr float footer_height = 48.0f;
        ImGui::BeginChild("hub_navigation", ImVec2(190.0f, -footer_height), true);
        ImGui::TextDisabled("WORKSPACE");
        ImGui::Dummy(ImVec2(0, 4));
        if (hub_ui::nav_item("Projects",        active_page == 0)) active_page = 0;
        if (hub_ui::nav_item("New Project",     active_page == 1)) active_page = 1;
        if (hub_ui::nav_item("Engine Versions", active_page == 2)) active_page = 2;
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));
        if (hub_ui::nav_item("Settings",        active_page == 3)) active_page = 3;
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("hub_content", ImVec2(0, -footer_height), true);
            // ---------------- Projects ----------------
            if (active_page == 0) {
                hub_ui::page_header("Projects", "Open a recent project or adjust the engine and modules it uses.");
                const float proj_list_h = ImGui::GetContentRegionAvail().y * 0.40f;
                ImGui::BeginChild("projlist", ImVec2(0, proj_list_h), true);
                const auto& items = projects.items();
                if (items.empty()) {
                    ImGui::Dummy(ImVec2(0, 18));
                    const char* empty_title = "No projects in this workspace";
                    const float center_x = (ImGui::GetContentRegionAvail().x -
                                            ImGui::CalcTextSize(empty_title).x) * 0.5f;
                    if (center_x > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + center_x);
                    ImGui::TextUnformatted(empty_title);
                    ImGui::TextDisabled("Create your first project or add an existing folder below.");
                }
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
                        const bool have = engines.find(pm.engine_version) != nullptr;
                        if (have) ImGui::TextDisabled("   engine %s", pm.engine_version.c_str());
                        else      ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
                                                     "   engine %s (not installed)", pm.engine_version.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();

                const bool can = sel_project >= 0 && sel_project < (int)items.size() &&
                                 fs::exists(items[sel_project].manifest_path);
                ImGui::BeginDisabled(!can);
                if (hub_ui::primary_button("Open", ImVec2(110, 0)) && can)
                    open_project(items[sel_project].manifest_path);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Remove from list", ImVec2(150, 0)) &&
                    sel_project >= 0 && sel_project < (int)items.size()) {
                    // remove() takes the path BY VALUE, so passing a reference into the
                    // vector it erases is safe (this used to crash the whole Hub).
                    projects.remove(items[sel_project].manifest_path); projects.save(); sel_project = -1;
                }
                ImGui::SameLine();
                if (ImGui::Button("Add Existing Project...", ImVec2(200, 0))) {
                    std::string folder = browse_folder("Choose an existing project folder");
                    if (!folder.empty()) {
                        // Seed a fresh manifest with the newest installed engine
                        // version (else the newest dev build, else the default).
                        std::string ver;
                        for (const auto& v : engines.versions()) if (!v.is_dev) { ver = v.version; break; }
                        if (ver.empty() && !engines.versions().empty()) ver = engines.versions().front().version;

                        std::string mpath, name;
                        if (import_existing_project(folder, ver, mpath, name)) {
                            // Keep display names unique. Only rename when the name
                            // collides with a DIFFERENT project already in the list.
                            bool collides = false;
                            for (const auto& it2 : projects.items())
                                if (it2.name == name && it2.manifest_path != mpath) { collides = true; break; }
                            if (collides) {
                                std::string uniq = projects.unique_name(name);
                                ProjectManifest pm;
                                if (ProjectManifest::load(mpath, pm)) { pm.name = uniq; ProjectManifest::save(mpath, pm); }
                                name = uniq;
                            }
                            projects.add(RecentProject{name, mpath}); projects.save();
                            sel_project = 0;
                            status = "Added project '" + name + "'. Any missing folders were created.";
                        } else {
                            status = "Could not add that folder as a project.";
                        }
                    }
                }

                // Per-project settings live in their OWN scrollable region so the
                // engine-version + modules controls are never clipped off the
                // bottom of the fixed-size window.
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::BeginChild("projdetail",
                                  ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), true);
                if (!can)
                    ImGui::TextDisabled("Select a project above to change its engine version or toggle its modules.");

                // Selected project: bound engine version + a control to change it.
                if (can) {
                    const std::string& mpath = items[sel_project].manifest_path;
                    ProjectManifest pm;
                    if (ProjectManifest::load(mpath, pm)) {
                        const bool have = engines.find(pm.engine_version) != nullptr;
                        ImGui::Dummy(ImVec2(0, 6));
                        ImGui::Separator();
                        ImGui::Text("Bound engine version: %s", pm.engine_version.c_str());
                        ImGui::SameLine();
                        if (have) ImGui::TextDisabled("(installed)");
                        else      ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
                                                     "(NOT installed — install it, or change it below)");
                        ImGui::SetNextItemWidth(220);
                        if (ImGui::BeginCombo("Change engine version", pm.engine_version.c_str())) {
                            for (const auto& v : engines.versions()) {
                                const bool seld = (v.version == pm.engine_version);
                                std::string lbl = v.version + (v.is_dev ? "  (dev)" : "");
                                if (ImGui::Selectable(lbl.c_str(), seld) && !seld) {
                                    pm.engine_version = v.version;
                                    if (ProjectManifest::save(mpath, pm))
                                        status = "Set '" + pm.name + "' to engine " + v.version + ".";
                                    else
                                        status = "Could not update the project file.";
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::TextDisabled("Only installed versions are listed. A project opens only in its bound version.");

                        // Modules: toggle which engine systems this project uses.
                        // Written to project.schizo (runtime config) — no rebuild.
                        ImGui::Dummy(ImVec2(0, 6));
                        ImGui::Separator();
                        ImGui::Text("Modules");
                        ImGui::TextDisabled("Toggle engine systems this project uses (saved to project.schizo; no rebuild needed).");
                        const auto before_mask = pm.features.raw();
                        draw_feature_checklist(pm.features);
                        if (pm.features.raw() != before_mask) {
                            if (ProjectManifest::save(mpath, pm))
                                status = "Updated modules for '" + pm.name + "'.";
                            else
                                status = "Could not update the project file.";
                        }
                    }
                }
                ImGui::EndChild();   // projdetail
            }

            // ---------------- New Project ----------------
            if (active_page == 1) {
                hub_ui::page_header("New Project", "Create a clean workspace with the engine features you need.");
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
                if (hub_ui::primary_button("Create & Open", ImVec2(150, 0)) && can) {
                    if (projects.name_exists(new_name)) {
                        status = "A project named '" + std::string(new_name) +
                                 "' already exists — choose a unique name.";
                    } else {
                        std::string ver = evs[std::min(new_engine_idx, (int)evs.size()-1)].version;
                        std::string mpath;
                        if (create_project(new_loc, new_name, new_features, mpath, ver)) open_project(mpath);
                        else status = "Could not create the project (folder exists/not empty, or path not writable?).";
                    }
                }
                ImGui::EndDisabled();
            }

            // ---------------- Engine Versions ----------------
            if (active_page == 2) {
                hub_ui::page_header("Engine Versions", "Install, update and manage the editors available to your projects.");
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

                // --- Remote updates FIRST (the recommended path) ---
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
                if (hub_ui::primary_button("Check for Updates")) {
                    set_github_repo(repo_buf);
                    set_github_token(token_buf);
                    std::string spec = repo_buf, e;
                    size_t slash = spec.find('/');
                    if (slash == std::string::npos || slash == 0 || slash + 1 >= spec.size()) {
                        status = "Enter the repo as owner/repo.";
                    } else if (fetch_github_releases(spec.substr(0, slash), spec.substr(slash + 1),
                                                     remote_versions, &e)) {
                        // Newest first, by NUMBER. GitHub returns releases in
                        // its own order and the list used to be shown as-is;
                        // sorting here means the version a person is most
                        // likely to want is the one at the top, and that
                        // "0.6.10" ranks above "0.6.9" rather than below it.
                        std::sort(remote_versions.begin(), remote_versions.end(),
                                  [](const RemoteVersion& a, const RemoteVersion& b) {
                                      return version_is_newer(a.version, b.version);
                                  });
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
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.47f, 0.57f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.62f, 0.73f, 1.0f));
                        if (ImGui::SmallButton("Download & Install")) {
                            std::string e;
                            status = "Downloading " + rv.version + "...";
                            if (download_and_install(rv, &e, nullptr, github_download_headers())) {
                                status = "Installed " + rv.version + ".";
                                engines.scan(dev_editor);
                            } else status = "Update failed: " + e;
                        }
                        ImGui::PopStyleColor(2);
                        ImGui::PopID();
                    }
                }

                // --- Manual install SECOND (advanced / offline path) ---
                ImGui::Dummy(ImVec2(0, 10));
                ImGui::Separator();
                ImGui::TextUnformatted("Install / Update a version manually");
                ImGui::TextDisabled("Advanced / offline: point to an engine folder (contains editor.exe) — e.g. an "
                                    "unpacked package's Engines/<version> dir. Same version name = update.");
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
            }

            // ---------------- Settings / Maintenance ----------------
            if (active_page == 3) {
                hub_ui::page_header("Settings", "Hub updates, storage locations and maintenance.");
                // ---- Hub self-update ----
                ImGui::TextUnformatted("Hub");
                ImGui::BulletText("This Hub: version %s   (%s)", hub_version().c_str(), hub_repo().c_str());
                if (hub_ui::primary_button("Check for Hub Updates")) {
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
            }
        ImGui::EndChild(); // hub_content

        // A launched Hub uninstall closes the app so the exe unlocks and the
        // uninstaller/self-delete helper can finish removing it.
        if (should_close) glfwSetWindowShouldClose(win, 1);

        ImGui::BeginChild("hub_status", ImVec2(0, 0), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const bool status_error = status.find("Failed") != std::string::npos ||
                                  status.find("failed") != std::string::npos ||
                                  status.find("error")  != std::string::npos ||
                                  status.find("Error")  != std::string::npos;
        ImGui::TextColored(status_error ? hub_ui::kDanger : hub_ui::kAccent, "STATUS");
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextWrapped("%s", status.empty() ? "Ready" : status.c_str());
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        int fbw, fbh; glfwGetFramebufferSize(win, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.030f, 0.041f, 0.058f, 1.0f);
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
