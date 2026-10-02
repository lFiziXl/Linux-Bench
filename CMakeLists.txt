cmake_minimum_required(VERSION 3.16)

project(LinuxBench
    VERSION 0.1.0
    DESCRIPTION "Native CPU benchmark (Cinebench-style) for Linux"
    LANGUAGES CXX
)

# --- Language / build settings ---------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- Dependencies ------------------------------------------------------------
include(FetchContent)

find_package(Threads REQUIRED)   # Threads::Threads (pthread)
find_package(OpenGL REQUIRED)    # OpenGL::GL

# GLFW 3.4 — windowing, input, OpenGL context (fetched at configure time).
# On Linux the X11 backend is used (runs under XWayland on Wayland sessions).
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

# Dear ImGui v1.90.4 — immediate-mode GUI (core + GLFW / OpenGL3 backends).
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.4
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

# Intel Embree v4.4.1 — production-grade SIMD BVH + ray intersection library.
# It replaces the old analytic sphere and provides the heavy traceRay()
# workload (a ~10M-triangle procedural scene).
#
# Options are set as cache variables *before* the fetch so the subproject
# configures with them and the build stays lean: no ISPC code generation,
# no tutorials, no test suite, and no TBB dependency (the built-in tasking
# scheduler is all a single-ray renderer needs).
set(EMBREE_ISPC_SUPPORT   OFF        CACHE BOOL   "Embree: disable ISPC support"        FORCE)
set(EMBREE_TUTORIALS      OFF        CACHE BOOL   "Embree: do not build tutorials"      FORCE)
set(EMBREE_TESTS          OFF        CACHE BOOL   "Embree: do not build tests"          FORCE)
set(EMBREE_TASKING_SYSTEM INTERNAL   CACHE STRING "Embree: built-in tasking, no TBB"    FORCE)

FetchContent_Declare(
    embree
    GIT_REPOSITORY https://github.com/RenderKit/embree.git
    GIT_TAG        v4.4.1
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

FetchContent_MakeAvailable(glfw imgui embree)

# --- Target --------------------------------------------------------------------
# Dear ImGui is a plain source tree: the core and the two backends are
# compiled straight into the executable.
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

add_executable(LinuxBench
    src/main.cpp
    src/Framebuffer.cpp
    src/Scene.cpp
    src/TileDispatcher.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(LinuxBench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    # Embree v4's public headers live in <repo>/include/embree4
    # (umbrella header rtcore.h).
    ${embree_SOURCE_DIR}/include/embree4
)

target_link_libraries(LinuxBench PRIVATE
    Threads::Threads
    OpenGL::GL
    glfw
    embree
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(LinuxBench PRIVATE -Wall -Wextra -Wpedantic)

    # Keep the build log clean: silence warnings in vendored third-party code.
    target_compile_options(glfw PRIVATE -w)
    foreach(SRC IN LISTS IMGUI_SOURCES)
        set_source_files_properties(${SRC} PROPERTIES COMPILE_OPTIONS "-w")
    endforeach()
endif()
