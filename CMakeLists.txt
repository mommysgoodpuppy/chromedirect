cmake_minimum_required(VERSION 3.20)
project(chromedirect_cef_offscreen LANGUAGES C CXX)

# Require C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(USE_SUBSYSTEM_WINDOWS "Build with /SUBSYSTEM:WINDOWS" ON)
option(USE_SANDBOX "Link with CEF sandbox (requires cef_sandbox.lib)" OFF)
set(APP_NAME "chromedirect_demo")

# Ensure sandbox is disabled in cache before loading CEF (CEF defaults to ON)
set(USE_SANDBOX OFF CACHE BOOL "Enable or disable use of the sandbox." FORCE)

# CEF root can be specified via -DCEF_ROOT=... or environment. Treat empty as unset.
if(NOT DEFINED CEF_ROOT OR CEF_ROOT STREQUAL "")
  if(DEFINED ENV{CEF_ROOT} AND NOT "$ENV{CEF_ROOT}" STREQUAL "")
    set(CEF_ROOT $ENV{CEF_ROOT})
  else()
    # Default to SDK path inside repo
    set(CEF_ROOT "${CMAKE_SOURCE_DIR}/SDK/cef_binary_139.0.28+g55ab8a8+chromium-139.0.7258.139_windows64_minimal")
  endif()
endif()

if(NOT EXISTS "${CEF_ROOT}/cmake/cef_variables.cmake")
  # Some CEF packages extract with an extra nested folder of the same name.
  # Probe one level deeper automatically.
  file(GLOB _cef_nested_candidates "${CEF_ROOT}/*/cmake/cef_variables.cmake")
  list(LENGTH _cef_nested_candidates _cef_nested_count)
  if(_cef_nested_count GREATER 0)
    # Take the first match and set CEF_ROOT to its parent directory.
    list(GET _cef_nested_candidates 0 _cef_nested_cmake)
    get_filename_component(_cef_nested_dir "${_cef_nested_cmake}" DIRECTORY)
    get_filename_component(_cef_nested_root "${_cef_nested_dir}" DIRECTORY)
    set(CEF_ROOT "${_cef_nested_root}")
  endif()
endif()

if(NOT EXISTS "${CEF_ROOT}/cmake/cef_variables.cmake")
  message(FATAL_ERROR "CEF not found at ${CEF_ROOT}. Set CEF_ROOT to your extracted CEF binary path.")
endif()

# Load CEF package (defines variables and macros)
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

# Add CEF as a subdirectory (this defines the cef_sandbox target, etc.)
add_subdirectory(${CEF_ROOT} cef_binary_dir)

# OpenVR paths (submodule)
set(OPENVR_ROOT "${CMAKE_SOURCE_DIR}/submodules/openvr")
set(OPENVR_INCLUDE_DIR "${OPENVR_ROOT}/headers")
set(OPENVR_LIB_DIR "${OPENVR_ROOT}/lib/win64")
set(OPENVR_BIN_DIR "${OPENVR_ROOT}/bin/win64")

# Sources
set(APP_SOURCES
  src/main.cpp
  src/Client.h
  src/Client.cpp
  src/D3DPresenter.h
  src/D3DPresenter.cpp
)

# Create the executable target
add_executable(${APP_NAME} ${APP_SOURCES})

# Link with libcef and CEF sandbox (if enabled)
set(CEF_TARGET ${APP_NAME})
set(CEF_HELPER_APP ON)

## Apply standard CEF target properties (compile flags/defines, linker flags, include dirs)
SET_EXECUTABLE_TARGET_PROPERTIES(${APP_NAME})

## Create an imported logical target for libcef
ADD_LOGICAL_TARGET(cef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

## Link with libcef, CEF wrapper, sandbox (if enabled), and system libs
target_link_libraries(${APP_NAME} PRIVATE cef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
if(USE_SANDBOX)
  target_link_libraries(${APP_NAME} PRIVATE cef_sandbox)
endif()

# Additional libraries (system libs)
target_link_libraries(${APP_NAME} PRIVATE d3d11 dxgi)

# OpenVR library
target_include_directories(${APP_NAME} PRIVATE ${OPENVR_INCLUDE_DIR})
target_link_directories(${APP_NAME} PRIVATE ${OPENVR_LIB_DIR})
target_link_libraries(${APP_NAME} PRIVATE openvr_api)

# Include directories (CEF headers)
target_include_directories(${APP_NAME} PRIVATE ${CEF_INCLUDE_PATH})

# Definitions to enable windowless rendering and GPU path tweaks
add_definitions(
  -DWIN32_LEAN_AND_MEAN
  -DNOMINMAX
)

# Runtime output dir
set_target_properties(${APP_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
)

# Copy CEF runtime files (DLLs and resources) next to the binary
set(CEF_RUNTIME_DEST "${CMAKE_BINARY_DIR}/bin")
# Binaries (dlls, snapshots, etc.) live under CEF_BINARY_DIR
COPY_FILES(${APP_NAME} "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_RUNTIME_DEST}")
# Resources (paks, icu, locales directory) live under CEF_RESOURCE_DIR
COPY_FILES(${APP_NAME} "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_RUNTIME_DEST}")

# Copy OpenVR runtime DLL next to the binary
if(EXISTS "${OPENVR_BIN_DIR}/openvr_api.dll")
  add_custom_command(TARGET ${APP_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENVR_BIN_DIR}/openvr_api.dll" "${CEF_RUNTIME_DEST}/openvr_api.dll")
endif()

# Subsystem
if(MSVC AND USE_SUBSYSTEM_WINDOWS)
  set_target_properties(${APP_NAME} PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
endif()
