cmake_minimum_required(VERSION 3.20)
project(SkyAPO VERSION 0.9.0 LANGUAGES C CXX)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SKYAPO_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(SKYAPO_BUILD_UI "Build the Qt configuration editor when Qt 6 is available" ON)
option(SKYAPO_ENABLE_FFTW3F "Enable upstream GraphicEQ and convolution through FFTW3f" ON)
option(SKYAPO_ENABLE_MUPARSER "Enable basic EAPO conditional expressions when muParser is available" ON)
option(SKYAPO_ENABLE_MUPARSERX "Fetch and use Equalizer APO's legacy MuParserX 3.0.1 parser" ON)
option(SKYAPO_BUILD_VST2_PROTOTYPE_TESTS
  "Build tests for the experimental FST-based VST2-compatible host" OFF)
option(SKYAPO_ENABLE_FST_VST2_HOST
  "Enable the experimental in-process FST-based legacy plugin host" OFF)
option(SKYAPO_ENABLE_PIPEWIRE_E2E_TESTS
  "Run an isolated PipeWire server and verify captured SkyAPO DSP output" OFF)
option(SKYAPO_ENABLE_CPACK "Enable DEB and RPM generators for CPack" ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
if(SKYAPO_ENABLE_FFTW3F)
  pkg_check_modules(FFTW3F QUIET IMPORTED_TARGET fftw3f)
else()
  set(FFTW3F_FOUND FALSE)
  message(STATUS "FFTW3f explicitly disabled: GraphicEQ/Convolution support is off")
endif()
pkg_check_modules(LILV QUIET IMPORTED_TARGET lilv-0)
pkg_check_modules(LV2 QUIET IMPORTED_TARGET lv2)
if(SKYAPO_ENABLE_MUPARSER)
  pkg_check_modules(MUPARSER QUIET IMPORTED_TARGET muparser)
else()
  set(MUPARSER_FOUND FALSE)
endif()

if(SKYAPO_ENABLE_MUPARSERX)
  set(SKYAPO_MUPARSERX_SHA256 "11689042843844638414af070f928ef5200461dc59fa237bad4a01531f982609")
  set(SKYAPO_MUPARSERX_ARCHIVE
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/muparserx_v3_0_1.zip"
      CACHE FILEPATH
      "Path to the pinned MuParserX 3.0.1 archive (empty downloads it from SourceForge)")
  if(SKYAPO_MUPARSERX_ARCHIVE)
    file(SHA256 "${SKYAPO_MUPARSERX_ARCHIVE}" SKYAPO_MUPARSERX_ARCHIVE_SHA256)
    if(NOT SKYAPO_MUPARSERX_ARCHIVE_SHA256 STREQUAL SKYAPO_MUPARSERX_SHA256)
      message(FATAL_ERROR "MuParserX archive SHA-256 mismatch: ${SKYAPO_MUPARSERX_ARCHIVE_SHA256}")
    endif()
    set(SKYAPO_MUPARSERX_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/skyapo_muparserx_301-src")
    if(NOT EXISTS "${SKYAPO_MUPARSERX_EXTRACT_DIR}/parser/mpParser.h" AND
       NOT EXISTS "${SKYAPO_MUPARSERX_EXTRACT_DIR}/muparserx_v3_0_1/parser/mpParser.h")
      file(MAKE_DIRECTORY "${SKYAPO_MUPARSERX_EXTRACT_DIR}")
      execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xvf "${SKYAPO_MUPARSERX_ARCHIVE}"
        WORKING_DIRECTORY "${SKYAPO_MUPARSERX_EXTRACT_DIR}"
        RESULT_VARIABLE SKYAPO_MUPARSERX_EXTRACT_RESULT
        OUTPUT_QUIET ERROR_VARIABLE SKYAPO_MUPARSERX_EXTRACT_ERROR)
      if(NOT SKYAPO_MUPARSERX_EXTRACT_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not extract MuParserX archive: ${SKYAPO_MUPARSERX_EXTRACT_ERROR}")
      endif()
    endif()
    if(EXISTS "${SKYAPO_MUPARSERX_EXTRACT_DIR}/parser/mpParser.h")
      set(SKYAPO_MUPARSERX_ROOT "${SKYAPO_MUPARSERX_EXTRACT_DIR}")
    else()
      set(SKYAPO_MUPARSERX_ROOT "${SKYAPO_MUPARSERX_EXTRACT_DIR}/muparserx_v3_0_1")
    endif()
  else()
    include(FetchContent)
    FetchContent_Declare(skyapo_muparserx_301
      URL https://sourceforge.net/p/equalizerapo/wiki/Developer%20documentation/attachment/muparserx_v3_0_1.zip
      URL_HASH SHA256=${SKYAPO_MUPARSERX_SHA256})
    FetchContent_MakeAvailable(skyapo_muparserx_301)
    set(SKYAPO_MUPARSERX_ROOT "${skyapo_muparserx_301_SOURCE_DIR}")
  endif()
  if(NOT EXISTS "${SKYAPO_MUPARSERX_ROOT}/License.txt" OR
     NOT EXISTS "${SKYAPO_MUPARSERX_ROOT}/parser/mpParser.h")
    message(FATAL_ERROR "MuParserX 3.0.1 archive has unexpected contents")
  endif()
  file(GLOB SKYAPO_MUPARSERX_SOURCES CONFIGURE_DEPENDS
    "${SKYAPO_MUPARSERX_ROOT}/parser/*.cpp")
  add_library(skyapo-muparserx STATIC ${SKYAPO_MUPARSERX_SOURCES})
  target_include_directories(skyapo-muparserx SYSTEM PUBLIC "${SKYAPO_MUPARSERX_ROOT}/parser")
  target_compile_features(skyapo-muparserx PUBLIC cxx_std_17)
  target_compile_definitions(skyapo-muparserx PRIVATE _UNICODE)
  target_compile_options(skyapo-muparserx PRIVATE
    -fext-numeric-literals -Wno-deprecated-declarations
    -Wno-deprecated-enum-enum-conversion -Wno-unknown-pragmas)
  set(SKYAPO_HAVE_MUPARSERX TRUE)
else()
  message(STATUS "MuParserX 3.0.1 disabled; upstream expression syntax is unavailable")
endif()

set(SKYAPO_VST3_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/upstream/vst3)
if(EXISTS "${SKYAPO_VST3_ROOT}/base/source/baseiids.cpp" AND
   EXISTS "${SKYAPO_VST3_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp")
  add_library(skyapo-vst3-base STATIC
    ${SKYAPO_VST3_ROOT}/base/source/baseiids.cpp
    ${SKYAPO_VST3_ROOT}/base/source/fbuffer.cpp
    ${SKYAPO_VST3_ROOT}/base/source/fdebug.cpp
    ${SKYAPO_VST3_ROOT}/base/source/fdynlib.cpp
    ${SKYAPO_VST3_ROOT}/base/source/fobject.cpp
    ${SKYAPO_VST3_ROOT}/base/source/fstreamer.cpp
    ${SKYAPO_VST3_ROOT}/base/source/fstring.cpp
    ${SKYAPO_VST3_ROOT}/base/source/timer.cpp
    ${SKYAPO_VST3_ROOT}/base/source/updatehandler.cpp
    ${SKYAPO_VST3_ROOT}/base/thread/source/fcondition.cpp
    ${SKYAPO_VST3_ROOT}/base/thread/source/flock.cpp)
  add_library(skyapo-vst3-interfaces STATIC
    ${SKYAPO_VST3_ROOT}/pluginterfaces/base/conststringtable.cpp
    ${SKYAPO_VST3_ROOT}/pluginterfaces/base/coreiids.cpp
    ${SKYAPO_VST3_ROOT}/pluginterfaces/base/funknown.cpp
    ${SKYAPO_VST3_ROOT}/pluginterfaces/base/ustring.cpp)
  add_library(skyapo-vst3-hosting STATIC
    ${SKYAPO_VST3_ROOT}/public.sdk/source/common/commonstringconvert.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/common/memorystream.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/common/threadchecker_linux.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/hosting/module.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/vstinitiids.cpp)
  foreach(target skyapo-vst3-base skyapo-vst3-interfaces skyapo-vst3-hosting)
    target_include_directories(${target} PUBLIC ${SKYAPO_VST3_ROOT})
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_definitions(${target} PRIVATE DEVELOPMENT=1)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endforeach()
  target_link_libraries(skyapo-vst3-hosting PUBLIC skyapo-vst3-base
    skyapo-vst3-interfaces ${CMAKE_DL_LIBS} Threads::Threads)
  set(SKYAPO_HAVE_VST3 TRUE)
else()
  message(STATUS "Steinberg VST3 SDK submodules missing: native VST3 hosting disabled")
endif()

if(SKYAPO_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

set(EAPO ${CMAKE_CURRENT_SOURCE_DIR}/upstream/equalizerapo)
execute_process(COMMAND git -C ${EAPO} rev-parse HEAD
  OUTPUT_VARIABLE SKYAPO_UPSTREAM_REVISION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE SKYAPO_UPSTREAM_GIT_RESULT)
if(NOT SKYAPO_UPSTREAM_GIT_RESULT EQUAL 0)
  set(SKYAPO_UPSTREAM_REVISION unknown)
endif()
include(cmake/PortableEapo.cmake)
set(SKYAPO_UPSTREAM_PROCESS_SOURCE
    ${EAPO_PORT}/FilterEngineProcess.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux/generate_upstream_filter_engine_process.py
  ${EAPO}/FilterEngine.cpp)
execute_process(
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux/generate_upstream_filter_engine_process.py
          ${EAPO}/FilterEngine.cpp ${SKYAPO_UPSTREAM_PROCESS_SOURCE}
  RESULT_VARIABLE SKYAPO_PROCESS_GENERATOR_RESULT
  ERROR_VARIABLE SKYAPO_PROCESS_GENERATOR_ERROR)
if(NOT SKYAPO_PROCESS_GENERATOR_RESULT EQUAL 0)
  message(FATAL_ERROR "Could not generate upstream FilterEngine process slice: ${SKYAPO_PROCESS_GENERATOR_ERROR}")
endif()
add_library(skyapo-eapo-dsp STATIC
  ${EAPO_SOURCES}
  src/platform/linux/compat.cpp
  src/platform/linux/ChannelHelper.cpp
  src/platform/linux/MemoryHelper.cpp
)
target_include_directories(skyapo-eapo-dsp BEFORE PRIVATE
  src/platform/linux src/core ${EAPO_PORT} ${EAPO})
target_compile_definitions(skyapo-eapo-dsp PRIVATE __forceinline=inline)
target_compile_options(skyapo-eapo-dsp PRIVATE -Wno-write-strings)
target_link_libraries(skyapo-eapo-dsp PUBLIC Threads::Threads)
if(TARGET PkgConfig::SNDFILE)
  target_link_libraries(skyapo-eapo-dsp PRIVATE PkgConfig::SNDFILE)
endif()
if(FFTW3F_FOUND)
  target_link_libraries(skyapo-eapo-dsp PRIVATE PkgConfig::FFTW3F)
endif()

add_library(skyapo-core STATIC src/core/Engine.cpp
  src/core/DevicePatternMatcher.cpp
  src/platform/linux/ConfigSource.cpp
  ${SKYAPO_UPSTREAM_PROCESS_SOURCE} ${EAPO_EXPRESSION_SOURCES})
target_include_directories(skyapo-core BEFORE PRIVATE src/platform/linux)
target_include_directories(skyapo-core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin src/core ${EAPO_PORT}
         ${EAPO_PORT}/filters ${EAPO_PORT}/parser ${EAPO}/filters ${EAPO}
         ${EAPO}/helpers)
target_compile_definitions(skyapo-core PRIVATE __forceinline=inline)
target_compile_definitions(skyapo-core PRIVATE SKYAPO_VERSION="${PROJECT_VERSION}")
target_link_libraries(skyapo-core PUBLIC skyapo-eapo-dsp)
if(SKYAPO_HAVE_MUPARSERX)
  target_link_libraries(skyapo-core PUBLIC skyapo-muparserx)
  target_compile_definitions(skyapo-core PRIVATE SKYAPO_HAVE_MUPARSERX=1 _UNICODE)
endif()

if(MUPARSER_FOUND)
  target_link_libraries(skyapo-core PUBLIC PkgConfig::MUPARSER)
  target_compile_definitions(skyapo-core PRIVATE SKYAPO_HAVE_MUPARSER=1)
else()
  message(STATUS "muParser not found: EAPO conditional directives disabled")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/clap/include/clap/clap.h")
  target_sources(skyapo-core PRIVATE src/plugin/CLAPPluginHost.cpp)
  target_include_directories(skyapo-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/clap/include)
  target_link_libraries(skyapo-core PUBLIC ${CMAKE_DL_LIBS})
  target_compile_definitions(skyapo-core PUBLIC SKYAPO_HAVE_CLAP=1)
else()
  message(STATUS "CLAP headers not present: initialize upstream/clap to enable CLAP")
endif()
if(SKYAPO_ENABLE_FST_VST2_HOST)
  set(SKYAPO_FST_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/upstream/fst/fst")
  if(NOT EXISTS "${SKYAPO_FST_INCLUDE}/fst.h")
    message(FATAL_ERROR
      "SKYAPO_ENABLE_FST_VST2_HOST requires the upstream/fst submodule")
  endif()
  target_sources(skyapo-core PRIVATE src/plugin/VST2PluginHost.cpp
    src/plugin/VST2PluginFilter.cpp)
  target_include_directories(skyapo-core SYSTEM PRIVATE "${SKYAPO_FST_INCLUDE}")
  target_link_libraries(skyapo-core PUBLIC ${CMAKE_DL_LIBS})
  target_compile_definitions(skyapo-core PUBLIC SKYAPO_HAVE_FST_VST2=1)
endif()
if(SKYAPO_HAVE_VST3)
  target_sources(skyapo-core PRIVATE src/plugin/VST3PluginHost.cpp)
  target_include_directories(skyapo-core PRIVATE ${SKYAPO_VST3_ROOT})
  target_link_libraries(skyapo-core PUBLIC skyapo-vst3-hosting)
  target_compile_definitions(skyapo-core PUBLIC SKYAPO_HAVE_VST3=1)

  # Self-contained, redistributable test bundle for repeatable numerical host
  # tests. Its code is SkyAPO-authored and depends on the pinned SDK interface.
  set(SKYAPO_TEST_VST3_BUNDLE
      "${CMAKE_CURRENT_BINARY_DIR}/test-plugins/skyapo-test-gain.vst3")
  set(SKYAPO_TEST_VST3_ARCH "${SKYAPO_TEST_VST3_BUNDLE}/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux")
  file(MAKE_DIRECTORY "${SKYAPO_TEST_VST3_ARCH}")
  add_library(skyapo-test-vst3 MODULE
    tests/plugins/vst3_gain.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/main/pluginfactory.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/main/moduleinit.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/main/linuxmain.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/common/commoniids.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/common/pluginview.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/vstaudioeffect.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/vstparameters.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/vstbus.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/vstcomponent.cpp
    ${SKYAPO_VST3_ROOT}/public.sdk/source/vst/vstcomponentbase.cpp)
  target_include_directories(skyapo-test-vst3 PRIVATE ${SKYAPO_VST3_ROOT})
  target_compile_features(skyapo-test-vst3 PRIVATE cxx_std_17)
  target_compile_definitions(skyapo-test-vst3 PRIVATE DEVELOPMENT=1)
  target_link_libraries(skyapo-test-vst3 PRIVATE skyapo-vst3-hosting)
  set_target_properties(skyapo-test-vst3 PROPERTIES PREFIX "" OUTPUT_NAME "skyapo-test-gain"
    LIBRARY_OUTPUT_DIRECTORY "${SKYAPO_TEST_VST3_ARCH}")
endif()
if(LILV_FOUND)
  target_sources(skyapo-core PRIVATE src/plugin/LV2PluginHost.cpp)
  target_link_libraries(skyapo-core PUBLIC PkgConfig::LILV)
  target_compile_definitions(skyapo-core PUBLIC SKYAPO_HAVE_LV2=1)
else()
  message(STATUS "Lilv not found: native LV2 hosting disabled")
endif()
if(FFTW3F_FOUND)
  target_sources(skyapo-core PRIVATE src/platform/linux/ConvolutionFilterLinux.cpp)
  target_compile_definitions(skyapo-core PRIVATE SKYAPO_HAVE_CONVOLUTION=1)
  target_compile_definitions(skyapo-core PUBLIC SKYAPO_HAVE_GRAPHIC_EQ=1)
else()
  message(STATUS "FFTW3f not found: GraphicEQ/Convolution directives disabled")
endif()

add_executable(skyapo-render src/render/main.cpp)
target_link_libraries(skyapo-render PRIVATE skyapo-core PkgConfig::SNDFILE)

add_executable(skyapo-bench src/benchmark/main.cpp)
target_link_libraries(skyapo-bench PRIVATE skyapo-core)

add_executable(skyapo src/cli/main.cpp)
target_compile_definitions(skyapo PRIVATE
  SKYAPO_VERSION="${PROJECT_VERSION}"
  SKYAPO_UPSTREAM_REVISION="${SKYAPO_UPSTREAM_REVISION}")
target_link_libraries(skyapo PRIVATE skyapo-core)
if(PIPEWIRE_FOUND)
  add_library(skyapo-pipewire STATIC
    src/pipewire/DeviceManager.cpp
    src/pipewire/DefaultSinkVolumeMonitor.cpp
    src/pipewire/Runtime.cpp
    src/platform/ConfigWatcher.cpp
    src/platform/RealtimeAudit.cpp)
  target_link_libraries(skyapo-pipewire PUBLIC skyapo-core PkgConfig::PIPEWIRE)
  target_include_directories(skyapo-pipewire PRIVATE src/platform/linux)
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
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME skyapo-desktop-metadata
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop_metadata_test.py
      ${CMAKE_CURRENT_SOURCE_DIR}/packaging/skyapo.desktop
      ${CMAKE_CURRENT_SOURCE_DIR}/packaging/skyapo.svg
      ${CMAKE_CURRENT_SOURCE_DIR}/packaging/org.skyapo.SkyAPO.metainfo.xml)
  add_test(NAME skyapo-diagnostics-json
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics_json_test.py
      $<TARGET_FILE:skyapo>)
  add_test(NAME skyapo-config-check-json
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_check_json_test.py
      $<TARGET_FILE:skyapo>)
  add_test(NAME skyapo-upstream-config-compatibility
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/upstream_config_compatibility_test.py
      $<TARGET_FILE:skyapo> ${EAPO})
  add_test(NAME skyapo-cli-basics
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_basics_test.py
      $<TARGET_FILE:skyapo>)
  add_test(NAME skyapo-ipc-cli
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/ipc_cli_test.py
      $<TARGET_FILE:skyapo>)
  add_test(NAME skyapo-benchmark
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_test.py
      $<TARGET_FILE:skyapo-bench>)
  if(FFTW3F_FOUND)
    add_test(NAME skyapo-benchmark-convolution
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_test.py
        $<TARGET_FILE:skyapo-bench> --convolution)
  endif()
endif()
add_executable(skyapo-ipc-protocol-tests tests/ipc_protocol_test.cpp)
add_test(NAME skyapo-ipc-protocol COMMAND skyapo-ipc-protocol-tests)
add_executable(skyapo-config-watcher-tests
  tests/config_watcher_test.cpp src/platform/ConfigWatcher.cpp
  src/platform/linux/ConfigSource.cpp)
target_include_directories(skyapo-config-watcher-tests PRIVATE
  src/platform/linux)
add_test(NAME skyapo-config-watcher COMMAND skyapo-config-watcher-tests)
add_test(NAME skyapo-default-sink-volume
  COMMAND skyapo-default-sink-volume-tests)
if(Python3_Interpreter_FOUND)
  add_test(NAME skyapo-install-license-notices
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_license_notices_test.py
      ${CMAKE_COMMAND} ${CMAKE_CURRENT_BINARY_DIR})
endif()
if(SKYAPO_BUILD_UI)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(Qt6Widgets_FOUND)
    # Qt Widgets can be used under LGPL-3.0 or GPL-2.0. SkyAPO keeps the
    # upstream GPL-2.0-or-later application license and dynamically links Qt;
    # ship the LGPL notice text with installed builds.
    find_file(SKYAPO_QT_LGPL3_LICENSE
      NAMES LGPL-3.0-only.txt LGPL-3.0.txt LGPL3.txt LGPL3 LGPL-3
      PATH_SUFFIXES share/licenses/qt6-base share/licenses/qt6
        share/licenses/spdx share/licenses/licenses share/licenses
        share/common-licenses)
    if(NOT SKYAPO_QT_LGPL3_LICENSE)
      message(FATAL_ERROR
        "Qt 6 Widgets is enabled but its LGPL-3.0 license text was not found. "
        "Set SKYAPO_QT_LGPL3_LICENSE to the license file from the Qt package.")
    endif()
    set(SKYAPO_EDITOR ${EAPO}/Editor)
    # The upstream row includes "FilterTable.h" relative to its source tree.
    # Build a copy beside the row with a narrow Linux adapter instead of
    # pulling the Windows APO/Registry-bound full FilterTable into this target.
    file(MAKE_DIRECTORY ${EAPO_PORT}/Editor)
    configure_file(${SKYAPO_EDITOR}/FilterTableRow.cpp
      ${EAPO_PORT}/Editor/FilterTableRow.cpp COPYONLY)
    configure_file(${SKYAPO_EDITOR}/FilterTableRow.h
      ${EAPO_PORT}/Editor/FilterTableRow.h COPYONLY)
    configure_file(${SKYAPO_EDITOR}/FilterTableRow.ui
      ${EAPO_PORT}/Editor/FilterTableRow.ui COPYONLY)
    configure_file(src/ui/upstream/FilterTable.h
      ${EAPO_PORT}/Editor/FilterTable.h COPYONLY)
    add_library(skyapo-eapo-editor STATIC
      ${EAPO_PORT}/Editor/FilterTableRow.cpp
      ${EAPO_PORT}/Editor/FilterTableRow.h
      ${EAPO_PORT}/Editor/FilterTableRow.ui
      ${EAPO_PORT}/Editor/FilterTable.h
      src/ui/resources/skyapo_editor.qrc
      ${SKYAPO_EDITOR}/widgets/CompactToolBar.cpp
      ${SKYAPO_EDITOR}/widgets/CompactToolBar.h
      ${SKYAPO_EDITOR}/widgets/EscapableLineEdit.cpp
      ${SKYAPO_EDITOR}/widgets/EscapableLineEdit.h
      ${SKYAPO_EDITOR}/IFilterGUI.cpp
      ${SKYAPO_EDITOR}/IFilterGUI.h
      ${SKYAPO_EDITOR}/IFilterGUIFactory.cpp
      ${SKYAPO_EDITOR}/IFilterGUIFactory.h
      ${SKYAPO_EDITOR}/FilterTemplate.cpp
      ${SKYAPO_EDITOR}/FilterTemplate.h
      ${SKYAPO_EDITOR}/helpers/GUIHelper.cpp
      ${SKYAPO_EDITOR}/helpers/GUIHelper.h
      ${SKYAPO_EDITOR}/guis/PreampFilterGUI.cpp
      ${SKYAPO_EDITOR}/guis/PreampFilterGUI.h
      ${SKYAPO_EDITOR}/guis/PreampFilterGUI.ui
      ${SKYAPO_EDITOR}/guis/PreampFilterGUIFactory.cpp
      ${SKYAPO_EDITOR}/guis/PreampFilterGUIFactory.h
      ${SKYAPO_EDITOR}/guis/BiQuadFilterGUI.cpp
      ${SKYAPO_EDITOR}/guis/BiQuadFilterGUI.h
      ${SKYAPO_EDITOR}/guis/BiQuadFilterGUI.ui
      ${SKYAPO_EDITOR}/guis/BiQuadFilterGUIFactory.cpp
      ${SKYAPO_EDITOR}/guis/BiQuadFilterGUIFactory.h
      ${SKYAPO_EDITOR}/guis/DelayFilterGUI.cpp
      ${SKYAPO_EDITOR}/guis/DelayFilterGUI.h
      ${SKYAPO_EDITOR}/guis/DelayFilterGUI.ui
      ${SKYAPO_EDITOR}/guis/DelayFilterGUIFactory.cpp
      ${SKYAPO_EDITOR}/guis/DelayFilterGUIFactory.h
      ${SKYAPO_EDITOR}/guis/StageFilterGUI.cpp
      ${SKYAPO_EDITOR}/guis/StageFilterGUI.h
      ${SKYAPO_EDITOR}/guis/StageFilterGUI.ui
      ${SKYAPO_EDITOR}/guis/StageFilterGUIFactory.cpp
      ${SKYAPO_EDITOR}/guis/StageFilterGUIFactory.h
    )
    if(FFTW3F_FOUND)
      target_sources(skyapo-eapo-editor PRIVATE src/ui/ResponseAnalysis.cpp
        src/ui/ResponseAnalysis.h src/ui/resources/skyapo_graphiceq.qrc)
      target_link_libraries(skyapo-eapo-editor PRIVATE PkgConfig::FFTW3F)
      target_compile_definitions(skyapo-eapo-editor PUBLIC
        SKYAPO_UI_HAVE_RESPONSE_ANALYSIS=1)
    endif()
    set_target_properties(skyapo-eapo-editor PROPERTIES
      AUTOMOC ON AUTOUIC ON AUTORCC ON)
    set_property(TARGET skyapo-eapo-editor PROPERTY AUTOUIC_SEARCH_PATHS
      "${EAPO_PORT}/Editor;${SKYAPO_EDITOR};${SKYAPO_EDITOR}/guis;${SKYAPO_EDITOR}/widgets")
    target_include_directories(skyapo-eapo-editor BEFORE PUBLIC ${EAPO_PORT} ${EAPO})
    if(FFTW3F_FOUND)
      target_include_directories(skyapo-eapo-editor PRIVATE
        ${SKYAPO_EDITOR}/guis ${SKYAPO_EDITOR}/widgets)
    endif()
    # The upstream editor's legacy stable.h supplied these Qt declarations
    # globally; provide only the declarations needed by the selected sources.
    target_precompile_headers(skyapo-eapo-editor PRIVATE
      <QString> <QStringList> <QMetaType> <QDateTime>
      # The upstream GraphicEQ GUI expects DBL_MAX from the removed Windows
      # stable.h umbrella header.
      <cfloat>)
    target_compile_definitions(skyapo-eapo-editor PRIVATE __forceinline=inline)
    target_link_libraries(skyapo-eapo-editor PUBLIC skyapo-core Qt6::Widgets)
    if(FFTW3F_FOUND)
      target_sources(skyapo-eapo-editor PRIVATE
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUI.cpp
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUI.h
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUI.ui
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUIFactory.cpp
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUIFactory.h
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUIItem.cpp
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUIItem.h
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUIScene.cpp
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUIScene.h
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUIView.cpp
        ${SKYAPO_EDITOR}/guis/GraphicEQFilterGUIView.h
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotHRuler.cpp
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotHRuler.h
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotItem.cpp
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotItem.h
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotScene.cpp
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotScene.h
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotView.cpp
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotView.h
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotVRuler.cpp
        ${SKYAPO_EDITOR}/widgets/FrequencyPlotVRuler.h
        ${SKYAPO_EDITOR}/widgets/ResizeCorner.cpp
        ${SKYAPO_EDITOR}/widgets/ResizeCorner.h)
    endif()

    add_executable(skyapo-ui src/ui/main.cpp src/ui/MainWindow.cpp
      src/ui/MainWindow.h src/ui/ConfigFile.cpp src/ui/ConfigFile.h
      src/ui/IncludeEditor.cpp src/ui/IncludeEditor.h
      src/ui/ConvolutionEditor.cpp src/ui/ConvolutionEditor.h
      src/ui/ChannelCopyEditor.cpp src/ui/ChannelCopyEditor.h
      src/ui/IIRFilterEditor.cpp src/ui/IIRFilterEditor.h
      src/ui/ResponseAnalysis.h)
    set_target_properties(skyapo-ui PROPERTIES AUTOMOC ON)
    target_include_directories(skyapo-ui PRIVATE ${SKYAPO_EDITOR})
    target_compile_definitions(skyapo-ui PRIVATE __forceinline=inline)
    target_link_libraries(skyapo-ui PRIVATE skyapo-eapo-editor)

    add_executable(skyapo-ui-test-cli tests/ui_cli_stub.cpp)
    add_executable(skyapo-ui-tests tests/ui_tests.cpp src/ui/MainWindow.cpp
      src/ui/MainWindow.h src/ui/ConfigFile.cpp src/ui/IncludeEditor.cpp
      src/ui/IncludeEditor.h src/ui/ConvolutionEditor.cpp
      src/ui/ConvolutionEditor.h src/ui/ChannelCopyEditor.cpp
      src/ui/ChannelCopyEditor.h src/ui/IIRFilterEditor.cpp
      src/ui/IIRFilterEditor.h src/ui/ResponseAnalysis.h)
    set_target_properties(skyapo-ui-tests PROPERTIES AUTOMOC ON)
    target_include_directories(skyapo-ui-tests PRIVATE src/ui)
    target_include_directories(skyapo-ui-tests PRIVATE ${SKYAPO_EDITOR})
    target_compile_definitions(skyapo-ui-tests PRIVATE __forceinline=inline)
    if(FFTW3F_FOUND)
      target_compile_definitions(skyapo-ui-tests PRIVATE
        SKYAPO_UI_HAVE_CONVOLUTION_VALIDATION=1)
    endif()
    target_link_libraries(skyapo-ui-tests PRIVATE skyapo-eapo-editor)
    add_test(NAME skyapo-ui-editor COMMAND skyapo-ui-tests
      $<TARGET_FILE:skyapo-ui-test-cli> $<TARGET_FILE:skyapo>)
    if(Python3_Interpreter_FOUND)
      add_test(NAME skyapo-ui-resource-audit
        COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_resource_audit_test.py
          ${CMAKE_CURRENT_SOURCE_DIR} ${FFTW3F_FOUND})
    endif()
    set_tests_properties(skyapo-ui-editor PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    install(TARGETS skyapo-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/skyapo.desktop
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES packaging/skyapo.svg
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
    install(FILES packaging/org.skyapo.SkyAPO.metainfo.xml
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
  else()
    message(STATUS "Qt 6 Widgets not found: skyapo-ui target disabled")
  endif()
endif()

add_executable(skyapo-tests tests/core_tests.cpp)
target_include_directories(skyapo-tests PRIVATE src/platform/linux)
target_link_libraries(skyapo-tests PRIVATE skyapo-core PkgConfig::SNDFILE)
target_compile_definitions(skyapo-tests PRIVATE __forceinline=inline
  SKYAPO_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(MUPARSER_FOUND OR SKYAPO_HAVE_MUPARSERX)
  target_compile_definitions(skyapo-tests PRIVATE SKYAPO_TEST_MUPARSER=1)
endif()
if(SKYAPO_HAVE_MUPARSERX)
  target_compile_definitions(skyapo-tests PRIVATE SKYAPO_TEST_MUPARSERX=1)
endif()
if(FFTW3F_FOUND)
  target_compile_definitions(skyapo-tests PRIVATE SKYAPO_TEST_CONVOLUTION=1)
endif()
add_test(NAME skyapo-core COMMAND skyapo-tests)

add_executable(skyapo-config-source-tests
  tests/config_source_tests.cpp src/platform/linux/ConfigSource.cpp)
add_test(NAME skyapo-config-source COMMAND skyapo-config-source-tests)

add_executable(skyapo-upstream-process-tests
  tests/upstream_filter_engine_process_test.cpp)
target_include_directories(skyapo-upstream-process-tests PRIVATE
  src/platform/linux ${EAPO_PORT} ${EAPO})
target_link_libraries(skyapo-upstream-process-tests PRIVATE skyapo-core)
add_test(NAME skyapo-upstream-process COMMAND skyapo-upstream-process-tests)
if(SKYAPO_ENABLE_FST_VST2_HOST)
  add_library(skyapo-test-vst2-plugin SHARED
    tests/plugins/vst2_gain_fixture.c)
  target_include_directories(skyapo-test-vst2-plugin SYSTEM PRIVATE
    "${SKYAPO_FST_INCLUDE}")
  add_library(skyapo-test-vst2-mono-plugin SHARED
    tests/plugins/vst2_gain_fixture.c)
  add_library(skyapo-test-vst2-dynamic-latency-plugin SHARED
    tests/plugins/vst2_gain_fixture.c)
  target_include_directories(skyapo-test-vst2-mono-plugin SYSTEM PRIVATE
    "${SKYAPO_FST_INCLUDE}")
  target_include_directories(skyapo-test-vst2-dynamic-latency-plugin SYSTEM PRIVATE
    "${SKYAPO_FST_INCLUDE}")
  target_compile_definitions(skyapo-test-vst2-dynamic-latency-plugin PRIVATE
    SKYAPO_VST2_FIXTURE_DYNAMIC_LATENCY=1)
  target_compile_definitions(skyapo-test-vst2-mono-plugin PRIVATE
    SKYAPO_VST2_FIXTURE_MONO=1 SKYAPO_VST2_FIXTURE_NO_CHUNKS=1)
  target_compile_definitions(skyapo-tests PRIVATE SKYAPO_TEST_VST2=1
    SKYAPO_TEST_VST2_PATH="$<TARGET_FILE:skyapo-test-vst2-plugin>"
    SKYAPO_TEST_VST2_MONO_PATH="$<TARGET_FILE:skyapo-test-vst2-mono-plugin>"
    SKYAPO_TEST_VST2_DYNAMIC_LATENCY_PATH="$<TARGET_FILE:skyapo-test-vst2-dynamic-latency-plugin>")
  add_dependencies(skyapo-tests skyapo-test-vst2-plugin
    skyapo-test-vst2-mono-plugin skyapo-test-vst2-dynamic-latency-plugin)
endif()
if(SKYAPO_BUILD_VST2_PROTOTYPE_TESTS)
  set(SKYAPO_FST_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/upstream/fst/fst")
  if(NOT EXISTS "${SKYAPO_FST_INCLUDE}/fst.h")
    message(FATAL_ERROR
      "SKYAPO_BUILD_VST2_PROTOTYPE_TESTS requires the upstream/fst submodule")
  endif()
  if(NOT TARGET skyapo-test-vst2-plugin)
    add_library(skyapo-test-vst2-plugin SHARED
      tests/plugins/vst2_gain_fixture.c)
    target_include_directories(skyapo-test-vst2-plugin SYSTEM PRIVATE
      "${SKYAPO_FST_INCLUDE}")
  endif()
  add_executable(skyapo-vst2-host-prototype-tests
    src/plugin/VST2PluginHost.cpp tests/vst2_host_prototype_test.cpp)
  target_include_directories(skyapo-vst2-host-prototype-tests PRIVATE src/plugin)
  target_include_directories(skyapo-vst2-host-prototype-tests SYSTEM PRIVATE
    "${SKYAPO_FST_INCLUDE}")
  target_link_libraries(skyapo-vst2-host-prototype-tests PRIVATE
    ${CMAKE_DL_LIBS})
  add_dependencies(skyapo-vst2-host-prototype-tests skyapo-test-vst2-plugin)
  add_test(NAME skyapo-vst2-host-prototype
    COMMAND skyapo-vst2-host-prototype-tests
      "$<TARGET_FILE:skyapo-test-vst2-plugin>")
endif()
add_executable(skyapo-realtime-safety-tests tests/realtime_safety_tests.cpp src/platform/RealtimeAudit.cpp)
target_link_libraries(skyapo-realtime-safety-tests PRIVATE skyapo-core)
if(SKYAPO_ENABLE_FST_VST2_HOST)
  target_compile_definitions(skyapo-realtime-safety-tests PRIVATE
    SKYAPO_TEST_VST2=1
    SKYAPO_TEST_VST2_PATH="$<TARGET_FILE:skyapo-test-vst2-plugin>"
    SKYAPO_TEST_VST2_MONO_PATH="$<TARGET_FILE:skyapo-test-vst2-mono-plugin>"
    SKYAPO_TEST_VST2_DYNAMIC_LATENCY_PATH="$<TARGET_FILE:skyapo-test-vst2-dynamic-latency-plugin>")
  add_dependencies(skyapo-realtime-safety-tests skyapo-test-vst2-plugin
    skyapo-test-vst2-mono-plugin skyapo-test-vst2-dynamic-latency-plugin)
endif()
if(FFTW3F_FOUND)
  target_compile_definitions(skyapo-realtime-safety-tests PRIVATE SKYAPO_TEST_CONVOLUTION=1)
  target_link_libraries(skyapo-realtime-safety-tests PRIVATE PkgConfig::SNDFILE)
endif()
target_link_options(skyapo-realtime-safety-tests PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign)
add_test(NAME skyapo-realtime-safety COMMAND skyapo-realtime-safety-tests)
add_executable(skyapo-node-id-parser-tests tests/node_id_parser_tests.cpp)
target_include_directories(skyapo-node-id-parser-tests PRIVATE src/pipewire)
add_test(NAME skyapo-node-id-parser COMMAND skyapo-node-id-parser-tests)
if(PIPEWIRE_FOUND)
  add_executable(skyapo-device-identity-tests tests/device_identity_tests.cpp)
  target_link_libraries(skyapo-device-identity-tests PRIVATE PkgConfig::PIPEWIRE)
  add_test(NAME skyapo-device-identity COMMAND skyapo-device-identity-tests)
endif()
if(LILV_FOUND AND LV2_FOUND)
  set(SKYAPO_TEST_LV2_PATH "${CMAKE_CURRENT_BINARY_DIR}/test-plugins/lv2")
  set(SKYAPO_TEST_LV2_DIR "${SKYAPO_TEST_LV2_PATH}/skyapo-test.lv2")
  set(SKYAPO_TEST_LV2_LATENCY_DIR "${SKYAPO_TEST_LV2_PATH}/skyapo-lv2-latency.lv2")
  add_library(skyapo-test-lv2 MODULE tests/plugins/test_gain.c)
  target_link_libraries(skyapo-test-lv2 PRIVATE PkgConfig::LV2)
  set_target_properties(skyapo-test-lv2 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "skyapo_test_gain"
    LIBRARY_OUTPUT_DIRECTORY "${SKYAPO_TEST_LV2_DIR}")
  configure_file(tests/plugins/skyapo-test.lv2/manifest.ttl
    "${SKYAPO_TEST_LV2_DIR}/manifest.ttl" COPYONLY)
  configure_file(tests/plugins/skyapo-test.lv2/plugin.ttl
    "${SKYAPO_TEST_LV2_DIR}/plugin.ttl" COPYONLY)
  add_library(skyapo-test-lv2-latency MODULE tests/plugins/test_latency_lv2.c)
  target_link_libraries(skyapo-test-lv2-latency PRIVATE PkgConfig::LV2)
  set_target_properties(skyapo-test-lv2-latency PROPERTIES
    PREFIX ""
    OUTPUT_NAME "skyapo_test_latency"
    LIBRARY_OUTPUT_DIRECTORY "${SKYAPO_TEST_LV2_LATENCY_DIR}")
  configure_file(tests/plugins/skyapo-lv2-latency.lv2/manifest.ttl
    "${SKYAPO_TEST_LV2_LATENCY_DIR}/manifest.ttl" COPYONLY)
  configure_file(tests/plugins/skyapo-lv2-latency.lv2/plugin.ttl
    "${SKYAPO_TEST_LV2_LATENCY_DIR}/plugin.ttl" COPYONLY)
  target_compile_definitions(skyapo-tests PRIVATE SKYAPO_TEST_LV2=1)
  target_compile_definitions(skyapo-tests PRIVATE
    SKYAPO_TEST_LV2_PATH="${SKYAPO_TEST_LV2_PATH}")
  target_compile_definitions(skyapo-realtime-safety-tests PRIVATE SKYAPO_TEST_LV2=1)
  add_dependencies(skyapo-tests skyapo-test-lv2 skyapo-test-lv2-latency)
  add_dependencies(skyapo-realtime-safety-tests skyapo-test-lv2
    skyapo-test-lv2-latency)
  add_executable(skyapo-lv2-oversize-test tests/plugin_oversize_test.cpp)
  target_include_directories(skyapo-lv2-oversize-test PRIVATE src/plugin)
  target_link_libraries(skyapo-lv2-oversize-test PRIVATE skyapo-core)
  add_dependencies(skyapo-lv2-oversize-test skyapo-test-lv2)
  add_test(NAME skyapo-lv2-oversize-block COMMAND skyapo-lv2-oversize-test)
  set_tests_properties(skyapo-lv2-oversize-block PROPERTIES
    ENVIRONMENT "LV2_PATH=${SKYAPO_TEST_LV2_PATH}")
  if(SKYAPO_SANITIZERS)
    set_property(TEST skyapo-lv2-oversize-block APPEND PROPERTY ENVIRONMENT
      "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
  endif()
  add_executable(skyapo-lv2-live-control-test
    tests/lv2_live_control_test.cpp)
  target_include_directories(skyapo-lv2-live-control-test PRIVATE src/plugin)
  target_link_libraries(skyapo-lv2-live-control-test PRIVATE skyapo-core)
  add_dependencies(skyapo-lv2-live-control-test skyapo-test-lv2)
  add_test(NAME skyapo-lv2-live-control COMMAND skyapo-lv2-live-control-test)
  set_tests_properties(skyapo-lv2-live-control PROPERTIES
    ENVIRONMENT "LV2_PATH=${SKYAPO_TEST_LV2_PATH}")
  if(SKYAPO_SANITIZERS)
    set_property(TEST skyapo-lv2-live-control APPEND PROPERTY ENVIRONMENT
      "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
  endif()
  set_tests_properties(skyapo-core skyapo-realtime-safety PROPERTIES
    ENVIRONMENT "LV2_PATH=${SKYAPO_TEST_LV2_PATH}")
  if(SKYAPO_SANITIZERS)
    set_property(TEST skyapo-core skyapo-realtime-safety APPEND PROPERTY
      ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
  endif()
else()
  message(STATUS "LV2 development headers not found: LV2 host test fixture disabled")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/clap/include/clap/clap.h")
  set(SKYAPO_TEST_CLAP_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-plugins/clap")
  file(MAKE_DIRECTORY "${SKYAPO_TEST_CLAP_DIR}")
  add_library(skyapo-test-clap MODULE tests/plugins/test_gain_clap.c)
  target_include_directories(skyapo-test-clap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/clap/include)
  set_target_properties(skyapo-test-clap PROPERTIES PREFIX ""
    SUFFIX ".clap" LIBRARY_OUTPUT_DIRECTORY "${SKYAPO_TEST_CLAP_DIR}")
  target_compile_definitions(skyapo-tests PRIVATE SKYAPO_TEST_CLAP=1)
  target_compile_definitions(skyapo-tests PRIVATE
    SKYAPO_TEST_CLAP_PATH="${SKYAPO_TEST_CLAP_DIR}")
  target_compile_definitions(skyapo-realtime-safety-tests PRIVATE SKYAPO_TEST_CLAP=1)
  add_dependencies(skyapo-tests skyapo-test-clap)
  add_dependencies(skyapo-realtime-safety-tests skyapo-test-clap)
  set_property(TEST skyapo-core skyapo-realtime-safety APPEND PROPERTY
    ENVIRONMENT "CLAP_PATH=${SKYAPO_TEST_CLAP_DIR};SKYAPO_CLAP_PATHS_ONLY=1")
  add_test(NAME skyapo-clap-plugin-info COMMAND skyapo plugin info
    org.skyapo.test.gain)
  set_tests_properties(skyapo-clap-plugin-info PROPERTIES
    ENVIRONMENT "CLAP_PATH=${SKYAPO_TEST_CLAP_DIR};SKYAPO_CLAP_PATHS_ONLY=1"
    PASS_REGULAR_EXPRESSION "7[ \t]+Gain")
  add_executable(skyapo-clap-failure-test tests/clap_failure_test.cpp)
  target_include_directories(skyapo-clap-failure-test PRIVATE src/plugin)
  target_link_libraries(skyapo-clap-failure-test PRIVATE skyapo-core)
  add_dependencies(skyapo-clap-failure-test skyapo-test-clap)
  add_test(NAME skyapo-clap-process-failure COMMAND skyapo-clap-failure-test)
  set_tests_properties(skyapo-clap-process-failure PROPERTIES
    ENVIRONMENT "CLAP_PATH=${SKYAPO_TEST_CLAP_DIR};SKYAPO_CLAP_PATHS_ONLY=1")
endif()

add_executable(skyapo-render-tests tests/render_tests.cpp)
target_link_libraries(skyapo-render-tests PRIVATE PkgConfig::SNDFILE)
add_test(NAME skyapo-offline-render COMMAND skyapo-render-tests $<TARGET_FILE:skyapo-render> ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt)
add_test(NAME skyapo-offline-device-render
  COMMAND skyapo-render-tests $<TARGET_FILE:skyapo-render>
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt -6 test.pipewire.input)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/clap/include/clap/clap.h")
  add_test(NAME skyapo-offline-clap-render COMMAND skyapo-render-tests
    $<TARGET_FILE:skyapo-render>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/clap_gain.txt -6.0206)
  add_test(NAME skyapo-offline-clap-parameter-render COMMAND skyapo-render-tests
    $<TARGET_FILE:skyapo-render>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/clap_override.txt -12.0412)
  set_tests_properties(skyapo-offline-clap-render PROPERTIES
    ENVIRONMENT "CLAP_PATH=${SKYAPO_TEST_CLAP_DIR};SKYAPO_CLAP_PATHS_ONLY=1")
  set_tests_properties(skyapo-offline-clap-parameter-render PROPERTIES
    ENVIRONMENT "CLAP_PATH=${SKYAPO_TEST_CLAP_DIR};SKYAPO_CLAP_PATHS_ONLY=1")
  add_dependencies(skyapo-render-tests skyapo-test-clap)
endif()
if(SKYAPO_HAVE_VST3)
  add_test(NAME skyapo-offline-vst3-render COMMAND skyapo-render-tests
    $<TARGET_FILE:skyapo-render>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/vst3_gain.txt -6.0206)
  add_test(NAME skyapo-offline-vst3-parameter-render COMMAND skyapo-render-tests
    $<TARGET_FILE:skyapo-render>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/vst3_parameter.txt -12.0412)
  add_test(NAME skyapo-vst3-plugin-info COMMAND skyapo plugin info
    534B5941504F00010000000000000001)
  set_tests_properties(skyapo-vst3-plugin-info PROPERTIES
    ENVIRONMENT "VST3_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins;SKYAPO_VST3_PATHS_ONLY=1"
    PASS_REGULAR_EXPRESSION "7[ \t]+Gain[ \t]+default=0.5")
  add_executable(skyapo-vst3-failure-test tests/vst3_failure_test.cpp)
  target_include_directories(skyapo-vst3-failure-test PRIVATE src/plugin)
  target_link_libraries(skyapo-vst3-failure-test PRIVATE skyapo-core)
  add_dependencies(skyapo-vst3-failure-test skyapo-test-vst3)
  add_test(NAME skyapo-vst3-process-failure COMMAND skyapo-vst3-failure-test)
  set_tests_properties(skyapo-vst3-process-failure PROPERTIES
    ENVIRONMENT "VST3_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins;SKYAPO_VST3_PATHS_ONLY=1")
  set_tests_properties(skyapo-offline-vst3-render skyapo-offline-vst3-parameter-render
    PROPERTIES ENVIRONMENT
    "VST3_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins;SKYAPO_VST3_PATHS_ONLY=1")
  add_dependencies(skyapo-render-tests skyapo-test-vst3)
  target_compile_definitions(skyapo-realtime-safety-tests PRIVATE SKYAPO_TEST_VST3=1)
  add_dependencies(skyapo-realtime-safety-tests skyapo-test-vst3)
  target_compile_definitions(skyapo-tests PRIVATE SKYAPO_TEST_VST3=1)
  target_compile_definitions(skyapo-tests PRIVATE
    SKYAPO_TEST_VST3_PATH="${CMAKE_CURRENT_BINARY_DIR}/test-plugins")
  add_dependencies(skyapo-tests skyapo-test-vst3)
  set_property(TEST skyapo-realtime-safety APPEND PROPERTY ENVIRONMENT
    "VST3_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins;SKYAPO_VST3_PATHS_ONLY=1")
  set_property(TEST skyapo-core APPEND PROPERTY ENVIRONMENT
    "VST3_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins;SKYAPO_VST3_PATHS_ONLY=1")
endif()
if(LILV_FOUND AND LV2_FOUND)
  add_test(NAME skyapo-offline-lv2-render COMMAND skyapo-render-tests
    $<TARGET_FILE:skyapo-render>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/lv2_realtime.txt -12.0206)
  add_test(NAME skyapo-offline-lv2-parameter-render COMMAND skyapo-render-tests
    $<TARGET_FILE:skyapo-render>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/lv2_override.txt -18.0412)
  set_tests_properties(skyapo-offline-lv2-render PROPERTIES
    ENVIRONMENT "LV2_PATH=${SKYAPO_TEST_LV2_PATH}")
  set_tests_properties(skyapo-offline-lv2-parameter-render PROPERTIES
    ENVIRONMENT "LV2_PATH=${SKYAPO_TEST_LV2_PATH}")
  if(SKYAPO_SANITIZERS)
    set_property(TEST skyapo-offline-lv2-render APPEND PROPERTY ENVIRONMENT
      "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
    set_property(TEST skyapo-offline-lv2-parameter-render APPEND PROPERTY ENVIRONMENT
      "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
  endif()
endif()

# Real hardware test, explicitly invoked; never part of unattended CTest.
add_executable(skyapo-realtime-probe tests/realtime_probe.cpp)
target_link_libraries(skyapo-realtime-probe PRIVATE PkgConfig::PIPEWIRE PkgConfig::SNDFILE)
add_executable(skyapo-pipewire-lifetime tests/pipewire_lifetime.cpp)
target_link_libraries(skyapo-pipewire-lifetime PRIVATE PkgConfig::PIPEWIRE)
add_executable(skyapo-pipewire-test-source tests/pipewire_test_source.cpp)
target_link_libraries(skyapo-pipewire-test-source PRIVATE PkgConfig::PIPEWIRE)
add_executable(skyapo-pipewire-test-consumer tests/pipewire_test_consumer.cpp)
target_link_libraries(skyapo-pipewire-test-consumer PRIVATE PkgConfig::PIPEWIRE)
add_executable(skyapo-default-sink-volume-tests
  tests/default_sink_volume_monitor_test.cpp
  src/pipewire/DefaultSinkVolumeMonitor.cpp)
target_link_libraries(skyapo-default-sink-volume-tests PRIVATE PkgConfig::PIPEWIRE)
if(SKYAPO_ENABLE_PIPEWIRE_E2E_TESTS)
  if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "PipeWire E2E tests require Python 3")
  endif()
  find_program(SKYAPO_PIPEWIRE_EXECUTABLE pipewire REQUIRED)
  find_program(SKYAPO_PW_CLI_EXECUTABLE pw-cli REQUIRED)
  find_program(SKYAPO_PW_DUMP_EXECUTABLE pw-dump REQUIRED)
  find_program(SKYAPO_PW_METADATA_EXECUTABLE pw-metadata REQUIRED)
  target_compile_definitions(skyapo-pipewire PRIVATE
    SKYAPO_PIPEWIRE_E2E_TESTING=1)
  find_file(SKYAPO_PIPEWIRE_CONFIG pipewire.conf
    PATHS /usr/share/pipewire /usr/local/share/pipewire REQUIRED)
  foreach(channel_mode IN ITEMS stereo mono)
    foreach(sample_rate IN ITEMS 44100 48000 96000)
      set(test_mode "${channel_mode}-${sample_rate}")
      add_test(NAME skyapo-pipewire-e2e-${test_mode}
        COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
          ${SKYAPO_PIPEWIRE_EXECUTABLE}
          ${SKYAPO_PW_CLI_EXECUTABLE}
          ${SKYAPO_PW_DUMP_EXECUTABLE}
          $<TARGET_FILE:skyapod>
          $<TARGET_FILE:skyapo>
          $<TARGET_FILE:skyapo-pipewire-test-source>
          $<TARGET_FILE:skyapo-pipewire-test-consumer>
          ${SKYAPO_PIPEWIRE_CONFIG}
          ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt
          ${test_mode})
      set_tests_properties(skyapo-pipewire-e2e-${test_mode}
        PROPERTIES TIMEOUT 60 LABELS "pipewire;integration")
    endforeach()
  endforeach()
  add_test(NAME skyapo-pipewire-e2e-source-replug
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
      ${SKYAPO_PIPEWIRE_EXECUTABLE}
      ${SKYAPO_PW_CLI_EXECUTABLE}
      ${SKYAPO_PW_DUMP_EXECUTABLE}
      $<TARGET_FILE:skyapod>
      $<TARGET_FILE:skyapo>
      $<TARGET_FILE:skyapo-pipewire-test-source>
      $<TARGET_FILE:skyapo-pipewire-test-consumer>
      ${SKYAPO_PIPEWIRE_CONFIG}
      ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt
      source-replug)
  set_tests_properties(skyapo-pipewire-e2e-source-replug
    PROPERTIES TIMEOUT 75 LABELS "pipewire;integration;recovery")
  add_test(NAME skyapo-pipewire-e2e-delay-pdc
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
      ${SKYAPO_PIPEWIRE_EXECUTABLE}
      ${SKYAPO_PW_CLI_EXECUTABLE}
      ${SKYAPO_PW_DUMP_EXECUTABLE}
      $<TARGET_FILE:skyapod>
      $<TARGET_FILE:skyapo>
      $<TARGET_FILE:skyapo-pipewire-test-source>
      $<TARGET_FILE:skyapo-pipewire-test-consumer>
      ${SKYAPO_PIPEWIRE_CONFIG}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/delay_pdc.txt
      delay-pdc)
  set_tests_properties(skyapo-pipewire-e2e-delay-pdc
    PROPERTIES TIMEOUT 75 LABELS "pipewire;integration;latency;pdc")
  add_test(NAME skyapo-pipewire-e2e-server-restart
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
      ${SKYAPO_PIPEWIRE_EXECUTABLE}
      ${SKYAPO_PW_CLI_EXECUTABLE}
      ${SKYAPO_PW_DUMP_EXECUTABLE}
      $<TARGET_FILE:skyapod>
      $<TARGET_FILE:skyapo>
      $<TARGET_FILE:skyapo-pipewire-test-source>
      $<TARGET_FILE:skyapo-pipewire-test-consumer>
      ${SKYAPO_PIPEWIRE_CONFIG}
      ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt
      server-restart)
  set_tests_properties(skyapo-pipewire-e2e-server-restart
    PROPERTIES TIMEOUT 90 LABELS "pipewire;integration;recovery")
  add_test(NAME skyapo-pipewire-e2e-include-reload
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
      ${SKYAPO_PIPEWIRE_EXECUTABLE}
      ${SKYAPO_PW_CLI_EXECUTABLE}
      ${SKYAPO_PW_DUMP_EXECUTABLE}
      $<TARGET_FILE:skyapod>
      $<TARGET_FILE:skyapo>
      $<TARGET_FILE:skyapo-pipewire-test-source>
      $<TARGET_FILE:skyapo-pipewire-test-consumer>
      ${SKYAPO_PIPEWIRE_CONFIG}
      ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt
      include-reload)
  set_tests_properties(skyapo-pipewire-e2e-include-reload
    PROPERTIES TIMEOUT 60 LABELS "pipewire;integration;config")
  add_test(NAME skyapo-pipewire-e2e-device-filter
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
      ${SKYAPO_PIPEWIRE_EXECUTABLE}
      ${SKYAPO_PW_CLI_EXECUTABLE}
      ${SKYAPO_PW_DUMP_EXECUTABLE}
      $<TARGET_FILE:skyapod>
      $<TARGET_FILE:skyapo>
      $<TARGET_FILE:skyapo-pipewire-test-source>
      $<TARGET_FILE:skyapo-pipewire-test-consumer>
      ${SKYAPO_PIPEWIRE_CONFIG}
      ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt
      device-filter)
  set_tests_properties(skyapo-pipewire-e2e-device-filter
    PROPERTIES TIMEOUT 60 LABELS "pipewire;integration;config")
  add_test(NAME skyapo-pipewire-e2e-device-switch
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
      ${SKYAPO_PIPEWIRE_EXECUTABLE}
      ${SKYAPO_PW_CLI_EXECUTABLE}
      ${SKYAPO_PW_DUMP_EXECUTABLE}
      $<TARGET_FILE:skyapod>
      $<TARGET_FILE:skyapo>
      $<TARGET_FILE:skyapo-pipewire-test-source>
      $<TARGET_FILE:skyapo-pipewire-test-consumer>
      ${SKYAPO_PIPEWIRE_CONFIG}
      ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt
      device-switch)
  set_tests_properties(skyapo-pipewire-e2e-device-switch
    PROPERTIES TIMEOUT 75 LABELS "pipewire;integration;device;recovery")
  add_test(NAME skyapo-pipewire-e2e-renegotiate
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
      ${SKYAPO_PIPEWIRE_EXECUTABLE}
      ${SKYAPO_PW_CLI_EXECUTABLE}
      ${SKYAPO_PW_DUMP_EXECUTABLE}
      $<TARGET_FILE:skyapod>
      $<TARGET_FILE:skyapo>
      $<TARGET_FILE:skyapo-pipewire-test-source>
      $<TARGET_FILE:skyapo-pipewire-test-consumer>
      ${SKYAPO_PIPEWIRE_CONFIG}
      ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt
      renegotiate)
  set_tests_properties(skyapo-pipewire-e2e-renegotiate
    PROPERTIES TIMEOUT 100 LABELS "pipewire;integration;recovery"
      ENVIRONMENT "SKYAPO_PW_METADATA=${SKYAPO_PW_METADATA_EXECUTABLE}")
  add_test(NAME skyapo-pipewire-e2e-transition-format
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
      ${SKYAPO_PIPEWIRE_EXECUTABLE}
      ${SKYAPO_PW_CLI_EXECUTABLE}
      ${SKYAPO_PW_DUMP_EXECUTABLE}
      $<TARGET_FILE:skyapod>
      $<TARGET_FILE:skyapo>
      $<TARGET_FILE:skyapo-pipewire-test-source>
      $<TARGET_FILE:skyapo-pipewire-test-consumer>
      ${SKYAPO_PIPEWIRE_CONFIG}
      ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt
      transition-format)
  set_tests_properties(skyapo-pipewire-e2e-transition-format
    PROPERTIES TIMEOUT 100 LABELS "pipewire;integration;recovery"
      ENVIRONMENT "SKYAPO_PW_METADATA=${SKYAPO_PW_METADATA_EXECUTABLE}")
  if(SKYAPO_TEST_CLAP_DIR)
    add_test(NAME skyapo-pipewire-e2e-plugin-bypass
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/clap_realtime.txt
        plugin-bypass)
    set_tests_properties(skyapo-pipewire-e2e-plugin-bypass
      PROPERTIES TIMEOUT 60 LABELS "pipewire;integration;plugin"
      ENVIRONMENT
        "CLAP_PATH=${SKYAPO_TEST_CLAP_DIR};SKYAPO_CLAP_PATHS_ONLY=1")
    add_test(NAME skyapo-pipewire-e2e-plugin-live-parameter
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/clap_realtime.txt
        plugin-live-param)
    set_tests_properties(skyapo-pipewire-e2e-plugin-live-parameter
      PROPERTIES TIMEOUT 60 LABELS "pipewire;integration;plugin"
      ENVIRONMENT
        "CLAP_PATH=${SKYAPO_TEST_CLAP_DIR};SKYAPO_CLAP_PATHS_ONLY=1")
    add_test(NAME skyapo-pipewire-e2e-plugin-latency
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/clap_latency_realtime.txt
        latency)
    set_tests_properties(skyapo-pipewire-e2e-plugin-latency
      PROPERTIES TIMEOUT 60 LABELS "pipewire;integration;plugin"
      ENVIRONMENT
        "CLAP_PATH=${SKYAPO_TEST_CLAP_DIR};SKYAPO_CLAP_PATHS_ONLY=1")
    add_test(NAME skyapo-pipewire-e2e-clap-dynamic-latency
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/clap_dynamic_latency.txt
        clap-latency-change)
    set_tests_properties(skyapo-pipewire-e2e-clap-dynamic-latency
      PROPERTIES TIMEOUT 75 LABELS "pipewire;integration;plugin;latency"
      ENVIRONMENT
        "CLAP_PATH=${SKYAPO_TEST_CLAP_DIR};SKYAPO_CLAP_PATHS_ONLY=1")
  endif()
  if(LILV_FOUND AND LV2_FOUND)
    add_test(NAME skyapo-pipewire-e2e-lv2-live-parameter
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/lv2_realtime.txt
        lv2-live-param)
    set_tests_properties(skyapo-pipewire-e2e-lv2-live-parameter
      PROPERTIES TIMEOUT 60 LABELS "pipewire;integration;plugin"
      ENVIRONMENT "LV2_PATH=${SKYAPO_TEST_LV2_PATH}")
    add_test(NAME skyapo-pipewire-e2e-lv2-dynamic-latency
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/lv2_latency_realtime.txt
        lv2-latency-change)
    set_tests_properties(skyapo-pipewire-e2e-lv2-dynamic-latency
      PROPERTIES TIMEOUT 75 LABELS "pipewire;integration;plugin;latency"
      ENVIRONMENT "LV2_PATH=${SKYAPO_TEST_LV2_PATH}")
    add_test(NAME skyapo-pipewire-e2e-lv2-pdc-final
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/lv2_pdc_final.txt
        lv2-pdc-final)
    set_tests_properties(skyapo-pipewire-e2e-lv2-pdc-final
      PROPERTIES TIMEOUT 75 LABELS "pipewire;integration;plugin;latency;pdc"
      ENVIRONMENT "LV2_PATH=${SKYAPO_TEST_LV2_PATH}")
  endif()
  if(SKYAPO_HAVE_VST3 AND TARGET skyapo-test-vst3)
    add_test(NAME skyapo-pipewire-e2e-vst3-live-parameter
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/vst3_realtime_chain.txt
        vst3-live-param)
    set_tests_properties(skyapo-pipewire-e2e-vst3-live-parameter
      PROPERTIES TIMEOUT 60 LABELS "pipewire;integration;plugin"
      ENVIRONMENT
        "VST3_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins;SKYAPO_VST3_PATHS_ONLY=1")
    add_test(NAME skyapo-pipewire-e2e-vst3-dynamic-latency
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/vst3_latency_change.txt
        vst3-latency-change)
    set_tests_properties(skyapo-pipewire-e2e-vst3-dynamic-latency
      PROPERTIES TIMEOUT 75 LABELS "pipewire;integration;plugin;latency"
      ENVIRONMENT
        "VST3_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins;SKYAPO_VST3_PATHS_ONLY=1")
  endif()
  if(SKYAPO_ENABLE_FST_VST2_HOST)
    set(SKYAPO_TEST_VST2_CONFIG
      "${CMAKE_CURRENT_BINARY_DIR}/tests/vst2_realtime_chain.txt")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    file(GENERATE OUTPUT "${SKYAPO_TEST_VST2_CONFIG}"
      CONTENT "Plugin: VST2 \"$<TARGET_FILE:skyapo-test-vst2-plugin>\" 0=0.25\n")
    add_test(NAME skyapo-pipewire-e2e-vst2-live-parameter
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${SKYAPO_TEST_VST2_CONFIG}
        vst2-live-param)
    set_tests_properties(skyapo-pipewire-e2e-vst2-live-parameter
      PROPERTIES TIMEOUT 60 LABELS "pipewire;integration;plugin"
      ENVIRONMENT
        "SKYAPO_TEST_VST2_PATH=$<TARGET_FILE:skyapo-test-vst2-plugin>")
    set(SKYAPO_TEST_VST2_DYNAMIC_LATENCY_CONFIG
      "${CMAKE_CURRENT_BINARY_DIR}/tests/vst2_dynamic_latency_chain.txt")
    file(GENERATE OUTPUT "${SKYAPO_TEST_VST2_DYNAMIC_LATENCY_CONFIG}"
      CONTENT "Preamp: -6 dB\nPlugin: VST2 \"$<TARGET_FILE:skyapo-test-vst2-dynamic-latency-plugin>\"\n")
    add_test(NAME skyapo-pipewire-e2e-vst2-dynamic-latency
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipewire_e2e_test.py
        ${SKYAPO_PIPEWIRE_EXECUTABLE}
        ${SKYAPO_PW_CLI_EXECUTABLE}
        ${SKYAPO_PW_DUMP_EXECUTABLE}
        $<TARGET_FILE:skyapod>
        $<TARGET_FILE:skyapo>
        $<TARGET_FILE:skyapo-pipewire-test-source>
        $<TARGET_FILE:skyapo-pipewire-test-consumer>
        ${SKYAPO_PIPEWIRE_CONFIG}
        ${SKYAPO_TEST_VST2_DYNAMIC_LATENCY_CONFIG}
        vst2-latency-change)
    set_tests_properties(skyapo-pipewire-e2e-vst2-dynamic-latency
      PROPERTIES TIMEOUT 75 LABELS "pipewire;integration;plugin;latency"
      ENVIRONMENT
        "SKYAPO_TEST_VST2_DYNAMIC_LATENCY_PATH=$<TARGET_FILE:skyapo-test-vst2-dynamic-latency-plugin>")
  endif()
endif()

# PipeWire's pw_context_load_module currently retains allocations from external
# modules at process exit. Keep LeakSanitizer enabled for independent tests and
# suppress only these integration tests; ASan/UBSan remain active throughout.
if(SKYAPO_SANITIZERS AND SKYAPO_ENABLE_PIPEWIRE_E2E_TESTS)
  get_property(SKYAPO_CONFIGURED_TESTS DIRECTORY PROPERTY TESTS)
  foreach(test_name IN LISTS SKYAPO_CONFIGURED_TESTS)
    if(test_name MATCHES "^skyapo-pipewire-e2e-")
      get_property(test_environment TEST ${test_name} PROPERTY ENVIRONMENT)
      list(APPEND test_environment "ASAN_OPTIONS=detect_leaks=0")
      set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "${test_environment}")
    endif()
  endforeach()
endif()

configure_file(packaging/skyapod.service.in
  ${CMAKE_CURRENT_BINARY_DIR}/skyapod.service @ONLY)
install(TARGETS skyapo skyapod skyapo-render skyapo-bench
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/skyapod.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
install(FILES docs/man/skyapo.1 docs/man/skyapod.1 docs/man/skyapo-bench.1
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES README.md ROADMAP.md CHANGELOG.md docs/THIRD_PARTY_NOTICES.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES upstream/equalizerapo/License.txt
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
  RENAME EqualizerAPO-License.txt)
if(SKYAPO_BUILD_UI AND Qt6Widgets_FOUND AND SKYAPO_QT_LGPL3_LICENSE)
  install(FILES "${SKYAPO_QT_LGPL3_LICENSE}"
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
    RENAME Qt-LGPL-3.0-License.txt)
endif()
install(FILES upstream/clap/LICENSE
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
  RENAME CLAP-License.txt)
install(FILES upstream/vst3/base/LICENSE.txt
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
  RENAME Steinberg-VST3-base-License.txt)
install(FILES upstream/vst3/pluginterfaces/LICENSE.txt
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
  RENAME Steinberg-VST3-pluginterfaces-License.txt)
install(FILES upstream/vst3/public.sdk/LICENSE.txt
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
  RENAME Steinberg-VST3-public-sdk-License.txt)
if(SKYAPO_ENABLE_FST_VST2_HOST)
  install(FILES upstream/fst/LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
    RENAME FST-GPL-3.0-or-later-License.txt)
endif()
if(SKYAPO_HAVE_MUPARSERX)
  install(FILES "${SKYAPO_MUPARSERX_ROOT}/License.txt"
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
    RENAME MuParserX-License.txt)
endif()

if(SKYAPO_ENABLE_CPACK)
  set(CPACK_PACKAGE_NAME "skyapo")
  set(CPACK_PACKAGE_VENDOR "SkyAPO contributors")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Equalizer APO DSP engine with a native PipeWire audio backend")
  set(CPACK_PACKAGE_CONTACT "SkyAPO maintainers")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME
      "skyapo-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
  set(CPACK_GENERATOR "DEB;RPM")
  set(CPACK_STRIP_FILES ON)

  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "SkyAPO maintainers")
  set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
  # Declare the Debian runtime packages for each optional feature detected at
  # configure time. This avoids dpkg-shlibdeps depending on the host's package
  # database when cross-building a .deb from another Linux distribution.
  set(_skyapo_debian_dependencies "libsndfile1;libpipewire-0.3-0")
  if(FFTW3F_FOUND)
    list(APPEND _skyapo_debian_dependencies "libfftw3-single3")
  endif()
  if(LILV_FOUND)
    list(APPEND _skyapo_debian_dependencies "liblilv-0-0")
  endif()
  if(MUPARSER_FOUND)
    list(APPEND _skyapo_debian_dependencies "libmuparser2v5")
  endif()
  if(SKYAPO_BUILD_UI AND Qt6Widgets_FOUND)
    list(APPEND _skyapo_debian_dependencies "libqt6widgets6")
  endif()
  list(JOIN _skyapo_debian_dependencies ", " CPACK_DEBIAN_PACKAGE_DEPENDS)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
  set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

  set(CPACK_RPM_PACKAGE_LICENSE
      "GPL-2.0-or-later AND BSD-2-Clause AND MIT AND LGPL-3.0-only")
  set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
  set(CPACK_RPM_PACKAGE_RELEASE "1")
  set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
  set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
  include(CPack)
endif()
