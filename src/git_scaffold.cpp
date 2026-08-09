#include "git_scaffold.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace schizo::project {
namespace {

// Run a command in `dir`, discarding output, returning its exit code.
int run_in(const fs::path& dir, const std::string& args) {
    // NOTE on quoting: do NOT wrap this whole line in an extra quote pair.
    // cmd.exe strips the outermost pair only when the line *begins* with a
    // quote — this one begins with `cd`, so nothing is stripped, and adding a
    // wrapper instead unbalances the final quote of a quoted argument. That
    // silently truncated `commit -m "..."` and produced a repository with no
    // initial commit, which the self-test caught.
#ifdef _WIN32
    const std::string cmd =
        "cd /d \"" + dir.string() + "\" && git " + args + " >nul 2>&1";
#else
    const std::string cmd =
        "cd \"" + dir.string() + "\" && git " + args + " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str());
}

bool write_file(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << text;
    return true;
}

// Binary formats go to LFS. Engine text formats explicitly do NOT — they are
// the files whose history is worth reading.
const char* kGitAttributes = R"(# Git LFS — configured BEFORE the first commit.
#
# This is the important part: adding LFS later leaves every already-committed
# binary in history forever, and the repository stays bloated. Written by the
# GameWorldshaper Hub at project creation so it cannot be forgotten.

# Textures
*.png   filter=lfs diff=lfs merge=lfs -text
*.jpg   filter=lfs diff=lfs merge=lfs -text
*.jpeg  filter=lfs diff=lfs merge=lfs -text
*.tga   filter=lfs diff=lfs merge=lfs -text
*.bmp   filter=lfs diff=lfs merge=lfs -text
*.hdr   filter=lfs diff=lfs merge=lfs -text
*.exr   filter=lfs diff=lfs merge=lfs -text
*.ktx   filter=lfs diff=lfs merge=lfs -text
*.dds   filter=lfs diff=lfs merge=lfs -text
*.ctex  filter=lfs diff=lfs merge=lfs -text
*.vt    filter=lfs diff=lfs merge=lfs -text

# Models
*.fbx   filter=lfs diff=lfs merge=lfs -text
*.glb   filter=lfs diff=lfs merge=lfs -text
*.gltf  filter=lfs diff=lfs merge=lfs -text
*.obj   filter=lfs diff=lfs merge=lfs -text
*.usd   filter=lfs diff=lfs merge=lfs -text
*.usda  filter=lfs diff=lfs merge=lfs -text
*.usdc  filter=lfs diff=lfs merge=lfs -text
*.usdz  filter=lfs diff=lfs merge=lfs -text
*.blend filter=lfs diff=lfs merge=lfs -text

# Audio and video
*.wav   filter=lfs diff=lfs merge=lfs -text
*.ogg   filter=lfs diff=lfs merge=lfs -text
*.mp3   filter=lfs diff=lfs merge=lfs -text
*.flac  filter=lfs diff=lfs merge=lfs -text
*.mp4   filter=lfs diff=lfs merge=lfs -text

# Cooked bundles
*.pak   filter=lfs diff=lfs merge=lfs -text

# Engine text formats stay TEXT so they diff and merge normally. Do not move
# these into LFS — readable scene and gameplay history is worth more than the
# few kilobytes it costs.
*.scene    text
*.logic    text
*.gameplay text
*.items    text
*.holes    text
*.schizo   text
*.py       text
*.cs       text
*.cpp      text
*.h        text
)";

const char* kGitIgnore = R"(# Build output and caches
build/
build-*/
out/
bin/
lib/
.cache/
cooked/
script_cache/

# Editor and user state
editor.ini
imgui.ini
*.user

# Crash reports and logs
diagnostics/
*.log
*.dmp

# OS noise
Thumbs.db
.DS_Store
)";

} // namespace

bool git_available() {
#ifdef _WIN32
    return std::system("where git >nul 2>&1") == 0;
#else
    return std::system("command -v git >/dev/null 2>&1") == 0;
#endif
}

GitScaffoldResult scaffold_git(const std::string& project_dir,
                               const std::string& project_name,
                               bool init_repo) {
    GitScaffoldResult r;
    const fs::path root(project_dir);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        r.message = "project directory does not exist";
        return r;
    }

    // 1. The files. These are written even without git installed — they are
    //    plain text, they are correct, and they will do their job the moment
    //    anyone runs `git init` here.
    const bool a = write_file(root / ".gitattributes", kGitAttributes);
    const bool b = write_file(root / ".gitignore",     kGitIgnore);
    r.wrote_files = a && b;
    if (!r.wrote_files) {
        r.message = "could not write .gitattributes/.gitignore";
        spdlog::warn("[git] {} in {}", r.message, project_dir);
        return r;
    }

    if (!init_repo) {
        r.message = ".gitattributes and .gitignore written (repository not initialised)";
        return r;
    }
    if (!git_available()) {
        r.message = "git not found on PATH — LFS rules written, run `git init` when ready";
        spdlog::warn("[git] {}", r.message);
        return r;
    }
    if (fs::exists(root / ".git", ec)) {
        r.repo_created = true;   // already a repo; leave its history alone
        r.message = "existing repository left untouched; LFS rules written";
        return r;
    }

    // 2. The repository.
    if (run_in(root, "init -q") != 0) {
        r.message = "git init failed — LFS rules written, initialise manually";
        spdlog::warn("[git] {}", r.message);
        return r;
    }
    r.repo_created = true;

    // 3. LFS. Optional: absence must not break anything, because .gitattributes
    //    is already correct and LFS can be installed later without a history
    //    rewrite — provided the binaries have not been committed yet, which is
    //    exactly the window this function exists to protect.
    r.lfs_available = (run_in(root, "lfs install --local") == 0);
    if (!r.lfs_available)
        spdlog::warn("[git] git-lfs not installed; .gitattributes is in place for when it is");

    // 4. The first commit, so the LFS rules are in history before any asset is.
    run_in(root, "add -A");
    const std::string msg = "\"Create " + project_name + " (GameWorldshaper)\"";
    const bool committed =
        run_in(root, "-c user.email=hub@gameworldshaper.local -c user.name=\"GameWorldshaper Hub\" "
                     "commit -q -m " + msg) == 0;
    r.committed = committed;

    r.message = committed
        ? (r.lfs_available ? "git repository created with LFS, initial commit made"
                           : "git repository created and committed; install git-lfs to enable LFS")
        : "git repository created; initial commit did not run";
    spdlog::info("[git] {} ({})", r.message, project_dir);
    return r;
}

} // namespace schizo::project
