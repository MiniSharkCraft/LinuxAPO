cmake_minimum_required(VERSION 3.20)
project(SkyAPO VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SKYAPO_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(SKYAPO_BUILD_UI "Build the Qt configuration editor when Qt 6 is available" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(FFTW3F QUIET IMPORTED_TARGET fftw3f)
pkg_check_modules(LILV QUIET IMPORTED_TARGET lilv-0)
pkg_check_modules(LV2 QUIET IMPORTED_TARGET lv2)

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
target_include_directories(skyapo-core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin src/core ${EAPO_PORT}
         ${EAPO_PORT}/filters ${EAPO}/filters ${EAPO} ${EAPO}/helpers)
target_compile_definitions(skyapo-core PRIVATE __forceinline=inline)
target_link_libraries(skyapo-core PUBLIC skyapo-eapo-dsp)
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
else()
  message(STATUS "FFTW3f not found: GraphicEQ/Convolution directives disabled")
endif()

add_executable(skyapo-render src/render/main.cpp)
target_link_libraries(skyapo-render PRIVATE skyapo-core PkgConfig::SNDFILE)

add_executable(skyapo src/cli/main.cpp)
target_compile_definitions(skyapo PRIVATE
  SKYAPO_VERSION="${PROJECT_VERSION}"
  SKYAPO_UPSTREAM_REVISION="${SKYAPO_UPSTREAM_REVISION}")
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
if(SKYAPO_BUILD_UI)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(Qt6Widgets_FOUND)
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
      ${SKYAPO_EDITOR}/Editor.qrc
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
    set_target_properties(skyapo-eapo-editor PROPERTIES
      AUTOMOC ON AUTOUIC ON AUTORCC ON)
    set_property(TARGET skyapo-eapo-editor PROPERTY AUTOUIC_SEARCH_PATHS
      "${EAPO_PORT}/Editor;${SKYAPO_EDITOR};${SKYAPO_EDITOR}/guis;${SKYAPO_EDITOR}/widgets")
    target_include_directories(skyapo-eapo-editor BEFORE PUBLIC ${EAPO_PORT} ${EAPO})
    # The upstream editor's legacy stable.h supplied these Qt declarations
    # globally; provide only the declarations needed by the selected sources.
    target_precompile_headers(skyapo-eapo-editor PRIVATE
      <QString> <QStringList> <QMetaType> <QDateTime>)
    target_compile_definitions(skyapo-eapo-editor PRIVATE __forceinline=inline)
    target_link_libraries(skyapo-eapo-editor PUBLIC skyapo-core Qt6::Widgets)

    add_executable(skyapo-ui src/ui/main.cpp src/ui/MainWindow.cpp
      src/ui/MainWindow.h src/ui/ConfigFile.cpp src/ui/ConfigFile.h
      src/ui/IncludeEditor.cpp src/ui/IncludeEditor.h)
    set_target_properties(skyapo-ui PROPERTIES AUTOMOC ON)
    target_include_directories(skyapo-ui PRIVATE ${SKYAPO_EDITOR})
    target_compile_definitions(skyapo-ui PRIVATE __forceinline=inline)
    target_link_libraries(skyapo-ui PRIVATE skyapo-eapo-editor)

    add_executable(skyapo-ui-test-cli tests/ui_cli_stub.cpp)
    add_executable(skyapo-ui-tests tests/ui_tests.cpp src/ui/MainWindow.cpp
      src/ui/MainWindow.h src/ui/ConfigFile.cpp src/ui/IncludeEditor.cpp
      src/ui/IncludeEditor.h)
    set_target_properties(skyapo-ui-tests PROPERTIES AUTOMOC ON)
    target_include_directories(skyapo-ui-tests PRIVATE src/ui)
    target_include_directories(skyapo-ui-tests PRIVATE ${SKYAPO_EDITOR})
    target_compile_definitions(skyapo-ui-tests PRIVATE __forceinline=inline)
    target_link_libraries(skyapo-ui-tests PRIVATE skyapo-eapo-editor)
    add_test(NAME skyapo-ui-editor COMMAND skyapo-ui-tests
      $<TARGET_FILE:skyapo-ui-test-cli>)
    set_tests_properties(skyapo-ui-editor PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    install(TARGETS skyapo-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/skyapo.desktop
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
  else()
    message(STATUS "Qt 6 Widgets not found: skyapo-ui target disabled")
  endif()
endif()

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
if(LILV_FOUND AND LV2_FOUND)
  set(SKYAPO_TEST_LV2_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/test-plugins/skyapo-test.lv2")
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
  target_compile_definitions(skyapo-tests PRIVATE SKYAPO_TEST_LV2=1)
  target_compile_definitions(skyapo-realtime-safety-tests PRIVATE SKYAPO_TEST_LV2=1)
  add_dependencies(skyapo-tests skyapo-test-lv2)
  add_dependencies(skyapo-realtime-safety-tests skyapo-test-lv2)
  set_tests_properties(skyapo-core skyapo-realtime-safety PROPERTIES
    ENVIRONMENT "LV2_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins")
  if(SKYAPO_SANITIZERS)
    set_property(TEST skyapo-core skyapo-realtime-safety APPEND PROPERTY
      ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
  endif()
else()
  message(STATUS "LV2 development headers not found: LV2 host test fixture disabled")
endif()

add_executable(skyapo-render-tests tests/render_tests.cpp)
target_link_libraries(skyapo-render-tests PRIVATE PkgConfig::SNDFILE)
add_test(NAME skyapo-offline-render COMMAND skyapo-render-tests $<TARGET_FILE:skyapo-render> ${CMAKE_CURRENT_SOURCE_DIR}/examples/preamp.txt)
if(LILV_FOUND AND LV2_FOUND)
  add_test(NAME skyapo-offline-lv2-render COMMAND skyapo-render-tests
    $<TARGET_FILE:skyapo-render>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/lv2_realtime.txt -12.0206)
  add_test(NAME skyapo-offline-lv2-parameter-render COMMAND skyapo-render-tests
    $<TARGET_FILE:skyapo-render>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/lv2_override.txt -18.0412)
  set_tests_properties(skyapo-offline-lv2-render PROPERTIES
    ENVIRONMENT "LV2_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins")
  set_tests_properties(skyapo-offline-lv2-parameter-render PROPERTIES
    ENVIRONMENT "LV2_PATH=${CMAKE_CURRENT_BINARY_DIR}/test-plugins")
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
