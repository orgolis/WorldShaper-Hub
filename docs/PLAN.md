# GameWorldshaper Hub — plan of record

**Created:** 2026-08-09 · **Current version:** 0.1.7 · **Repo:** `orgolis/WorldShaper-Hub`

> Until now the Hub had no planning document — only a README describing what it does today. It ships fast and
> its CI is green, but nothing recorded where it was going. This is that document.
>
> The Hub is deliberately **excluded from the engine's 16-stage roadmap**: it is the stable outer shell that
> installs and launches *many* engine versions, so it must not be tied to any one of them. It is not, however,
> excluded from the product plan — see the engine repo's
> [`PRODUCT_AND_ECOSYSTEM.md`](https://github.com/orgolis/c-Engine-Game/blob/main/docs/EngineMasterPlan/PRODUCT_AND_ECOSYSTEM.md)
> and [`WORKFLOW_PLAN.md`](https://github.com/orgolis/c-Engine-Game/blob/main/docs/EngineMasterPlan/WORKFLOW_PLAN.md),
> whose phase milestones this repo mirrors.

---

## 1. What the Hub is

A standalone launcher, in the shape of Unity Hub: it manages **projects** and **engine versions**, and it is the
only thing a user installs directly. Everything else arrives through it.

**Non-negotiable properties**

- **Version-independent.** The Hub outlives any engine version and must run every one it can install.
- **A project is bound to an engine version.** Opening a project launches *that* version's `editor.exe`, never
  "the latest". Changing the binding is an explicit, visible action.
- **Projects are sandboxed.** `project_paths` chdirs into the project so an engine cannot scribble outside it.
- **Self-contained.** Ships its own runtime DLLs; installing the Hub requires nothing else preinstalled.

## 2. What the Hub is not

Recorded so these are not re-litigated:

- **Not an engine build system.** It invokes the engine's CLI; it does not know how to compile anything.
- **Not a marketplace.** Payments, curation, tax and disputes are a business, not a feature.
- **Not a backend.** Accounts, matchmaking and cloud saves are integrated from a provider if ever, never operated.
- **Not a second editor.** No content authoring belongs here.

## 3. Where it stands (2026-08-09)

| Capability | State |
|---|---|
| Install / uninstall versioned engine builds from GitHub Releases | ✅ token-free against the public repo |
| Install history (present vs previously-installed), forget | ✅ |
| Create / open / import projects, unique naming, native folder picker | ✅ |
| Project bound to an exact engine version, with a change-version UI | ✅ |
| Per-project module toggles (`project.schizo`) | ✅ |
| Self-update, with elevation when installed under Program Files | ✅ |
| Uninstall the Hub, optionally keeping engine versions | ✅ |
| GUI subsystem — no console window | ✅ |
| CI (`release-hub.yml`) | ✅ green |
| **Plan of record** | ✅ this document |

**Known polish backlog** — [#3](https://github.com/orgolis/WorldShaper-Hub/issues/3): threaded downloads (the UI
blocks during an install), a per-user install that needs no administrator rights, and Release-build size.

**Hard-won detail worth not rediscovering — release assets are part of the install path.** The Hub picks a
release asset by *substring match* and takes the **first hit in GitHub's name-sorted list**. When engine v0.2.0
added a second asset (`engine-v0.2.0-win64-symbols.zip`), it sorted *ahead of* `engine-v0.2.0-win64.zip`, so every
Hub already installed would have downloaded 63 MB of debug binaries and installed them as the engine. Two rules
follow:

1. **Adding an asset to an engine release is a change to the Hub's install path.** Treat it as such.
2. **Hubs already in the wild can never be patched.** Any fix must work for the version users already have — which
   is why the symbols archive became `.tar.gz` (matching no `.zip` filter by construction) rather than relying on
   the Hub's matcher being tightened. The tightened default (`-win64.zip`) is the second line of defence only.

`hub_selftest <api_base> <owner> <repo>` exercises the real feed end-to-end (download, extract, install, verify
`editor.exe`). **It should run in this repo's CI against the live feed**, which would have caught the above before
the release rather than after.

**Hard-won detail worth not rediscovering:** the Hub installs to Program Files, so both the NSIS uninstaller and
the self-update file-swap need elevation. Launching them with `CreateProcess` fails with
`ERROR_ELEVATION_REQUIRED`; they must go through `ShellExecute` with `runas`, and user data must only be removed
*after* UAC is accepted.

## 4. Where it is going

Ordered by the shared phase milestones. The through-line: **turn the Hub from a launcher into the place a
developer starts their day.**

### Phase 1 — [#2](https://github.com/orgolis/WorldShaper-Hub/issues/2) Projects born correctly
New projects get a Git repository with **LFS configured from the first commit**, plus correct `.gitattributes`
and `.gitignore`. Configuring LFS late is the single mistake that ruins game repositories, and the Hub is the
only place with the leverage to make it impossible. A working reference already exists: `WorldShaper-Samples`
was bootstrapped exactly this way.

### Phase 5 — [#4](https://github.com/orgolis/WorldShaper-Hub/issues/4) The operations console
- **[#5](https://github.com/orgolis/WorldShaper-Hub/issues/5) Upgrade preflight — the feature that sells the Hub.**
  Engine upgrades are frightening because breakage is found *after* committing to them. Install the candidate
  version alongside, open the project against it in a sandbox, run the engine's check suite and cook, and report
  what breaks **before** the developer switches — with one-click rollback. **No engine offers this.** It is
  available to us only because versioned side-by-side installs (here) and 20 headless check binaries (engine)
  both already exist. Depends on the engine CLI.
- **[#6](https://github.com/orgolis/WorldShaper-Hub/issues/6) Project health** — build status, test results,
  crash reports, cook cache size, branch and days since last commit.

### Phase 6 — [#7](https://github.com/orgolis/WorldShaper-Hub/issues/7) Templates
Surface the eight genre starter templates from `WorldShaper-Samples`, with the correct module set pre-selected.
The design work is already done in the engine's feature catalog.

### Phase 7 — [#8](https://github.com/orgolis/WorldShaper-Hub/issues/8) The Ship tab
The lifecycle currently has no ending: a developer can build a game and then has nowhere to go.
[#9](https://github.com/orgolis/WorldShaper-Hub/issues/9) build targets, icons, version strings and signing;
[#10](https://github.com/orgolis/WorldShaper-Hub/issues/10) Steam depot upload, achievements, cloud saves and
Workshop.

### Phase 8 — [#11](https://github.com/orgolis/WorldShaper-Hub/issues/11) Telemetry and remote config
Integrate a provider; do not operate one.

## 5. Release policy

**Releases are deliberate, not automatic.** Do not cut a release as a routine follow-up to finishing work —
commit and push, and release when there is a reason to. The rapid 0.1.x cadence made the version number
meaningless. **The next release is 0.2.0**, not 0.1.8.

Mechanically: bump `project(... VERSION x.y.z)`, tag `vX.Y.Z`, and `release-hub.yml` builds the installer and ZIP
and attaches them to the Release. The Hub's own self-update reads those Releases.

## 6. How this repo is tracked

Issues here use the **same ten phase milestones and eleven labels** as the engine and samples repositories, so a
single view spans all three. Sequencing lives in the engine's `WORKFLOW_PLAN.md`; this document explains the
*why* for the Hub specifically.
