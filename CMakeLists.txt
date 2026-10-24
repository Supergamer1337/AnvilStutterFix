cmake_minimum_required(VERSION 3.21)
project(AnvilStutterFix LANGUAGES C CXX)

option(ASF_DEBUG "Build the debug variant" OFF)
set(ASF_VERSION "dev" CACHE STRING "Version shown in the log")

add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)

add_library(version SHARED
    src/d3d12_hooks.cpp
    src/debug.cpp
    src/dllmain.cpp
    src/proxy_version.cpp
    src/util.cpp
    src/version.def
)
set_target_properties(version PROPERTIES PREFIX "" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_definitions(version PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 ASF_VERSION="${ASF_VERSION}" $<$<BOOL:${ASF_DEBUG}>:ASF_DEBUG=1>)
target_compile_options(version PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(version PRIVATE -static -s -Wl,--no-insert-timestamp)
target_link_libraries(version PRIVATE minhook)

install(FILES $<TARGET_FILE:version> LICENSE DESTINATION . COMPONENT AnvilStutterFix)
install(FILES third_party/minhook/LICENSE.txt DESTINATION . RENAME LICENSE-MinHook.txt COMPONENT AnvilStutterFix)
