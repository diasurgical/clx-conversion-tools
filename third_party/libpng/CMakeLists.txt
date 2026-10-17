include(functions/FetchContent_ExcludeFromAll_backport)

# Workaround for deprecation of older CMake versions
set(CMAKE_POLICY_VERSION_MINIMUM 3.22)

set(PNG_STATIC ON)
set(PNG_SHARED OFF)
set(PNG_FRAMEWORK OFF)
set(PNG_TESTS OFF)
set(PNG_TOOLS OFF)
set(SKIP_INSTALL_ALL ON)

if(APPLE AND "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "arm64")
  set(PNG_ARM_NEON "on" CACHE STRING "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare_ExcludeFromAll(libpng
  URL https://github.com/pnggroup/libpng/archive/3061454d980de7d53608f594194cfac722721d2a.tar.gz
  URL_HASH MD5=2007281c431730eca37a71878efcc858
)
FetchContent_MakeAvailable_ExcludeFromAll(libpng)

target_include_directories(png_static INTERFACE
  $<BUILD_INTERFACE:${libpng_SOURCE_DIR}>
  $<BUILD_INTERFACE:${libpng_BINARY_DIR}>)

add_library(PNG::PNG ALIAS png_static)
