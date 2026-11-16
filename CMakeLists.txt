cmake_minimum_required(VERSION 4.2...4.4)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

if(NOT WIN32)
  message(FATAL_ERROR "OBS DeckLink Low Latency currently targets Windows x64 only.")
endif()

set(OBS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-${_version}-sources")
set(OBS_DECKLINK_SOURCE_DIR "${OBS_SOURCE_DIR}/plugins/decklink")
set(OBS_DECKLINK_SDK_DIR "${OBS_DECKLINK_SOURCE_DIR}/win/decklink-sdk")

if(NOT EXISTS "${OBS_DECKLINK_SOURCE_DIR}/decklink-device.cpp")
  message(FATAL_ERROR "The pinned OBS ${_version} DeckLink source dependency was not found at ${OBS_DECKLINK_SOURCE_DIR}.")
endif()

find_package(libobs REQUIRED)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/decklink/plugin-main.cpp
    src/decklink/decklink-output.cpp
    src/decklink/DecklinkBase.cpp
    src/decklink/DeckLinkOutput.cpp
    src/decklink/decklink-device-instance.cpp
    ${OBS_DECKLINK_SOURCE_DIR}/decklink-device-discovery.cpp
    ${OBS_DECKLINK_SOURCE_DIR}/decklink-device.cpp
    ${OBS_DECKLINK_SOURCE_DIR}/decklink-device-mode.cpp
    ${OBS_DECKLINK_SOURCE_DIR}/decklink-devices.cpp
    ${OBS_DECKLINK_SOURCE_DIR}/util.cpp
    ${OBS_DECKLINK_SOURCE_DIR}/win/platform.cpp
)

set(DECKLINK_IDL_FILES
  ${OBS_DECKLINK_SDK_DIR}/DeckLinkAPI.idl
  ${OBS_DECKLINK_SDK_DIR}/DeckLinkAPITypes.idl
  ${OBS_DECKLINK_SDK_DIR}/DeckLinkAPIModes.idl
  ${OBS_DECKLINK_SDK_DIR}/DeckLinkAPIDiscovery.idl
  ${OBS_DECKLINK_SDK_DIR}/DeckLinkAPIConfiguration.idl
  ${OBS_DECKLINK_SDK_DIR}/DeckLinkAPIDeckControl.idl
  ${OBS_DECKLINK_SDK_DIR}/DeckLinkAPIStreaming.idl
)

add_custom_command(
  OUTPUT
    ${CMAKE_CURRENT_BINARY_DIR}/DeckLinkAPI.h
    ${CMAKE_CURRENT_BINARY_DIR}/DeckLinkAPI_i.c
  COMMAND
    midl /nologo
      /h "${CMAKE_CURRENT_BINARY_DIR}/DeckLinkAPI.h"
      /iid "${CMAKE_CURRENT_BINARY_DIR}/DeckLinkAPI_i.c"
      /notlb
      /win64
      DeckLinkAPI.idl
  WORKING_DIRECTORY "${OBS_DECKLINK_SDK_DIR}"
  DEPENDS ${DECKLINK_IDL_FILES}
  COMMENT "Generating the pinned Blackmagic DeckLink COM interfaces"
  VERBATIM
)

set_source_files_properties(
  ${CMAKE_CURRENT_BINARY_DIR}/DeckLinkAPI.h
  PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/DeckLinkAPI.h
    ${CMAKE_CURRENT_BINARY_DIR}/DeckLinkAPI_i.c
)

target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decklink"
    "${OBS_DECKLINK_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    OBS::libobs
    ole32
    oleaut32
)

target_compile_definitions(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
