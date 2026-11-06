# Build the headless vConnect-STAND binary (GPL) against system libraries.
#
# This builds only the `stand` synthesis library and a small `stand_main`
# front-end. The Qt GUI (StandConverter), the C++/CLI vtdEditor and the
# vcnctd daemon are deliberately excluded: they need Qt5/Qt6, Windows
# managed C++ and a POSIX daemon respectively, and none are needed to make
# vConnect usable as an OpenUtau resampler.
#
# vConnect-STAND is GPL. Building it here does not change its license, and
# because OpenUtau invokes the resulting binary across a process boundary,
# OpenUtau (MIT) is not a derived work of it.

cmake_minimum_required(VERSION 3.16)
project(vConnectStand LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3 REQUIRED fftw3)
pkg_check_modules(OGG REQUIRED ogg)
pkg_check_modules(VORBIS REQUIRED vorbisfile)
# vorbisenc is only pulled in by the converter; keep it optional.
pkg_check_modules(VORBISENC vorbisenc)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(Iconv)

# The 2011 sources rely on PATH_MAX / memset / uint32_t being visible transitively.
add_compile_options(-w)

set(STAND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/stand)

file(GLOB STAND_SOURCES
  ${STAND_DIR}/*.cpp
  ${STAND_DIR}/utau/*.cpp
  ${STAND_DIR}/vsq/*.cpp
  ${STAND_DIR}/WaveBuffer/*.cpp
  ${STAND_DIR}/world/*.cpp
)
# The original CLI predates the resampler front-end; stand_main.cpp replaces it.
list(FILTER STAND_SOURCES EXCLUDE REGEX "/main\\.cpp$")

add_library(stand STATIC ${STAND_SOURCES})
# Forced includes rather than #include edits: keeps the vendored 2011 tree
# byte-identical to upstream as far as the build is concerned.
target_compile_options(stand PUBLIC -include ${CMAKE_CURRENT_SOURCE_DIR}/frontend/forced_includes.h)
target_include_directories(stand PUBLIC
  ${STAND_DIR}
  ${FFTW3_INCLUDE_DIRS}
  ${OGG_INCLUDE_DIRS}
  ${VORBIS_INCLUDE_DIRS}
)
target_link_libraries(stand PUBLIC
  ${FFTW3_LIBRARIES}
  ${OGG_LIBRARIES}
  ${VORBIS_LIBRARIES}
  ${VORBISENC_LIBRARIES}
  vorbis
  Boost::filesystem
  Iconv::Iconv
  pthread
)

add_executable(vConnect-STAND ${CMAKE_CURRENT_SOURCE_DIR}/frontend/stand_main.cpp)
target_link_libraries(vConnect-STAND PRIVATE stand)

install(TARGETS vConnect-STAND RUNTIME DESTINATION .)
