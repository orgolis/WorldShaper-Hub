#pragma once
// ============================================================================
// Hub self-update. The Hub checks its OWN GitHub repo's Releases for a newer Hub
// build, downloads the packaged Hub .zip, and swaps itself in place. Because a
// running .exe can't overwrite itself, the swap is done by a detached helper
// process that waits for the Hub to exit, copies the new files over the install,
// and relaunches — the same technique the uninstaller uses.
// ============================================================================
#include "update_feed.h"   // RemoteVersion

#include <functional>
#include <string>

namespace schizo::project {

// This build's version (compile-time, from CMake PROJECT_VERSION).
std::string hub_version();

// The Hub's own "owner/repo" for self-update (persisted in the config dir);
// defaults to orgolis/WorldShaper-Hub.
std::string hub_repo();
void        set_hub_repo(const std::string& owner_slash_repo);

// Compare dotted-numeric versions: true if `candidate` is strictly newer than
// `current` (e.g. "0.2.0" vs "0.1.3" -> true). Non-numeric parts count as 0.
bool version_is_newer(const std::string& candidate, const std::string& current);

// Query the Hub repo's releases. On success returns true and, if a release newer
// than this build exists, sets `found=true` and fills `newer`; otherwise
// `found=false` (up to date). Returns false + `err` on a network/parse error.
bool check_hub_update(RemoteVersion& newer, bool& found, std::string* err = nullptr);

// Download + stage the new Hub package, then spawn a detached helper that waits
// for this process to exit, swaps the Hub files in place, and relaunches the Hub.
// On success the caller MUST close the window so the exe unlocks. Windows only.
bool apply_hub_update(const RemoteVersion& rv, std::string* err = nullptr,
                      const std::function<void(const std::string&)>& progress = nullptr);

} // namespace schizo::project
