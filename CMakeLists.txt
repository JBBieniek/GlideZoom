cmake_minimum_required(VERSION 3.20)
set(CMAKE_VS_GLOBALS "ImportDirectoryBuildProps=false;ImportDirectoryBuildTargets=false")
project(GlideZoom VERSION 0.6.2 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(GLIDEZOOM_BUILD_APP "Build the native Windows app and installer" ON)
option(GLIDEZOOM_LIBFUZZER "Use ASan and libFuzzer instead of the deterministic input harness" OFF)
include(CTest)

function(glidezoom_compile_settings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /MP1)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(WIN32)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  endif()
endfunction()

function(glidezoom_test target source)
  add_executable(${target} ${source})
  glidezoom_compile_settings(${target})
  add_test(NAME ${target} COMMAND "${CMAKE_COMMAND}"
    "-DGLIDEZOOM_TEST_EXE=$<TARGET_FILE:${target}>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RunPortableTest.cmake")
endfunction()

if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  foreach(suite core input_state input_transport zoom_session settings control_channel fusion
                monitor_geometry view_mode monitor_presenter shortcut touchpad touchpad_packets touchpad_hid
                tray setup zoom_precision setup_ui pointer_motion pointer_channel capture viewport_motion
                cursor_visibility latency input_context gpu_frame_policy gpu_cursor_pixels gpu_queue_policy
                pointer_feedback overlay_order passive_monitor window_fit settings_reload monitor_return pointer_speed_composition)
    glidezoom_test(glidezoom_${suite}_tests "tests/${suite}_tests.cpp")
    target_link_libraries(glidezoom_${suite}_tests PRIVATE Threads::Threads)
  endforeach()
  target_include_directories(glidezoom_touchpad_hid_tests PRIVATE tests/hid_mocks)
  foreach(suite capture_native monitor_native latency_native paint_native)
    glidezoom_test(glidezoom_${suite}_tests "tests/${suite}_tests.cpp")
  endforeach()
  target_include_directories(glidezoom_capture_native_tests PRIVATE tests/capture_mocks)
  target_include_directories(glidezoom_monitor_native_tests PRIVATE tests/renderer_mocks)
  target_include_directories(glidezoom_latency_native_tests PRIVATE tests/timing_mocks)
  target_include_directories(glidezoom_paint_native_tests PRIVATE tests/paint_mocks)
  foreach(suite InputHealth Routing WheelAcceleration WheelActivation PointerSpeed ScopeIndicator IndicatorVisibility)
    glidezoom_test(glidezoom_${suite}_tests "tests/${suite}Tests.cpp")
    target_link_libraries(glidezoom_${suite}_tests PRIVATE Threads::Threads)
  endforeach()
  glidezoom_test(glidezoom_settings_ui_tests tests/settings_ui_tests.cpp)
  if(WIN32)
    glidezoom_test(glidezoom_shader_tests tests/GpuShaderTests.cpp)
    target_link_libraries(glidezoom_shader_tests PRIVATE d3dcompiler)
    find_program(GLIDEZOOM_POWERSHELL NAMES powershell pwsh REQUIRED)
    get_filename_component(GLIDEZOOM_POWERSHELL_HOME "${GLIDEZOOM_POWERSHELL}" DIRECTORY)
    foreach(suite install_acl install_lifecycle setup_powershell)
      add_test(NAME ${suite} COMMAND "${GLIDEZOOM_POWERSHELL}" -NoLogo -NoProfile -NonInteractive
        -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}_tests.ps1")
      # A CTest grandchild of PowerShell 7 inherits its module directories;
      # Windows PowerShell 5.1 cannot import those editions of built-in modules.
      # Limit only these test processes to their selected host's native modules.
      set_tests_properties(${suite} PROPERTIES TIMEOUT 180
        ENVIRONMENT "PSModulePath=${GLIDEZOOM_POWERSHELL_HOME}/Modules")
    endforeach()
  endif()
endif()

if(BUILD_TESTING OR GLIDEZOOM_LIBFUZZER)
  add_executable(glidezoom_input_fuzz fuzz/GlideZoom.FuzzingTest.cpp fuzz/RoutingFuzzer.cpp fuzz/PointerSpeedFuzzer.cpp)
  glidezoom_compile_settings(glidezoom_input_fuzz)
  set_property(SOURCE fuzz/GlideZoom.FuzzingTest.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/tests/hid_mocks")
  if(GLIDEZOOM_LIBFUZZER)
    target_compile_definitions(glidezoom_input_fuzz PRIVATE GLIDEZOOM_LIBFUZZER
      _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION _DISABLE_OPTIONAL_ANNOTATION)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_GENERATOR MATCHES "Visual Studio")
      set_target_properties(glidezoom_input_fuzz PROPERTIES VS_GLOBAL_EnableASAN true VS_GLOBAL_EnableFuzzer true)
      target_compile_options(glidezoom_input_fuzz PRIVATE /Zi /fsanitize=address
        /fsanitize-coverage=inline-8bit-counters /fsanitize-coverage=edge
        /fsanitize-coverage=trace-cmp /fsanitize-coverage=trace-div)
      target_link_libraries(glidezoom_input_fuzz PRIVATE libsancov legacy_stdio_definitions)
      target_link_options(glidezoom_input_fuzz PRIVATE /DEBUG /INCREMENTAL:NO)
      get_filename_component(GLIDEZOOM_COMPILER_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)
      add_custom_command(TARGET glidezoom_input_fuzz POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${GLIDEZOOM_COMPILER_DIRECTORY}/clang_rt.asan_dynamic-x86_64.dll" "$<TARGET_FILE_DIR:glidezoom_input_fuzz>" VERBATIM)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(glidezoom_input_fuzz PRIVATE -fsanitize=fuzzer,address -fno-omit-frame-pointer)
      target_link_options(glidezoom_input_fuzz PRIVATE -fsanitize=fuzzer,address)
    else()
      message(FATAL_ERROR "GLIDEZOOM_LIBFUZZER requires Clang, or Visual Studio with the C++ AddressSanitizer and fuzzing tools.")
    endif()
    if(BUILD_TESTING)
      add_test(NAME fuzz_input COMMAND glidezoom_input_fuzz -runs=10000 -max_len=16384)
    endif()
  else()
    add_test(NAME fuzz_input COMMAND "${CMAKE_COMMAND}"
      "-DGLIDEZOOM_TEST_EXE=$<TARGET_FILE:glidezoom_input_fuzz>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RunPortableTest.cmake")
  endif()
endif()

if(WIN32 AND GLIDEZOOM_BUILD_APP)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build native x64 or ARM64. WOW64 is unsupported by the Magnification API.")
  endif()
  enable_language(RC)
  add_executable(GlideZoom WIN32 src/main.cpp src/gpu_monitor_magnifier.cpp src/zoom_capture.cpp src/window_fit.cpp src/app.rc)
  target_include_directories(GlideZoom PRIVATE src)
  glidezoom_compile_settings(GlideZoom)
  target_link_libraries(GlideZoom PRIVATE user32 shell32 advapi32 gdi32 ole32 wtsapi32 uuid hid dwmapi
    d3d11 dxgi d3dcompiler runtimeobject dcomp comctl32)
  add_executable(GlideZoomUninstall WIN32 src/uninstall.cpp src/uninstall.rc)
  set_target_properties(GlideZoomUninstall PROPERTIES OUTPUT_NAME "GlideZoom-Uninstall")
  target_include_directories(GlideZoomUninstall PRIVATE src)
  glidezoom_compile_settings(GlideZoomUninstall)
  target_link_libraries(GlideZoomUninstall PRIVATE user32 shell32 advapi32 gdi32 ole32 uuid)

  # Re-embed current binaries and exact consent/install payloads whenever their
  # contents change, even when the resource paths remain unchanged.
  set(setup_resource "${CMAKE_CURRENT_BINARY_DIR}/setup-$<CONFIG>.rc")
  add_custom_command(OUTPUT "${setup_resource}"
    COMMAND "${CMAKE_COMMAND}"
      "-DGLIDEZOOM_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DGLIDEZOOM_APP_EXE=$<TARGET_FILE:GlideZoom>"
      "-DGLIDEZOOM_UNINSTALL_EXE=$<TARGET_FILE:GlideZoomUninstall>"
      "-DGLIDEZOOM_SETUP_RC=${setup_resource}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/GenerateSetupResources.cmake"
    DEPENDS GlideZoom GlideZoomUninstall src/setup.rc.in src/setup.manifest src/glidezoom.ico
      scripts/GenerateSetupResources.cmake scripts/Install.ps1 scripts/Uninstall.ps1
      Uninstall.cmd resources/install-security.txt
    VERBATIM COMMENT "Embedding the current app and installer consent payloads")
  add_executable(GlideZoomSetup WIN32 src/setup.cpp "${setup_resource}")
  set_target_properties(GlideZoomSetup PROPERTIES OUTPUT_NAME "GlideZoom-Setup")
  target_include_directories(GlideZoomSetup PRIVATE src)
  glidezoom_compile_settings(GlideZoomSetup)
  target_link_libraries(GlideZoomSetup PRIVATE user32 shell32 advapi32 gdi32 ole32 oleaut32 uuid comctl32)
  foreach(target GlideZoom GlideZoomUninstall GlideZoomSetup)
    if(MSVC)
      target_link_options(${target} PRIVATE /MANIFEST:NO /DYNAMICBASE /NXCOMPAT)
    else()
      target_link_options(${target} PRIVATE -municode -static -Wl,--dynamicbase,--nxcompat)
    endif()
  endforeach()
endif()
