cmake_minimum_required(VERSION 3.21)

include(cmake/LpmMetadata.cmake)
project(${LILYGO_UI_CMAKE_PROJECT_NAME} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)
find_package(Threads REQUIRED)

configure_file(
    cmake/app_identity.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/app_identity.h
    @ONLY)

# Device packages use libraries from the target's standard multiarch paths.
# Do not preserve sysroot-derived runtime search paths in the root service.
set(CMAKE_SKIP_RPATH TRUE)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LilyGoUI CONFIG REQUIRED)
add_subdirectory(transport)

add_library(launcher_logging STATIC src/logging.cpp)
set_target_properties(launcher_logging PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(launcher_logging PUBLIC src)
target_compile_options(launcher_logging PRIVATE -Wall -Wextra -Wpedantic)

add_library(launcher_file_share STATIC src/file_share.cpp src/file_share_receiver.cpp)
set_target_properties(launcher_file_share PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(launcher_file_share PUBLIC src)
target_compile_definitions(launcher_file_share PRIVATE
    LAUNCHER_FILE_SHARE_AGENT="${CMAKE_INSTALL_PREFIX}/lib/lilygo-ui-launcher/file-share/lilygo-ui-file-share-agent")
target_compile_options(launcher_file_share PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(launcher_file_share PUBLIC Threads::Threads)
if(CM0_SIMULATOR)
    target_compile_definitions(launcher_file_share PRIVATE LAUNCHER_SIMULATOR=1)
endif()

add_library(launcher_core STATIC
    src/manifest.cpp
    src/registry.cpp
    src/process.cpp
    src/session.cpp)
target_include_directories(launcher_core PUBLIC src)
target_compile_options(launcher_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(launcher_core PUBLIC
    launcher_logging launcher_file_share LilyGoUI::SystemStatus)

add_library(launcher_setup STATIC
    src/setup_command.cpp src/setup_flow.cpp src/setup_region.cpp
    src/setup_network.cpp src/setup_update.cpp)
target_include_directories(launcher_setup PUBLIC src)
target_compile_options(launcher_setup PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(launcher_setup PUBLIC Threads::Threads)
if(CM0_SIMULATOR)
    target_compile_definitions(launcher_setup PUBLIC LAUNCHER_SIMULATOR=1)
endif()

add_executable(${LILYGO_UI_EXECUTABLE_NAME}
    src/main.cpp
    src/display_session.cpp
    src/keyboard_device.cpp
    src/power_button.cpp
    src/system_controls.cpp
    src/account_view.cpp
    src/setup_view.cpp
    src/first_boot_wizard.cpp
    src/ui.cpp
    src/launcher_view.cpp)
target_include_directories(${LILYGO_UI_EXECUTABLE_NAME} PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(${LILYGO_UI_EXECUTABLE_NAME} PRIVATE
    -Wall -Wextra -Wpedantic)
if(CM0_SIMULATOR)
    target_compile_definitions(launcher_core PRIVATE LAUNCHER_SIMULATOR=1)
    target_compile_definitions(${LILYGO_UI_EXECUTABLE_NAME} PRIVATE
        LAUNCHER_SIMULATOR=1)
endif()

if(NOT CM0_SIMULATOR)
    target_compile_definitions(launcher_core PRIVATE LAUNCHER_SYSTEM_SESSION=1)
    target_compile_definitions(${LILYGO_UI_EXECUTABLE_NAME} PRIVATE LAUNCHER_SYSTEM_SESSION=1)
    find_library(LAUNCHER_PAM_LIBRARY NAMES pam pam.so.0 libpam.so.0 REQUIRED)
    find_library(LAUNCHER_ALSA_LIBRARY NAMES asound REQUIRED)
    target_link_libraries(${LILYGO_UI_EXECUTABLE_NAME} PRIVATE ${LAUNCHER_ALSA_LIBRARY})
    target_link_libraries(launcher_core PUBLIC ${LAUNCHER_PAM_LIBRARY})
endif()
target_link_libraries(${LILYGO_UI_EXECUTABLE_NAME} PRIVATE
    launcher_core launcher_setup LilyGoUI::StatusBar LilyGoUI::UIFonts LilyGoUI::Input)

# Manual hardware harness is cross-buildable, but never runs automatically or
# becomes part of a published application.
if(BUILD_TESTING)
    add_executable(launcher-file-share-target tests/file_share_target.cpp)
    target_compile_options(launcher-file-share-target PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(launcher-file-share-target PRIVATE launcher_file_share)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(launcher-system-controls-tests tests/test_system_controls.cpp src/system_controls.cpp)
    target_include_directories(launcher-system-controls-tests PRIVATE src)
    target_compile_options(launcher-system-controls-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_library(LAUNCHER_TEST_ALSA_LIBRARY NAMES asound REQUIRED)
        target_link_libraries(launcher-system-controls-tests PRIVATE ${LAUNCHER_TEST_ALSA_LIBRARY})
    endif()
    add_test(NAME launcher-system-controls COMMAND launcher-system-controls-tests)

    add_executable(launcher-simulator-controls-tests tests/test_system_controls.cpp src/system_controls.cpp)
    target_include_directories(launcher-simulator-controls-tests PRIVATE src)
    target_compile_definitions(launcher-simulator-controls-tests PRIVATE LAUNCHER_SIMULATOR=1)
    target_compile_options(launcher-simulator-controls-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME launcher-simulator-controls COMMAND launcher-simulator-controls-tests)

    add_executable(launcher-manifest-tests tests/test_manifest.c src/manifest.cpp)
    target_include_directories(launcher-manifest-tests PRIVATE src)
    target_compile_options(launcher-manifest-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME launcher-manifest COMMAND launcher-manifest-tests)

    add_executable(launcher-registry-tests tests/test_registry.c)
    target_include_directories(launcher-registry-tests PRIVATE src)
    target_compile_options(launcher-registry-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-registry-tests PRIVATE launcher_core)
    add_executable(launcher-process-child tests/process_child.c)
    add_test(NAME launcher-registry COMMAND launcher-registry-tests
        "$<TARGET_FILE:launcher-process-child>")

    add_executable(launcher-process-tests tests/test_process.c src/process.cpp)
    target_include_directories(launcher-process-tests PRIVATE src)
    target_compile_options(launcher-process-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-process-tests PRIVATE launcher_logging)
    add_test(NAME launcher-process COMMAND launcher-process-tests
        "$<TARGET_FILE:launcher-process-child>")

    add_executable(launcher-process-identity-tests
        tests/test_process_identity.c src/process.cpp)
    target_include_directories(launcher-process-identity-tests PRIVATE src)
    target_compile_options(launcher-process-identity-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-process-identity-tests PRIVATE launcher_logging)
    add_test(NAME launcher-process-identity COMMAND launcher-process-identity-tests)

    add_executable(launcher-process-locale-tests tests/test_process_locale.cpp)
    target_compile_options(launcher-process-locale-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-process-locale-tests PRIVATE launcher_logging)
    add_test(NAME launcher-process-locale COMMAND launcher-process-locale-tests)

    add_executable(launcher-file-share-tests tests/test_file_share.cpp)
    target_include_directories(launcher-file-share-tests PRIVATE src)
    target_compile_options(launcher-file-share-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-file-share-tests PRIVATE launcher_file_share)
    add_test(NAME launcher-file-share COMMAND launcher-file-share-tests)

    add_executable(launcher-file-share-fake-agent tests/file_share_fake_agent.cpp)
    target_compile_options(launcher-file-share-fake-agent PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(launcher-file-share-receiver-tests tests/test_file_share_receiver.cpp)
    target_include_directories(launcher-file-share-receiver-tests PRIVATE src)
    target_compile_options(launcher-file-share-receiver-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-file-share-receiver-tests PRIVATE launcher_file_share)
    add_test(NAME launcher-file-share-receiver COMMAND launcher-file-share-receiver-tests
        "$<TARGET_FILE:launcher-file-share-fake-agent>")
    set_tests_properties(launcher-file-share-receiver PROPERTIES TIMEOUT 60)

    add_executable(launcher-session-tests tests/test_session.cpp src/session.cpp)
    target_include_directories(launcher-session-tests PRIVATE src)
    target_compile_options(launcher-session-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-session-tests PRIVATE launcher_logging)
    add_test(NAME launcher-session COMMAND launcher-session-tests)

    if(CM0_SIMULATOR)
        add_executable(launcher-simulator-session-tests
            tests/test_simulator_session.cpp src/session.cpp)
        target_include_directories(launcher-simulator-session-tests PRIVATE src)
        target_compile_definitions(launcher-simulator-session-tests PRIVATE LAUNCHER_SIMULATOR=1)
        target_compile_options(launcher-simulator-session-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        target_link_libraries(launcher-simulator-session-tests PRIVATE launcher_logging)
        add_test(NAME launcher-simulator-session COMMAND launcher-simulator-session-tests)
    endif()

    add_executable(launcher-session-pam-tests tests/test_session_pam.cpp)
    target_include_directories(launcher-session-pam-tests PRIVATE src)
    target_compile_options(launcher-session-pam-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME launcher-session-pam COMMAND launcher-session-pam-tests)

    add_executable(launcher-account-view-tests tests/test_account_view.cpp src/account_view.cpp)
    target_include_directories(launcher-account-view-tests PRIVATE src)
    target_compile_options(launcher-account-view-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-account-view-tests PRIVATE LilyGoUI::UIFonts)
    add_test(NAME launcher-account-view COMMAND launcher-account-view-tests)

    if(CM0_SIMULATOR)
        add_executable(launcher-first-boot-wizard-tests
            tests/test_first_boot_wizard.cpp src/first_boot_wizard.cpp
            src/account_view.cpp src/setup_view.cpp)
        target_compile_options(launcher-first-boot-wizard-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        target_link_libraries(launcher-first-boot-wizard-tests PRIVATE
            launcher_core launcher_setup LilyGoUI::UIFonts)
        add_test(NAME launcher-first-boot-wizard COMMAND launcher-first-boot-wizard-tests)
    endif()

    add_executable(launcher-setup-view-tests tests/test_setup_view.cpp src/setup_view.cpp)
    target_include_directories(launcher-setup-view-tests PRIVATE src)
    target_compile_options(launcher-setup-view-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-setup-view-tests PRIVATE LilyGoUI::UIFonts)
    add_test(NAME launcher-setup-view COMMAND launcher-setup-view-tests)

    foreach(component command flow region network update)
        add_executable(launcher-setup-${component}-tests tests/test_setup_${component}.cpp)
        target_compile_options(launcher-setup-${component}-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        target_link_libraries(launcher-setup-${component}-tests PRIVATE launcher_setup)
        add_test(NAME launcher-setup-${component} COMMAND launcher-setup-${component}-tests)
    endforeach()
    add_test(NAME launcher-setup-update-helper
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup_update.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/lilygo-ui-launcher-first-boot-update")

    add_executable(launcher-keyboard-device-tests
        tests/test_keyboard_device.cpp src/keyboard_device.cpp)
    target_include_directories(launcher-keyboard-device-tests PRIVATE src)
    target_compile_options(launcher-keyboard-device-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-keyboard-device-tests PRIVATE launcher_logging)
    add_test(NAME launcher-keyboard-device COMMAND launcher-keyboard-device-tests)

    add_test(NAME launcher-display-session
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_display_session.sh"
            "${CMAKE_CXX_COMPILER}")

    add_executable(launcher-render-ui
        tests/render_ui.c src/ui.cpp src/launcher_view.cpp)
    target_include_directories(launcher-render-ui PRIVATE src)
    target_compile_definitions(launcher-render-ui PRIVATE
        LAUNCHER_TEST_APP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/apps")
    target_compile_options(launcher-render-ui PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(launcher-render-ui PRIVATE
        launcher_core LilyGoUI::StatusBar LilyGoUI::UIFonts)
    add_test(NAME launcher-render-ui COMMAND launcher-render-ui
        "${CMAKE_CURRENT_BINARY_DIR}/launcher-home.ppm"
        "${CMAKE_CURRENT_BINARY_DIR}/launcher-quick-controls.ppm"
        "${CMAKE_CURRENT_BINARY_DIR}/launcher-home-landscape.ppm"
        "${CMAKE_CURRENT_BINARY_DIR}/launcher-home-compact.ppm"
        "${CMAKE_CURRENT_BINARY_DIR}/launcher-quick-controls-compact.ppm"
        "${CMAKE_CURRENT_BINARY_DIR}/launcher-file-sharing.ppm"
        "${CMAKE_CURRENT_BINARY_DIR}/launcher-file-sharing-compact.ppm")

    add_test(NAME launcher-postinst
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_postinst.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm")

    add_test(NAME launcher-update-helper
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_helper.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/lilygo-ui-launcher-update"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/lilygo-ui-launcher-update.service")

    add_test(NAME launcher-service-unit
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_service_unit.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/lilygo-ui-launcher.service"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/userconfig.service.d/50-lilygo-ui-launcher.conf")

    add_test(NAME launcher-first-boot
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_first_boot.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/lilygo-ui-launcher-first-boot")

    add_test(NAME launcher-cli
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.sh"
            "$<TARGET_FILE:${LILYGO_UI_EXECUTABLE_NAME}>")

endif()

install(TARGETS ${LILYGO_UI_EXECUTABLE_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT launcher)
install(FILES packaging/systemd/lilygo-ui-launcher.service
              packaging/systemd/lilygo-ui-launcher-update.service
              packaging/systemd/lilygo-ui-launcher-first-boot-update.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
        COMPONENT launcher)
install(FILES packaging/systemd/userconfig.service.d/50-lilygo-ui-launcher.conf
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system/userconfig.service.d
        COMPONENT launcher)
install(PROGRAMS packaging/systemd/lilygo-ui-launcher-console
                 packaging/systemd/lilygo-ui-launcher-first-boot
                 packaging/systemd/lilygo-ui-launcher-first-boot-update
                 packaging/systemd/lilygo-ui-launcher-update
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/${LILYGO_UI_PACKAGE_NAME}
        COMPONENT launcher)
install(FILES packaging/pam/lilygo-ui-launcher
        DESTINATION /etc/pam.d
        COMPONENT launcher)
lilygo_ui_install_font_licenses(
    "${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}" launcher)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${LILYGO_UI_VERSION}")
if(LILYGO_UI_VERSION MATCHES
   "^([0-9]+:)?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_2}")
    set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_3}")
    set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_4}")
else()
    set(CPACK_PACKAGE_VERSION_MAJOR 0)
    set(CPACK_PACKAGE_VERSION_MINOR 0)
    set(CPACK_PACKAGE_VERSION_PATCH 0)
endif()
set(CPACK_PACKAGE_CONTACT "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_VENDOR "${LILYGO_UI_PACKAGE_VENDOR}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${LILYGO_UI_APP_SUMMARY}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${LILYGO_UI_APP_HOMEPAGE}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL launcher)
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_DEBIAN_LAUNCHER_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_LAUNCHER_DESCRIPTION "${LILYGO_UI_APP_SUMMARY}")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_DEPENDS
    "libc6, libstdc++6, libdrm2, libfreetype6, libpng16-16t64, zlib1g, libbz2-1.0, libbrotli1, libpam0g, libpam-modules, libasound2t64 | libasound2, libglib2.0-0t64, libssl3t64, passwd, sudo, raspi-config, locales, tzdata, keyboard-configuration, xkb-data, network-manager, apt, util-linux, bluez, lilygo-ui-appkit-dev (>= ${LILYGO_UI_MIN_APPKIT_VERSION}), systemd, dpkg, coreutils")
if(LILYGO_UI_GRAPHICS_DEBIAN_DEPENDS)
    string(APPEND CPACK_DEBIAN_LAUNCHER_PACKAGE_DEPENDS
        ", ${LILYGO_UI_GRAPHICS_DEBIAN_DEPENDS}")
endif()
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_PROVIDES
    "cm0-launcher (= ${LILYGO_UI_VERSION})")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_CONFLICTS "cm0-launcher")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_REPLACES "cm0-launcher")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_CONTROL_STRICT_PERMISSION ON)
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "required")
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        "${LILYGO_UI_PACKAGE_ARCHITECTURE}")
endif()
include(CPack)
