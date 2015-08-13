project (master-thesis)

# Disable debug messages and assertions.
add_definitions(-DNDEBUG)

cmake_minimum_required (VERSION 2.8)

set (CMAKE_CC_FLAGS     "-Wall -std=c99 -O2")
set (CMAKE_CXX_FLAGS    "-Wall -std=c++11 -O2")

find_package (Threads REQUIRED)

include_directories (.)

add_executable(httpd_epoll httpd_epoll.cpp)

target_link_libraries (httpd_epoll  pthread)
