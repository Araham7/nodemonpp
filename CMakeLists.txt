cmake_minimum_required(VERSION 3.16)
project(nodemonpp VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
option(NODEMONPP_BUILD_TESTS "Build nodemonpp tests (need python3)" ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "nodemonpp uses inotify and needs Linux")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)

add_executable(nodemonpp src/main.cc)
target_link_libraries(nodemonpp PRIVATE PkgConfig::JSONCPP)
target_compile_options(nodemonpp PRIVATE -Wall -Wextra)

include(GNUInstallDirs)
install(TARGETS nodemonpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(NODEMONPP_BUILD_TESTS)
  enable_testing()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME nodemonpp_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nodemonpp.py $<TARGET_FILE:nodemonpp>)
endif()
