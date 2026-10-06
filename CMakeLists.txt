cmake_minimum_required(VERSION 3.27...4.5)
project(NyanLock C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default to Debug build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ENABLE_SANITIZERS "build with ASan + UBSan (default)" ON)
option(ENABLE_CLANG_TIDY "Run clang-tidy during builds" OFF)
option(ENABLE_CPPCHECK "Run cppcheck during builds" OFF)
option(WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(ENABLE_COVERAGE "Build with gcov(lcov) coverage" OFF)

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

# ------Generated code-----
# We generally don't care about this code's lint or analysis
# We could make a version that we do maintain but
# We'll burn that bridge when we get to it.
set(GENERATED_SOURCE_REGEX "-protocol\\.c$")
set(GENERATED_SOURCES ${SOURCES})
list(FILTER GENERATED_SOURCES INCLUDE REGEX "${GENERATED_SOURCE_REGEX}")
if(GENERATED_SOURCES)
  set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
  SKIP_LINTING ON
  COMPILE_OPTIONS "-w")
endif()

target_include_directories(nyanlock SYSTEM PRIVATE include/protocols)

#----API Feature selection
target_compile_definitions(nyanlock PRIVATE
  _POSIX_C_SOURCE=199309L
  _DEFAULT_SOURCE
)

# ---- Warnings ----
target_compile_options(nyanlock PRIVATE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat=2
  -Wnull-dereference
)
if(WARNINGS_AS_ERRORS)
  target_compile_options(nyanlock PRIVATE -Werror)
endif()

# ---- Sanitizers (Debug only, compile AND link) ----
if(ENABLE_SANITIZERS)
  target_compile_options(nyanlock PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>)
  target_link_options(nyanlock PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
endif()

# ---- Coverage ----
if(ENABLE_COVERAGE)
  target_compile_options(nyanlock PRIVATE --coverage -O0)
  target_link_options(nyanlock PRIVATE --coverage)
endif()

# ---- Hardening (non-Debug builds) ----
target_compile_options(nyanlock PRIVATE
  $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3 -fstack-protector-strong>)
target_link_options(nyanlock PRIVATE
  $<$<NOT:$<CONFIG:Debug>>:-Wl,-z,relro,-z,now>)

# ---- Static analysis ----
find_program(CLANG_TIDY_EXE clang-tidy)
find_program(CPPCHECK_EXE cppcheck)

if(ENABLE_CLANG_TIDY AND CLANG_TIDY_EXE)
  set_target_properties(nyanlock PROPERTIES C_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()
if(ENABLE_CPPCHECK AND CPPCHECK_EXE)
  set_target_properties(nyanlock PROPERTIES
    C_CPPCHECK "${CPPCHECK_EXE};--enable=warning,style,performance;--inline-suppr;--quiet")
endif()
