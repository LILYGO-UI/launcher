cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(LilyGoLauncherTransport LANGUAGES C CXX)
    include(GNUInstallDirs)
    include(CTest)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/LauncherTransportDependencies.cmake")
launcher_prepare_transport_dependencies()
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
# Homebrew cross compilers do not search Debian's multiarch include directory
# automatically. OpenSSL keeps its target configuration headers there.
if(CMAKE_CROSSCOMPILING AND CMAKE_LIBRARY_ARCHITECTURE AND
   EXISTS "${OPENSSL_INCLUDE_DIR}/${CMAKE_LIBRARY_ARCHITECTURE}/openssl/opensslconf.h")
    foreach(_openssl_target IN ITEMS OpenSSL::SSL OpenSSL::Crypto)
        target_include_directories(${_openssl_target} INTERFACE
            "${OPENSSL_INCLUDE_DIR}/${CMAKE_LIBRARY_ARCHITECTURE}")
    endforeach()
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(LAUNCHER_GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

function(lilygo_transport_dependencies)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_SHARED_DEPS_LIBS OFF)
    set(NO_MEDIA ON)
    set(NO_WEBSOCKET ON)
    set(NO_EXAMPLES ON)
    set(NO_TESTS ON)
    set(PREFER_SYSTEM_LIB OFF)
    set(USE_NICE OFF)
    set(USE_GNUTLS OFF)
    set(USE_MBEDTLS OFF)
    set(USE_SYSTEM_JUICE OFF)
    set(USE_SYSTEM_USRSCTP OFF)
    set(USE_SYSTEM_PLOG OFF)
    FetchContent_Declare(lilygo_datachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG 9e6a13abbb6846c003d817d0387b6706466e2b03
        GIT_SUBMODULES deps/json deps/libjuice deps/plog deps/usrsctp
        GIT_SUBMODULES_RECURSE FALSE)
    FetchContent_MakeAvailable(lilygo_datachannel)
    set_property(DIRECTORY "${lilygo_datachannel_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
    add_library(launcher_transport_json INTERFACE)
    target_include_directories(launcher_transport_json SYSTEM INTERFACE
        "${lilygo_datachannel_SOURCE_DIR}/deps/json/single_include")
    set(LAUNCHER_DATACHANNEL_SOURCE_DIR "${lilygo_datachannel_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

lilygo_transport_dependencies()

add_library(launcher_transport_core STATIC
    ble_framing.cpp storage_ipc.cpp webrtc_session.cpp)
target_include_directories(launcher_transport_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(launcher_transport_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(launcher_transport_core PUBLIC
    launcher_transport_json LibDataChannel::LibDataChannel Threads::Threads)

add_executable(lilygo-ui-file-share-agent main.cpp bluez_agent.cpp)
target_compile_options(lilygo-ui-file-share-agent PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo-ui-file-share-agent PRIVATE
    launcher_transport_core PkgConfig::LAUNCHER_GIO)

install(TARGETS lilygo-ui-file-share-agent
    RUNTIME DESTINATION "lib/lilygo-ui-launcher/file-share"
    COMPONENT launcher)
install(FILES NOTICE.md
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/lilygo-ui-launcher/transport"
    COMPONENT launcher)
foreach(dependency libdatachannel libjuice usrsctp plog json)
    if(dependency STREQUAL "libdatachannel")
        set(license_dir "${LAUNCHER_DATACHANNEL_SOURCE_DIR}")
    else()
        set(license_dir "${LAUNCHER_DATACHANNEL_SOURCE_DIR}/deps/${dependency}")
    endif()
    file(GLOB license_files LIST_DIRECTORIES FALSE "${license_dir}/LICENSE*" "${license_dir}/COPYING*")
    install(FILES ${license_files}
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/lilygo-ui-launcher/transport/${dependency}"
        COMPONENT launcher)
endforeach()
file(STRINGS "${LAUNCHER_DATACHANNEL_SOURCE_DIR}/deps/json/single_include/nlohmann/json.hpp"
    json_copyright_lines ENCODING UTF-8 REGEX "^// SPDX-FileCopyrightText:")
list(REMOVE_DUPLICATES json_copyright_lines)
string(REPLACE ";" "\n" json_copyright_text "${json_copyright_lines}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/json-COPYRIGHT" "${json_copyright_text}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/json-COPYRIGHT"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/lilygo-ui-launcher/transport/json"
    RENAME COPYRIGHT COMPONENT launcher)

if(BUILD_TESTING)
    add_executable(launcher-transport-tests ../tests/test_transport.cpp)
    target_compile_options(launcher-transport-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-transport-tests PRIVATE launcher_transport_core OpenSSL::Crypto)
    add_test(NAME launcher-transport COMMAND launcher-transport-tests)
    set_tests_properties(launcher-transport PROPERTIES TIMEOUT 90)

    add_executable(launcher-bluez-agent-tests ../tests/test_bluez_agent.cpp)
    target_compile_options(launcher-bluez-agent-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-bluez-agent-tests PRIVATE launcher_transport_core PkgConfig::LAUNCHER_GIO)
    add_test(NAME launcher-bluez-agent COMMAND launcher-bluez-agent-tests $<TARGET_FILE:lilygo-ui-file-share-agent>)
    set_tests_properties(launcher-bluez-agent PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
