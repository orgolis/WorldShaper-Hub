# GameWorldshaper Hub

A standalone launcher for the GameWorldshaper engine — like Unity Hub. It manages
**projects** and **engine versions**: create/open projects (each bound to a
specific engine version), install engine versions locally, and pull new versions
straight from a GitHub repository's **Releases**.

It is a **separate program and repository** from the engine on purpose: the Hub
is the stable outer shell that installs and launches *many* engine versions, so
it must not be tied to any single one.

---

## Build

Requires CMake ≥ 3.20 and a C++20 compiler. On Windows the pinned toolchain is
Ninja + MinGW g++ (Strawberry Perl's toolchain). Dependencies (GLFW, Dear ImGui,
spdlog, nlohmann/json) are fetched automatically via CMake **FetchContent** — no
submodules, no vendored source.

```sh
cmake --preset windows          # first configure downloads the deps
cmake --build --preset windows
# -> build/bin/GameWorldshaperHub.exe  (+ the 3 MinGW runtime DLLs beside it)
```

Package an installer (ZIP always; a Windows `.exe` installer when NSIS/`makensis`
is on PATH):

```sh
cd build && cpack
```

---

## How it works

- **Projects** live in a folder with a `project.schizo` manifest (name, enabled
  features, default scene, and the **bound engine version**). Opening a project
  launches that engine's `editor.exe --project <path>`.
- **Engine versions** are self-contained editor folders (editor.exe + runtime
  DLLs + default content). The Hub discovers them under
  `%LOCALAPPDATA%\GameWorldshaper\Engines\<version>\` and next to the Hub exe,
  and registers a sibling `editor.exe` as the `dev` version.
- **Remote updates** pull from a GitHub repo's Releases: set `owner/repo` in the
  Engine Versions tab, click **Check for Updates**, and the Hub reads the repo's
  releases, then downloads + extracts + installs the engine `.zip` asset from any
  release you pick.

---

## Distribution model (two repos)

```
  ┌─────────────────────────┐        publishes releases        ┌────────────────────┐
  │  Engine repo             │  ── engine-<ver>-win64.zip ───▶  │  GitHub Releases   │
  │  (editor + engine)       │     (built by CI on each tag)    └─────────┬──────────┘
  └─────────────────────────┘                                             │
                                                                          │ GitHub Releases API
                                                                          ▼
  ┌─────────────────────────┐   Check for Updates → download → install  ┌────────────────────┐
  │  Hub repo (this one)     │  ◀───────────────────────────────────────  │  This Hub app      │
  └─────────────────────────┘                                            └────────────────────┘
```

- The **engine** repo builds an engine package on each release and uploads it as
  a release asset (see its `.github/workflows/release-engine.yml`).
- The **Hub** (this repo) queries that repo's Releases API and installs the
  package the user chooses.

---

## Setup / handoff checklist

These require a GitHub account and can't be done from a local build:

1. **Create two GitHub repos** — one for the engine, one for this Hub. Push each.
2. **Engine repo:** ensure `.github/workflows/release-engine.yml` is present (it
   builds → CPacks an engine-only `.zip` → uploads it to the Release). Cut a
   release by pushing a tag like `v0.2.0`.
3. **In the Hub:** open the *Engine Versions* tab, set **owner/repo** to your
   engine repo (e.g. `your-org/GameWorldshaper-Engine`), and **Check for Updates**.
   For a **private** engine repo, also paste a GitHub **personal-access-token**
   (read access) into the token field — the Hub then authenticates both the
   releases API and the asset download. Public repos need no token.
4. Optionally add CI to this repo to publish the Hub's own installer on release.

The `owner/repo` you enter is saved to
`%APPDATA%\GameWorldshaper\github_repo.txt`.
