#pragma once
// ============================================================================
// Git + LFS scaffolding for new projects.
//
// WHY THIS EXISTS: Git LFS must be configured BEFORE the first commit. Add it
// later and every binary already committed stays in history forever — the
// repository is permanently bloated and the only real fix is a rewrite. It is
// the single most common way a game repository is ruined, it is entirely
// avoidable, and the Hub is the one place with the leverage to make it
// impossible: it creates the project, so it can create the repository too.
//
// Deliberately narrow. This is not a source-control UI (that is the editor's
// job, later) — it runs once, at creation, and then gets out of the way.
//
// Failure policy: scaffolding NEVER fails project creation. A developer without
// git installed still gets a working project; they just get a warning and the
// files sitting there ready for whenever they do run `git init`.
// ============================================================================
#include <string>

namespace schizo::project {

struct GitScaffoldResult {
    bool wrote_files   = false;  // .gitattributes / .gitignore written
    bool repo_created  = false;  // git init succeeded
    bool committed     = false;  // initial commit made
    bool lfs_available = false;  // git-lfs present and installed into the repo
    std::string message;         // human-readable summary, for the UI and logs
};

// Write .gitattributes (LFS tracking) and .gitignore into `project_dir`, then
// optionally `git init` + make the first commit.
//
// The LFS patterns cover what game projects actually store as binaries —
// textures, models, audio, video, cooked bundles — while the engine's own text
// formats (.scene, .logic, .gameplay, .items, .schizo) are pinned as text so
// they keep diffing and merging normally. That distinction is the whole point:
// LFS everything and you lose readable history on the files you most want it for.
GitScaffoldResult scaffold_git(const std::string& project_dir,
                               const std::string& project_name,
                               bool init_repo = true);

// True if a `git` executable is on PATH. Exposed so the UI can explain itself
// rather than silently skipping.
bool git_available();

} // namespace schizo::project
