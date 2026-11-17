# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.25)
project(armv5 VERSION 0.1.0 LANGUAGES C CXX)

find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 19)
  message(FATAL_ERROR "armv5 requires LLVM 19")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(armv5 STATIC src/core.cpp src/decode.cpp src/image.cpp src/native.cpp
  src/lower.cpp src/alu.cpp src/memory.cpp src/dsp.cpp src/thumb.cpp src/flow.cpp src/loops.cpp src/system.cpp src/system.hpp)
target_include_directories(armv5 SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(armv5 PRIVATE LLVM)
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
  include/armv5/memory.hpp src/core.cpp src/decode.cpp src/decode.hpp src/identity.hpp.in src/image.cpp
  src/image.hpp src/native.cpp src/runtime.hpp src/lower.cpp src/lower.hpp
  src/alu.cpp src/memory.cpp src/dsp.cpp src/thumb.cpp src/flow.cpp src/loops.cpp src/system.cpp src/system.hpp)
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
  target_sources(armv5_isa_test PRIVATE test/isa/modes.cpp test/isa/dsp.cpp test/isa/thumb.cpp test/isa/system.cpp test/isa/tcm.cpp test/isa/control.cpp test/isa/aborts.cpp)
  target_sources(armv5_core_test PRIVATE test/core/consumer.cpp test/core/interrupts.cpp test/core/returns.cpp test/core/loops.cpp test/core/system.cpp test/core/tcm.cpp test/core/control.cpp)
  target_include_directories(armv5_core_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(armv5_core_test PRIVATE LLVM)
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

if(BUILD_TESTING)
  add_executable(armv5_bench test/bench/kernels.cpp test/bench/reference.cpp)
  target_link_libraries(armv5_bench PRIVATE armv5::core)
  target_include_directories(armv5_bench PRIVATE test)
  target_include_directories(armv5_bench SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(armv5_bench PRIVATE LLVM)
  target_compile_options(armv5_bench PRIVATE -O3 -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME bench COMMAND armv5_bench)
endif()
