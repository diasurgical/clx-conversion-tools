cmake_minimum_required(VERSION 3.16)

project(DvlGfx VERSION 1.0.0 LANGUAGES C CXX)

option(ASAN "Enable address sanitizer" ON)
option(UBSAN "Enable undefined behaviour sanitizer" ON)
option(ENABLE_INSTALL "Enable install targets" ON)
option(ENABLE_PNG_TOOLS "Build png2clx and clx2png" ON)
option(USE_SYSTEM_LIBPNG "Use system libpng instead of fetching it and linking it statically" OFF)
include(CMakeDependentOption)
cmake_dependent_option(USE_SYSTEM_ZLIB "Use system zlib for the fetched libpng instead of fetching it and linking it statically" OFF
  "ENABLE_PNG_TOOLS;NOT USE_SYSTEM_LIBPNG" OFF)

if(POLICY CMP0135)
  # Set the timestamps of files extracted by FetchContent to the time of extraction.
  cmake_policy(SET CMP0135 NEW)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

if(CMAKE_BUILD_TYPE MATCHES "Release")
  set(ASAN OFF)
  set(UBSAN OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # for clang-tidy

# Built-in palettes
foreach(_path
  default diablo_menu hellfire_menu)
  file(READ data/${_path}.pal _hex_string HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _array_values ${_hex_string})
  string(REGEX REPLACE ", $" "" _array_values ${_array_values})
  set(_c_name "dvl_gfx_embedded_${_path}_pal")
  string(APPEND _output_c "
const unsigned char ${_c_name}_data[] = {
  ${_array_values}
};
const unsigned ${_c_name}_size = sizeof(${_c_name}_data) / sizeof(${_c_name}_data[0]);
")
  string(APPEND _output_h "
extern unsigned char const ${_c_name}_data[];
extern const unsigned ${_c_name}_size;
")
endforeach()

# Note: The file will only be regenarated on reconfigure, not when the source file changes.
# We do not expect the source files to change much so it's fine.
file(GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dvl_gfx_embedded_palettes.h
  CONTENT "#ifndef DVL_GFX_EMBEDDED_PALETTES_H_
${_output_h}
#define DVL_GFX_EMBEDDED_PALETTES_H_
#endif  // DVL_GFX_EMBEDDED_PALETTES_H_
")
file(GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dvl_gfx_embedded_palettes.c
  CONTENT "#include \"dvl_gfx_embedded_palettes.h\"
${_output_c}
")
add_library(dvl_gfx_embedded_palettes
  ${CMAKE_CURRENT_BINARY_DIR}/dvl_gfx_embedded_palettes.c)
add_library(DvlGfx::embedded_palettes ALIAS dvl_gfx_embedded_palettes)
target_include_directories(dvl_gfx_embedded_palettes INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<INSTALL_INTERFACE:include>)
set_target_properties(dvl_gfx_embedded_palettes PROPERTIES PUBLIC_HEADER "${CMAKE_CURRENT_BINARY_DIR}/dvl_gfx_embedded_palettes.h")

add_library(common INTERFACE)
target_include_directories(common INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/public/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/tl>
  $<INSTALL_INTERFACE:include>)
set_target_properties(common PROPERTIES PUBLIC_HEADER
  "src/public/include/dvl_gfx_common.hpp;src/public/include/dvl_gfx_endian.hpp")

add_library(clx_decode INTERFACE)
add_library(DvlGfx::clx_decode ALIAS clx_decode)
target_include_directories(clx_decode INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/public/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(clx_decode PROPERTIES PUBLIC_HEADER "src/public/include/clx_decode.hpp")

add_library(
  clx_encode
  src/internal/clx_encode.cpp)
add_library(DvlGfx::clx_encode ALIAS clx_encode)
target_link_libraries(clx_encode PUBLIC common)
set_target_properties(clx_decode PROPERTIES PUBLIC_HEADER "src/public/include/clx_encode.hpp")
target_include_directories(clx_encode PRIVATE src/internal)

add_library(
  cel2clx
  src/internal/cel2clx.cpp
)
add_library(DvlGfx::cel2clx ALIAS cel2clx)
target_link_libraries(cel2clx PUBLIC common)
target_link_libraries(cel2clx PRIVATE clx_encode)
set_target_properties(cel2clx PROPERTIES PUBLIC_HEADER "src/public/include/cel2clx.hpp")
target_include_directories(cel2clx PRIVATE src/internal)

add_executable(cel2clx_main src/internal/cel2clx_main.cpp)
set_property(TARGET cel2clx_main PROPERTY RUNTIME_OUTPUT_NAME cel2clx)
target_link_libraries(cel2clx_main PRIVATE cel2clx)
target_include_directories(cel2clx_main PRIVATE src/internal)

add_library(
  cl22clx
  src/internal/cl22clx.cpp
)
add_library(DvlGfx::cl22clx ALIAS cl22clx)
target_link_libraries(cl22clx PUBLIC common)
target_link_libraries(cl22clx PRIVATE clx_encode)
set_target_properties(cl22clx PROPERTIES PUBLIC_HEADER "src/public/include/cl22clx.hpp")
target_include_directories(cl22clx PRIVATE src/internal)

add_executable(cl22clx_main src/internal/cl22clx_main.cpp)
set_property(TARGET cl22clx_main PROPERTY RUNTIME_OUTPUT_NAME cl22clx)
target_link_libraries(cl22clx_main PRIVATE cl22clx)
target_include_directories(cl22clx_main PRIVATE src/internal)

add_library(
  pcx_encode
  src/internal/pcx_encode.cpp)
target_link_libraries(pcx_encode PUBLIC common)
target_include_directories(pcx_encode PRIVATE src/internal)

add_executable(clx2pcx_main src/internal/clx2pcx_main.cpp)
set_property(TARGET clx2pcx_main PROPERTY RUNTIME_OUTPUT_NAME clx2pcx)
target_link_libraries(clx2pcx_main PRIVATE clx2pixels pcx_encode dvl_gfx_embedded_palettes)
target_include_directories(clx2pcx_main PRIVATE src/internal)

add_library(
  pcx2clx
  src/internal/pcx.cpp
  src/internal/pcx2clx.cpp)
add_library(DvlGfx::pcx2clx ALIAS pcx2clx)
target_link_libraries(pcx2clx PUBLIC common)
target_link_libraries(pcx2clx PRIVATE clx_encode)
set_target_properties(pcx2clx PROPERTIES PUBLIC_HEADER "src/public/include/pcx2clx.hpp")
target_include_directories(pcx2clx PRIVATE src/internal)

add_executable(pcx2clx_main src/internal/pcx2clx_main.cpp)
set_property(TARGET pcx2clx_main PROPERTY RUNTIME_OUTPUT_NAME pcx2clx)
target_link_libraries(pcx2clx_main PRIVATE pcx2clx)
target_include_directories(pcx2clx_main PRIVATE src/internal)

add_library(
  pixels2clx
  src/internal/pixels2clx.cpp)
add_library(DvlGfx::pixels2clx ALIAS pixels2clx)
target_link_libraries(pixels2clx PUBLIC common)
target_link_libraries(pixels2clx PRIVATE clx_encode)
set_target_properties(pixels2clx PROPERTIES PUBLIC_HEADER "src/public/include/pixels2clx.hpp")
target_include_directories(pixels2clx PRIVATE src/internal)

add_library(
  clx2pixels
  src/internal/clx2pixels.cpp)
add_library(DvlGfx::clx2pixels ALIAS clx2pixels)
target_link_libraries(clx2pixels PUBLIC common clx_decode)
set_target_properties(clx2pixels PROPERTIES PUBLIC_HEADER "src/public/include/clx2pixels.hpp")
target_include_directories(clx2pixels PRIVATE src/internal)

set(_executables cel2clx_main cl22clx_main clx2pcx_main pcx2clx_main)

if(ENABLE_PNG_TOOLS)
  if(USE_SYSTEM_LIBPNG)
    find_package(PNG REQUIRED)
  else()
    if(USE_SYSTEM_ZLIB)
      find_package(ZLIB REQUIRED)
    else()
      add_subdirectory(third_party/zlib)
    endif()
    add_subdirectory(third_party/libpng)
  endif()

  add_library(png_codec STATIC src/internal/png_codec.cpp)
  target_link_libraries(png_codec PUBLIC common PRIVATE PNG::PNG)
  target_include_directories(png_codec PRIVATE src/internal)

  add_executable(png2clx_main src/internal/png2clx_main.cpp)
  set_property(TARGET png2clx_main PROPERTY RUNTIME_OUTPUT_NAME png2clx)
  target_link_libraries(png2clx_main PRIVATE png_codec pixels2clx)
  target_include_directories(png2clx_main PRIVATE src/internal)

  add_executable(clx2png_main src/internal/clx2png_main.cpp)
  set_property(TARGET clx2png_main PROPERTY RUNTIME_OUTPUT_NAME clx2png)
  target_link_libraries(clx2png_main PRIVATE png_codec clx2pixels dvl_gfx_embedded_palettes)
  target_include_directories(clx2png_main PRIVATE src/internal)

  list(APPEND _executables png2clx_main clx2png_main)
endif()

foreach(_target ${_executables})
  if(ASAN)
    target_compile_options(${_target} PUBLIC -fsanitize=address -fsanitize-recover=address)
    target_link_libraries(${_target} PUBLIC -fsanitize=address -fsanitize-recover=address)
  endif()

  if(UBSAN)
    target_compile_options(${_target} PUBLIC -fsanitize=undefined)
    target_link_libraries(${_target} PUBLIC -fsanitize=undefined)
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_link_libraries(${_target} PUBLIC "-static-libgcc -static-libstdc++")
  endif()
endforeach()

if(ENABLE_INSTALL)
  install(
    TARGETS cel2clx cl22clx pcx2clx pixels2clx clx2pixels clx_encode clx_decode dvl_gfx_embedded_palettes common
    EXPORT DvlGfxTargets
    PUBLIC_HEADER
    CONFIGURATIONS Release
    COMPONENT Development
  )
  install(
    TARGETS ${_executables}
    CONFIGURATIONS Release
    COMPONENT Binaries
  )

  if(NOT DEFINED LIB_INSTALL_DIR)
    set(LIB_INSTALL_DIR lib)
  endif()
  set(ConfigPackageLocation "${LIB_INSTALL_DIR}/cmake/DvlGfx")

  include(CMakePackageConfigHelpers)
  configure_package_config_file(cmake/DvlGfx.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/DvlGfx/DvlGfxConfig.cmake"
    INSTALL_DESTINATION ${ConfigPackageLocation})
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/DvlGfx/DvlGfxConfigVersion.cmake"
    VERSION ${DvlGfx_VERSION}
    COMPATIBILITY AnyNewerVersion
  )

  export(EXPORT DvlGfxTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/DvlGfx/DvlGfxTargets.cmake"
    NAMESPACE DvlGfx::
  )

  install(EXPORT DvlGfxTargets
    FILE
      DvlGfxTargets.cmake
    NAMESPACE
      DvlGfx::
    DESTINATION
      ${ConfigPackageLocation}
    COMPONENT
      Development
  )
  install(
    FILES
      "${CMAKE_CURRENT_BINARY_DIR}/DvlGfx/DvlGfxConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/DvlGfx/DvlGfxConfigVersion.cmake"
    DESTINATION
      ${ConfigPackageLocation}
    COMPONENT
      Development
  )
endif()
