#pragma once
// ============================================================================
// GitHub Releases as the engine-version source. The Hub queries a repo's
// releases, and each release that carries an engine package (a .zip asset)
// becomes an installable RemoteVersion. Downloading/installing then reuses the
// same download_and_install() as the generic feed.
// ============================================================================
#include "update_feed.h"  // RemoteVersion, download_and_install

#include <string>
#include <vector>

namespace schizo::project {

// Query GitHub Releases for `owner/repo`. Each non-draft release with an asset
// whose filename contains `asset_match` yields a RemoteVersion (version = tag
// with any leading 'v' stripped, url = asset browser_download_url). `api_base`
// defaults to the public API; override it (e.g. a local mock) for testing.
bool fetch_github_releases(const std::string& owner, const std::string& repo,
                           std::vector<RemoteVersion>& out, std::string* err,
                           const std::string& asset_match = ".zip",
                           const std::string& api_base = "https://api.github.com");

// Persisted "owner/repo" the Hub pulls from (%APPDATA%/GameWorldshaper/github_repo.txt).
std::string github_repo();
void        set_github_repo(const std::string& owner_slash_repo);

// Optional GitHub personal-access-token (read access) for PRIVATE engine repos.
// Persisted at %APPDATA%/GameWorldshaper/github_token.txt. When set, the API is
// queried with Authorization, and release assets are pulled via the auth'd asset
// API. Empty token = public-repo mode (unauthenticated).
std::string github_token();
void        set_github_token(const std::string& token);

// Request headers for downloading a release asset (Authorization + the octet
// Accept the asset API needs) when a token is set; empty otherwise. Pass to
// download_and_install().
HttpHeaders github_download_headers();

} // namespace schizo::project
