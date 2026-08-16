#pragma once
// ============================================================================
// Dotted-version comparison, in ONE place.
//
// WHY THIS FILE EXISTS. The Hub had two ways of deciding which version was
// newer. Its self-update path parsed the dotted numbers and compared them
// component by component, which is correct. Its ENGINE path compared the
// version strings with `operator>`, which is correct only by coincidence:
//
//     "0.6.10" > "0.6.9"   ->   false        ('1' sorts before '9')
//
// Every engine release from 0.6.0 to 0.6.9 had a single-digit patch number, so
// lexicographic order happened to match numeric order and the bug was invisible
// for fifty releases. v0.6.10 was the first two-digit patch, and the Hub
// immediately decided the newly installed engine was older than the one it
// replaced — so it kept launching 0.6.9 and would not treat 0.6.10 as an update.
//
// The old call site was even labelled "lexicographic-ish", which is the sort of
// comment that marks a known approximation nobody expected to be load-bearing.
//
// Both paths now call the same function, so they cannot disagree again.
// ============================================================================

#include <string>
#include <vector>

namespace schizo::project {

/// Numeric leading components of a dotted version; any trailing pre-release
/// suffix is ignored. "1.2.3-rc1" -> [1,2,3]. "" -> [].
inline std::vector<long> version_parts(const std::string& v) {
    std::vector<long> parts;
    size_t i = 0;
    while (i < v.size()) {
        long n = 0;
        bool any = false;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
            n = n * 10 + (v[i] - '0');
            ++i;
            any = true;
        }
        if (any) parts.push_back(n); else break;
        if (i < v.size() && v[i] == '.') ++i; else break;
    }
    return parts;
}

/// True when `candidate` is strictly newer than `current`.
///
/// Missing components count as 0, so "1.2" and "1.2.0" compare equal — a
/// release tagged either way must not look like an update to itself.
inline bool version_is_newer(const std::string& candidate, const std::string& current) {
    const std::vector<long> a = version_parts(candidate);
    const std::vector<long> b = version_parts(current);
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const long ca = i < a.size() ? a[i] : 0;
        const long cb = i < b.size() ? b[i] : 0;
        if (ca != cb) return ca > cb;
    }
    return false;   // equal
}

}  // namespace schizo::project
