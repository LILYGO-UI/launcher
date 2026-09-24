set(_LILYGO_UI_PROJECT_CONFIG
    "${CMAKE_CURRENT_LIST_DIR}/../lpm.toml")
if(NOT EXISTS "${_LILYGO_UI_PROJECT_CONFIG}")
    message(FATAL_ERROR
        "Missing LPM project configuration: ${_LILYGO_UI_PROJECT_CONFIG}")
endif()

if(LPM_EXECUTABLE AND NOT EXISTS "${LPM_EXECUTABLE}")
    unset(LPM_EXECUTABLE CACHE)
    unset(LPM_EXECUTABLE)
endif()
find_program(LPM_EXECUTABLE NAMES lpm
    HINTS "${CMAKE_CURRENT_LIST_DIR}/../.venv/bin"
    DOC "LILYGO package manager executable")
if(NOT LPM_EXECUTABLE)
    message(FATAL_ERROR
        "lpm is required to read project metadata; install lilygo-lpm or set LPM_EXECUTABLE")
endif()

execute_process(
    COMMAND "${LPM_EXECUTABLE}" metadata --format cmake
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    RESULT_VARIABLE _LILYGO_UI_METADATA_RESULT
    OUTPUT_VARIABLE _LILYGO_UI_METADATA_CONTENT
    ERROR_VARIABLE _LILYGO_UI_METADATA_ERROR)
if(NOT _LILYGO_UI_METADATA_RESULT EQUAL 0)
    string(STRIP "${_LILYGO_UI_METADATA_ERROR}" _LILYGO_UI_METADATA_ERROR)
    message(FATAL_ERROR
        "Could not read lpm.toml with lpm "
        "(${_LILYGO_UI_METADATA_RESULT}): ${_LILYGO_UI_METADATA_ERROR}")
endif()

set(_LILYGO_UI_METADATA_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_LILYGO_UI_METADATA_FILE
    "${_LILYGO_UI_METADATA_DIR}/lpm_metadata.cmake")
file(MAKE_DIRECTORY "${_LILYGO_UI_METADATA_DIR}")
file(WRITE "${_LILYGO_UI_METADATA_FILE}"
    "${_LILYGO_UI_METADATA_CONTENT}")
include("${_LILYGO_UI_METADATA_FILE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_LILYGO_UI_PROJECT_CONFIG}")

# LPM's CMake output exposes only the first author's name. Debian packages
# require a complete RFC 822 maintainer contact, so read the same validated
# metadata as JSON and retain the email without introducing another source.
execute_process(
    COMMAND "${LPM_EXECUTABLE}" metadata --format publish-json
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    RESULT_VARIABLE _LILYGO_UI_PUBLISH_RESULT
    OUTPUT_VARIABLE _LILYGO_UI_PUBLISH_JSON
    ERROR_VARIABLE _LILYGO_UI_PUBLISH_ERROR)
if(NOT _LILYGO_UI_PUBLISH_RESULT EQUAL 0)
    string(STRIP "${_LILYGO_UI_PUBLISH_ERROR}" _LILYGO_UI_PUBLISH_ERROR)
    message(FATAL_ERROR
        "Could not read package contact with lpm "
        "(${_LILYGO_UI_PUBLISH_RESULT}): ${_LILYGO_UI_PUBLISH_ERROR}")
endif()
string(JSON LILYGO_UI_PACKAGE_VENDOR ERROR_VARIABLE _LILYGO_UI_AUTHOR_ERROR
    GET "${_LILYGO_UI_PUBLISH_JSON}" authors 0 name)
if(NOT _LILYGO_UI_AUTHOR_ERROR STREQUAL "NOTFOUND")
    message(FATAL_ERROR
        "Could not read the first project author's name: ${_LILYGO_UI_AUTHOR_ERROR}")
endif()
string(JSON _LILYGO_UI_PACKAGE_EMAIL ERROR_VARIABLE _LILYGO_UI_EMAIL_ERROR
    GET "${_LILYGO_UI_PUBLISH_JSON}" authors 0 email)
if(NOT _LILYGO_UI_EMAIL_ERROR STREQUAL "NOTFOUND")
    message(FATAL_ERROR
        "The first project author must define an email for Debian packages")
endif()
set(LILYGO_UI_PACKAGE_CONTACT
    "${LILYGO_UI_PACKAGE_VENDOR} <${_LILYGO_UI_PACKAGE_EMAIL}>")

function(_lilygo_ui_escape_c output input)
    set(_value "${input}")
    string(REPLACE "\\" "\\\\" _value "${_value}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    string(REPLACE "\r" "\\r" _value "${_value}")
    string(REPLACE "\n" "\\n" _value "${_value}")
    string(REPLACE "\t" "\\t" _value "${_value}")
    set(${output} "${_value}" PARENT_SCOPE)
endfunction()

_lilygo_ui_escape_c(LILYGO_UI_APP_ID_C "${LILYGO_UI_APP_ID}")
_lilygo_ui_escape_c(LILYGO_UI_APP_NAME_C "${LILYGO_UI_APP_NAME}")
