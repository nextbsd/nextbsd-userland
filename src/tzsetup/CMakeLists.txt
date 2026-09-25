# tzsetup — pick a time zone in the installer's look (nextbsd-userland#302).
#
# Replaces FreeBSD's bsddialog tzsetup at the same path; nextbsd-freebsd-compat
# strips the base copy through scripts/collisions, the way it does for passwd
# and adduser.
#
# Shares nextbsd-installer's amber look through ../libnbui (#304) and its
# vendored FTXUI at ../ftxui, so this adds no dependency of its own.
#
#   cmake -S src/tzsetup -B build -G Ninja && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(nextbsd_tzsetup CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The same vendored FTXUI the installer builds, lib-only. EXCLUDE_FROM_ALL so
# building this target never builds ftxui's own extras.
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_QUIET          ON  CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ftxui ${CMAKE_BINARY_DIR}/ftxui EXCLUDE_FROM_ALL)

add_executable(tzsetup
  main.cpp
  zones.cpp
  apply.cpp
)
target_include_directories(tzsetup PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../libnbui)
target_link_libraries(tzsetup PRIVATE ftxui::component)

install(TARGETS tzsetup RUNTIME DESTINATION usr/sbin)
install(FILES tzsetup.8 DESTINATION usr/share/man/man8)
