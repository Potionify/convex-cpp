# Bumps the library version (include/convex/version.h and the README's
# FetchContent pin), commits, and creates the matching annotated tag. Run from
# anywhere inside the repo:
#
#   cmake -DBUMP=patch -P scripts/bump_version.cmake    # 0.1.0 -> 0.1.1
#   cmake -DBUMP=minor -P scripts/bump_version.cmake    # 0.1.0 -> 0.2.0
#   cmake -DBUMP=major -P scripts/bump_version.cmake    # 0.1.0 -> 1.0.0
#   cmake -DBUMP=1.2.3 -P scripts/bump_version.cmake    # explicit version
#
# PowerShell splits unquoted dotted args — quote them: cmake "-DBUMP=1.2.3" ...
# Or use the wrappers (no quoting needed): scripts/bump_version.bat patch
# on Windows, scripts/bump_version.sh patch on Linux/macOS.
#
# Publishing. The release workflow triggers on the tag, and git does not push
# tags by default — neither does a plain `git push` nor the Push button in a
# GUI client. A tag left behind that way looks like a release that silently
# never happened. So this script sets push.followTags in the repo's local
# config, which makes every subsequent push carry annotated tags reachable
# from the commits it sends. Publish with either:
#
#   cmake -DBUMP=patch -DPUSH=ON -P scripts/bump_version.cmake   # bump + push
#   git push origin main                                        # after a bump
#
# Both send the tag. `git push origin main --tags` still works and is what to
# use if an earlier tag was stranded locally.

if(NOT DEFINED BUMP)
    message(FATAL_ERROR "Pass -DBUMP=major|minor|patch|x.y.z (the -D must come before -P)")
endif()

get_filename_component(repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(version_h "${repo_root}/include/convex/version.h")

# Refuse to mix the release commit with unrelated changes.
execute_process(COMMAND git -C "${repo_root}" status --porcelain
                OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
if(NOT dirty STREQUAL "")
    message(FATAL_ERROR "Working tree is not clean; commit or stash first:\n${dirty}")
endif()

file(READ "${version_h}" content)
foreach(part MAJOR MINOR PATCH)
    if(NOT content MATCHES "#define CONVEX_VERSION_${part} ([0-9]+)")
        message(FATAL_ERROR "CONVEX_VERSION_${part} not found in ${version_h}")
    endif()
    set(${part} "${CMAKE_MATCH_1}")
endforeach()
set(old_version "${MAJOR}.${MINOR}.${PATCH}")

if(BUMP STREQUAL "major")
    math(EXPR MAJOR "${MAJOR} + 1")
    set(MINOR 0)
    set(PATCH 0)
elseif(BUMP STREQUAL "minor")
    math(EXPR MINOR "${MINOR} + 1")
    set(PATCH 0)
elseif(BUMP STREQUAL "patch")
    math(EXPR PATCH "${PATCH} + 1")
elseif(BUMP MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    set(MAJOR "${CMAKE_MATCH_1}")
    set(MINOR "${CMAKE_MATCH_2}")
    set(PATCH "${CMAKE_MATCH_3}")
else()
    message(FATAL_ERROR "BUMP must be major, minor, patch, or x.y.z (got '${BUMP}')")
endif()
set(new_version "${MAJOR}.${MINOR}.${PATCH}")

foreach(part MAJOR MINOR PATCH)
    string(REGEX REPLACE "#define CONVEX_VERSION_${part} [0-9]+"
           "#define CONVEX_VERSION_${part} ${${part}}" content "${content}")
endforeach()
file(WRITE "${version_h}" "${content}")

# The README's FetchContent snippet pins a tag. Left to a human it goes stale
# silently, and a stale pin hands every new user an old library.
set(readme "${repo_root}/README.md")
file(READ "${readme}" readme_content)
if(NOT readme_content MATCHES "GIT_TAG v[0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "No 'GIT_TAG vX.Y.Z' pin found in ${readme}; update it by hand "
                        "or fix this script's pattern")
endif()
string(REGEX REPLACE "GIT_TAG v[0-9]+\\.[0-9]+\\.[0-9]+" "GIT_TAG v${new_version}"
       readme_content "${readme_content}")
file(WRITE "${readme}" "${readme_content}")

# Make an ordinary push carry the tag. Without this the release workflow waits
# on a tag that never arrives, and the failure is silent.
execute_process(COMMAND git -C "${repo_root}" config --local push.followTags true
                COMMAND_ERROR_IS_FATAL ANY)

execute_process(COMMAND git -C "${repo_root}" add include/convex/version.h README.md
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git -C "${repo_root}" commit -m "Release v${new_version}"
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git -C "${repo_root}" tag -a "v${new_version}" -m "v${new_version}"
                COMMAND_ERROR_IS_FATAL ANY)

message(STATUS "${old_version} -> ${new_version}, committed and tagged v${new_version}")

if(PUSH)
    execute_process(COMMAND git -C "${repo_root}" push origin HEAD "v${new_version}"
                    COMMAND_ERROR_IS_FATAL ANY)
    message(STATUS "Pushed the release commit and tag v${new_version}")
else()
    message(STATUS "Publish with: git push origin main   (push.followTags carries the tag)")
endif()
