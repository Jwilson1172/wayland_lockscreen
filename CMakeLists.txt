cmake_minimum_required(VERSION 4.4)
project(NyanLock C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(PAM REQUIRED IMPORTED_TARGET pam)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

add_executable(nyanlock)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.c")
target_sources(nyanlock PRIVATE ${SOURCES})
target_include_directories(nyanlock PRIVATE include)

target_link_libraries(nyanlock PRIVATE
  PkgConfig::PAM
  PkgConfig::WAYLAND_CLIENT
  PkgConfig::XKBCOMMON
)
