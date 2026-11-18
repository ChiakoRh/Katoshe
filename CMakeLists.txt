cmake_minimum_required(VERSION 3.20)
project(Katoshe VERSION 0.1.0 LANGUAGES C)

# One C standard for the whole project. MSVC maps C99 to no extra flag, so this
# is also the quietest setting: CUI sees the value it would have picked itself.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Static CRT everywhere. The exes must run without the VC++ redistributable, and
# a /MD libcurl would fail to link against a /MT app (LNK2038).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# CUI source: the clone next to this project by default, override with -DCUI_DIR=<path>.
# Same pattern as CUI's own template; a submodule clone fought the network here and
# bought nothing on a single-machine setup.
set(CUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CUI" CACHE PATH "CUI source tree")
add_subdirectory("${CUI_DIR}" cui_build)

set(KATOSHE_CURL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl")

add_library(katoshe_core STATIC
    src/core/version.c
    src/core/paths.c
    src/core/config.c
    src/core/io.c
    src/core/match.c
    src/core/env.c
    src/core/state.c
    src/core/plan.c
    src/core/preset.c
    src/core/archive.c
    src/core/sha256.c
    src/core/download.c
    src/core/selftest.c
    src/core/gh.c
    src/core/secrets.c
    vendor/cjson/cJSON.c
)
target_include_directories(katoshe_core PUBLIC
    src/core
    vendor
    "${KATOSHE_CURL}/include"
)
target_compile_definitions(katoshe_core
    PUBLIC CURL_STATICLIB WIN32_LEAN_AND_MEAN NOMINMAX
    PRIVATE KATOSHE_VERSION="${PROJECT_VERSION}" _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(katoshe_core PUBLIC
    "${KATOSHE_CURL}/lib/libcurl.lib"
    ws2_32 crypt32 bcrypt advapi32 normaliz user32 secur32 iphlpapi
)

add_executable(katoshe-cli src/cli/main.c)
target_link_libraries(katoshe-cli PRIVATE katoshe_core)

add_executable(katoshe src/gui/main.c)
target_link_libraries(katoshe PRIVATE katoshe_core cui::cui)
cui_app_target(katoshe)   # windowed, no console, links only the components used

if (MSVC)
    foreach(t katoshe_core katoshe-cli katoshe)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${t} PRIVATE /W4 /utf-8)
    endforeach()
endif()
