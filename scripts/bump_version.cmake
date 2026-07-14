# Bumps the library version (include/convex/version.h), commits, and creates
# the matching annotated tag. Run from anywhere inside the repo:
#
#   cmake -DBUMP=patch -P scripts/bump_version.cmake    # 0.1.0 -> 0.1.1
#   cmake -DBUMP=minor -P scripts/bump_version.cmake    # 0.1.0 -> 0.2.0
#   cmake -DBUMP=major -P scripts/bump_version.cmake    # 0.1.0 -> 1.0.0
#   cmake -DBUMP=1.2.3 -P scripts/bump_version.cmake    # explicit version
#
# Then publish (this triggers the release workflow):
#
#   git push origin main --tags

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

execute_process(COMMAND git -C "${repo_root}" add include/convex/version.h
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git -C "${repo_root}" commit -m "Release v${new_version}"
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git -C "${repo_root}" tag -a "v${new_version}" -m "v${new_version}"
                COMMAND_ERROR_IS_FATAL ANY)

message(STATUS "${old_version} -> ${new_version}, committed and tagged v${new_version}")
message(STATUS "Publish with: git push origin main --tags")
