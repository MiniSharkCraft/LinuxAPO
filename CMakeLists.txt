cmake_minimum_required(VERSION 3.20)
project(SkyAPO VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SKYAPO_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(FFTW3F QUIET IMPORTED_TARGET fftw3f)

if(SKYAPO_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

set(EAPO ${CMAKE_CURRENT_SOURCE_DIR}/upstream/equalizerapo)
include(cmake/PortableEapo.cmake)
add_library(skyapo-eapo-dsp STATIC
  ${EAPO_SOURCES}
  src/platform/linux/compat.cpp
  src/platform/linux/ChannelHelper.cpp
  src/platform/linux/MemoryHelper.cpp
)
target_include_directories(skyapo-eapo-dsp BEFORE PRIVATE src/platform/linux ${EAPO_PORT} ${EAPO})
target_compile_definitions(skyapo-eapo-dsp PRIVATE __forceinline=inline)
target_compile_options(skyapo-eapo-dsp PRIVATE -Wno-write-strings)
if(FFTW3F_FOUND)
  target_link_libraries(skyapo-eapo-dsp PRIVATE PkgConfig::FFTW3F PkgConfig::SNDFILE)
endif()

add_library(skyapo-core STATIC src/core/Engine.cpp)
target_include_directories(skyapo-core BEFORE PRIVATE src/platform/linux)
target_include_directories(skyapo-core PUBLIC src/core ${EAPO_PORT} ${EAPO_PORT}/filters ${EAPO}/filters ${EAPO} ${EAPO}/helpers)
target_compile_definitions(skyapo-core PRIVATE __forceinline=inline)
target_link_libraries(skyapo-core PUBLIC skyapo-eapo-dsp)
if(FFTW3F_FOUND)
  target_sources(skyapo-core PRIVATE src/platform/linux/ConvolutionFilterLinux.cpp)
  target_compile_definitions(skyapo-core PRIVATE SKYAPO_HAVE_CONVOLUTION=1)
else()
  message(STATUS "FFTW3f not found: GraphicEQ/Convolution directives disabled")
endif()

add_executable(skyapo-render src/render/main.cpp)
target_link_libraries(skyapo-render PRIVATE skyapo-core PkgConfig::SNDFILE)

add_executable(skyapo src/cli/main.cpp)
target_link_libraries(skyapo PRIVATE skyapo-core)
if(PIPEWIRE_FOUND)
  add_library(skyapo-pipewire STATIC src/pipewire/DeviceManager.cpp src/pipewire/Runtime.cpp src/platform/RealtimeAudit.cpp)
  target_link_libraries(skyapo-pipewire PUBLIC skyapo-core PkgConfig::PIPEWIRE)
  target_link_libraries(skyapo PRIVATE skyapo-pipewire)
  target_sources(skyapo PRIVATE src/platform/RealtimeAudit.cpp)
  target_link_options(skyapo PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign)
  target_compile_definitions(skyapo PRIVATE SKYAPO_HAVE_PIPEWIRE=1)
endif()

add_executable(skyapod src/daemon/main.cpp)
if(NOT PIPEWIRE_FOUND)
  message(FATAL_ERROR "The realtime daemon requires libpipewire-0.3 development files")
endif()
target_link_libraries(skyapod PRIVATE skyapo-pipewire)
target_link_options(skyapod PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign)

enable_testing()
add_executable(skyapo-tests tests/core_tests.cpp)
target_link_libraries(skyapo-tests PRIVATE skyapo-core PkgConfig::SNDFILE)
if(FFTW3F_FOUND)
  target_compile_definitions(skyapo-tests PRIVATE SKYAPO_TEST_CONVOLUTION=1)
endif()
add_test(NAME skyapo-core COMMAND skyapo-tests)
add_executable(skyapo-realtime-safety-tests tests/realtime_safety_tests.cpp src/platform/RealtimeAudit.cpp)
target_link_libraries(skyapo-realtime-safety-tests PRIVATE skyapo-core)
if(FFTW3F_FOUND)
  target_compile_definitions(skyapo-realtime-safety-tests PRIVATE SKYAPO_TEST_CONVOLUTION=1)
  target_link_libraries(skyapo-realtime-safety-tests PRIVATE PkgConfig::SNDFILE)
endif()
target_link_options(skyapo-realtime-safety-tests PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign)
add_test(NAME skyapo-realtime-safety COMMAND skyapo-realtime-safety-tests)

add_executable(skyapo-render-tests tests/render_tests.cpp)
target_link_libraries(skyapo-render-tests PRIVATE PkgConfig::SNDFILE)
add_test(NAME skyapo-offline-render COMMAND skyapo-render-tests $<TARGET_FILE:skyapo-render> ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt)

# Real hardware test, explicitly invoked; never part of unattended CTest.
add_executable(skyapo-realtime-probe tests/realtime_probe.cpp)
target_link_libraries(skyapo-realtime-probe PRIVATE PkgConfig::PIPEWIRE PkgConfig::SNDFILE)
add_executable(skyapo-pipewire-lifetime tests/pipewire_lifetime.cpp)
target_link_libraries(skyapo-pipewire-lifetime PRIVATE PkgConfig::PIPEWIRE)

configure_file(packaging/skyapod.service.in
  ${CMAKE_CURRENT_BINARY_DIR}/skyapod.service @ONLY)
install(TARGETS skyapo skyapod skyapo-render
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/skyapod.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
install(FILES README.md ROADMAP.md CHANGELOG.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES upstream/equalizerapo/License.txt
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
  RENAME EqualizerAPO-License.txt)
