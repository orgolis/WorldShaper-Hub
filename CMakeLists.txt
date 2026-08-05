cmake_minimum_required(VERSION 3.20)
project(GameWorldshaperHub VERSION 0.1.4 LANGUAGES CXX)

# ============================================================================
# GameWorldshaper Hub — standalone launcher. Independent of the engine source
# tree: all dependencies come via CMake FetchContent (pinned versions).
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The Hub's reported version. Defaults to the project() version, but the release
# CI overrides it from the git tag (-DGWS_HUB_VERSION=x.y.z) so cutting a release
# is just "tag vX.Y.Z" — no source edit. This drives both the compiled-in
# hub_version() (self-update comparison) and the package/asset name.
if(NOT DEFINED GWS_HUB_VERSION OR GWS_HUB_VERSION STREQUAL "")
    set(GWS_HUB_VERSION "${PROJECT_VERSION}")
endif()

if(MINGW)
    add_compile_options(-Wa,-mbig-obj)   # harmless; matches the engine toolchain
endif()

include(FetchContent)
FetchContent_Declare(glfw          GIT_REPOSITORY https://github.com/glfw/glfw.git      GIT_TAG 3.4      GIT_SHALLOW TRUE)
FetchContent_Declare(spdlog        GIT_REPOSITORY https://github.com/gabime/spdlog.git  GIT_TAG v1.14.1  GIT_SHALLOW TRUE)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git  GIT_TAG v3.11.3  GIT_SHALLOW TRUE)
FetchContent_Declare(imgui         GIT_REPOSITORY https://github.com/ocornut/imgui.git   GIT_TAG v1.90.9  GIT_SHALLOW TRUE)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw spdlog nlohmann_json imgui)   # imgui has no CMake — just populated

# ImGui is source-only (no CMake target); compile it into the Hub with the
# GLFW + legacy-GL2 backends.
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp)

# The launcher core (shared conceptually with the engine's editor via the
# project.schizo format contract; kept as this repo's own copy).
set(HUB_CORE
    src/project.cpp
    src/engine_registry.cpp
    src/http_client.cpp
    src/update_feed.cpp
    src/github_releases.cpp
    src/self_update.cpp)

add_executable(GameWorldshaperHub src/main.cpp ${HUB_CORE} ${IMGUI_SOURCES})
target_include_directories(GameWorldshaperHub PRIVATE
    src ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(GameWorldshaperHub PRIVATE HUB_VERSION="${GWS_HUB_VERSION}")
# GUI subsystem: no console/cmd window pops up when the Hub is launched. MinGW's
# runtime still calls main(), so no WinMain is needed.
if(WIN32)
    set_target_properties(GameWorldshaperHub PROPERTIES WIN32_EXECUTABLE ON)
endif()
target_link_libraries(GameWorldshaperHub PRIVATE
    glfw spdlog::spdlog nlohmann_json::nlohmann_json)
if(WIN32)
    # opengl32: imgui_impl_opengl2. shell32/ole32: folder picker. winhttp: releases API.
    target_link_libraries(GameWorldshaperHub PRIVATE opengl32 shell32 ole32 winhttp)
endif()

# Self-test (headless): GitHub-Releases-API path (fetch/parse/download/install/
# remove) against a mock URL passed as argv[1].
add_executable(hub_selftest tests/selftest.cpp ${HUB_CORE})
target_include_directories(hub_selftest PRIVATE src)
target_compile_definitions(hub_selftest PRIVATE HUB_VERSION="${GWS_HUB_VERSION}")
target_link_libraries(hub_selftest PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(hub_selftest PRIVATE winhttp shell32)  # shell32: self_update ShellExecute
endif()

# Bundle the MinGW runtime DLLs next to the exe so it runs on a clean box.
if(MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        add_custom_command(TARGET GameWorldshaperHub POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_mingw_bin}/${dll}" "$<TARGET_FILE_DIR:GameWorldshaperHub>" VERBATIM)
        install(FILES "${_mingw_bin}/${dll}" DESTINATION . COMPONENT hub)
    endforeach()
endif()

install(TARGETS GameWorldshaperHub RUNTIME DESTINATION . COMPONENT hub)

# --- Packaging: a Hub installer (ZIP always; NSIS when makensis is present) ---
set(CPACK_PACKAGE_NAME "GameWorldshaperHub")
set(CPACK_PACKAGE_VENDOR "GameWorldshaper")
set(CPACK_PACKAGE_VERSION "${GWS_HUB_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "GameWorldshaper Hub")
set(CPACK_PACKAGE_EXECUTABLES "GameWorldshaperHub;GameWorldshaper Hub")
# Package ONLY the "hub" component (exe + runtime DLLs) so FetchContent deps'
# own install() output (GLFW headers/libs, etc.) is excluded.
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL hub)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_ARCHIVE_FILE_NAME "GameWorldshaperHub-${GWS_HUB_VERSION}-win64")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    find_program(GWS_MAKENSIS makensis)
    if(GWS_MAKENSIS)
        list(APPEND CPACK_GENERATOR "NSIS")
    endif()
    set(CPACK_NSIS_DISPLAY_NAME "GameWorldshaper Hub")
    set(CPACK_NSIS_MENU_LINKS "GameWorldshaperHub.exe" "GameWorldshaper Hub")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
endif()
include(CPack)
