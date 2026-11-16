# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.25)
project(armv5 VERSION 0.1.0 LANGUAGES C CXX)

find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 19)
  message(FATAL_ERROR "armv5 requires LLVM 19")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(armv5 STATIC src/core.cpp src/decode.cpp)
add_library(armv5::core ALIAS armv5)
target_compile_features(armv5 PUBLIC cxx_std_23)
target_include_directories(armv5 PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_include_directories(armv5 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(armv5 PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(armv5 PROPERTIES CXX_EXTENSIONS OFF EXPORT_NAME core)
# An edit to any preparation input invalidates cached compiler identities,
# including uncommitted source changes. No Git invocation is needed to build.
set(ARMV5_PREPARATION_INPUTS CMakeLists.txt include/armv5/core.hpp
  include/armv5/memory.hpp src/core.cpp src/decode.cpp src/decode.hpp src/identity.hpp.in)
set(ARMV5_INPUT_HASHES "")
foreach(input IN LISTS ARMV5_PREPARATION_INPUTS)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${input}")
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${input}" input_hash)
  string(APPEND ARMV5_INPUT_HASHES "${input_hash}")
endforeach()
string(SHA256 ARMV5_SOURCE_ID "${ARMV5_INPUT_HASHES}")
configure_file(src/identity.hpp.in generated/identity.hpp @ONLY)

include(CTest)
if(BUILD_TESTING)
  foreach(test IN ITEMS isa core)
    add_executable(armv5_${test}_test test/${test}/preparation.cpp)
    target_link_libraries(armv5_${test}_test PRIVATE armv5::core)
    target_include_directories(armv5_${test}_test PRIVATE test)
    target_compile_options(armv5_${test}_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${test} COMMAND armv5_${test}_test)
  endforeach()
endif()

include(GNUInstallDirs)
install(TARGETS armv5 EXPORT armv5Targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/armv5/core.hpp include/armv5/memory.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/armv5)
install(EXPORT armv5Targets NAMESPACE armv5:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/armv5)

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/armv5Config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/armv5Config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/armv5")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/armv5ConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/armv5Config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/armv5ConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/armv5")
