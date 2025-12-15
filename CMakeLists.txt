cmake_minimum_required(VERSION 3.16)

project(objective-c-mangler LANGUAGES C OBJC CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

####################################################################################################

add_custom_target(BuildTools
  SOURCES
    _clang-format
    .clang-tidy
    .gitignore
    .pre-commit-config.yaml
    Readme.md
)

####################################################################################################
# 3rdparty dependencies via cmp

file(
  DOWNLOAD
  https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.42.0/CPM.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
  EXPECTED_HASH SHA256=2020b4fc42dba44817983e06342e682ecfc3d2f484a581f11cc5731fbe4dce8a
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/cpm_source_cache")

CPMAddPackage("gh:CLIUtils/CLI11@2.6.1")

####################################################################################################
# clang

if(EXISTS "/opt/homebrew/opt/llvm/lib/cmake/llvm")
  list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/llvm/lib/cmake/llvm")
else()
  CPMAddPackage(
    NAME          qt_llvm
    DOWNLOAD_ONLY
    URL           https://download.qt.io/development_releases/prebuilt/libclang/qt/libclang-llvmorg-20.1.0-macos-universal.7z
  )
  list(APPEND CMAKE_PREFIX_PATH "${qt_llvm_SOURCE_DIR}/lib/cmake/llvm")
endif()

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

if (NOT LLVM_FOUND)
  message(FATAL_ERROR "LLVM not found. Please install LLVM or set CMAKE_PREFIX_PATH to the LLVM installation directory.")
endif()

llvm_map_components_to_libnames(llvm_libs Support Object)

####################################################################################################
# executable

add_executable(objective-c-mangler main.cpp)
target_link_libraries(objective-c-mangler
  PRIVATE
    ${llvm_libs}
    CLI11::CLI11
)

target_include_directories(objective-c-mangler PUBLIC "${LLVM_INCLUDE_DIRS}")

####################################################################################################
# tests

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  enable_testing()

  foreach(test_executable test_executable_1 test_executable_2 test_executable_3 test_executable_4 test_executable_5)
    add_executable(${test_executable}
      tests/test.m
    )
    target_link_libraries(${test_executable} PUBLIC objc)
  endforeach()

  # suffix
  add_custom_command(TARGET test_executable_1 POST_BUILD
    COMMAND "$<TARGET_FILE:objective-c-mangler>"
            --replace "_Suffix" "_SUFFIX"
            "$<TARGET_FILE:test_executable_1>"

    # we've changed the binary, so we need to re-sign it
    COMMAND codesign
            --sign
            "-"
            "$<TARGET_FILE:test_executable_1>"
  )

  add_test(NAME test_replace_suffix COMMAND test_executable_1)
  set_property(TEST test_replace_suffix PROPERTY
    PASS_REGULAR_EXPRESSION "^TestClass_SUFFIX"
  )

  # prefix
  add_custom_command(TARGET test_executable_2 POST_BUILD
    COMMAND "$<TARGET_FILE:objective-c-mangler>"
            --replace "TestClass_" "TestKlass_"
            "$<TARGET_FILE:test_executable_2>"

    # we've changed the binary, so we need to re-sign it
    COMMAND codesign
            --sign
            "-"
            "$<TARGET_FILE:test_executable_2>"
  )

  add_test(NAME test_replace_prefix COMMAND test_executable_2)
  set_property(TEST test_replace_prefix PROPERTY
    PASS_REGULAR_EXPRESSION "^TestKlass_Suffix"
  )

  # infix
  add_custom_command(TARGET test_executable_3 POST_BUILD
    COMMAND "$<TARGET_FILE:objective-c-mangler>"
            --replace "Class_" "Qlass_"
            "$<TARGET_FILE:test_executable_3>"

    # we've changed the binary, so we need to re-sign it
    COMMAND codesign
            --sign
            "-"
            "$<TARGET_FILE:test_executable_3>"
  )

  add_test(NAME test_replace_infix COMMAND test_executable_3)
  set_property(TEST test_replace_infix PROPERTY
    PASS_REGULAR_EXPRESSION "^TestQlass_Suffix"
  )

  # exclude
  add_custom_command(TARGET test_executable_4 POST_BUILD
    COMMAND "$<TARGET_FILE:objective-c-mangler>"
            --exclude TestClass_Suffix
            --replace "_Suffix" "_SUFFIX"
            "$<TARGET_FILE:test_executable_4>"

    # we've changed the binary, so we need to re-sign it
    COMMAND codesign
            --sign
            "-"
            "$<TARGET_FILE:test_executable_4>"
  )

  add_test(NAME test_exclude COMMAND test_executable_4)
  set_property(TEST test_exclude PROPERTY
    PASS_REGULAR_EXPRESSION "^TestClass_Suffix"
  )

  # dry-run
  add_custom_command(TARGET test_executable_5 POST_BUILD
    COMMAND "$<TARGET_FILE:objective-c-mangler>"
            --dry-run
            --exclude TestClass_Suffix
            --replace "_Suffix" "_SUFFIX"
            "$<TARGET_FILE:test_executable_5>"
  )

  add_test(NAME test_dry-run COMMAND test_executable_5)
  set_property(TEST test_dry-run PROPERTY
    PASS_REGULAR_EXPRESSION "^TestClass_Suffix"
  )
endif()
