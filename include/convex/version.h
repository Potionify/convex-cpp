#pragma once

// Single source of truth for the library version. CMakeLists.txt parses the
// numeric macros for project(VERSION ...), the default client_options::
// client_id embeds CONVEX_VERSION, and the release workflow checks the git
// tag against these values. Bump only here when cutting a release.

#define CONVEX_VERSION_MAJOR 0
#define CONVEX_VERSION_MINOR 1
#define CONVEX_VERSION_PATCH 2

#define CONVEX_DETAIL_VERSION_STR(x) #x
#define CONVEX_DETAIL_VERSION_STR2(x) CONVEX_DETAIL_VERSION_STR(x)

/// The version as a "major.minor.patch" string literal.
#define CONVEX_VERSION                            \
    CONVEX_DETAIL_VERSION_STR2(CONVEX_VERSION_MAJOR) \
    "." CONVEX_DETAIL_VERSION_STR2(CONVEX_VERSION_MINOR) \
    "." CONVEX_DETAIL_VERSION_STR2(CONVEX_VERSION_PATCH)
