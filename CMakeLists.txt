cmake_minimum_required(VERSION 3.21)
project(lilygo_ui_launcher VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)
find_package(Threads REQUIRED)

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

add_library(launcher_logging STATIC src/logging.cpp)
set_target_properties(launcher_logging PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(launcher_logging PUBLIC src)
target_compile_options(launcher_logging PRIVATE -Wall -Wextra -Wpedantic)

add_library(launcher_core STATIC
    src/manifest.cpp
    src/registry.cpp
    src/process.cpp)
target_include_directories(launcher_core PUBLIC src)
target_compile_options(launcher_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(launcher_core PUBLIC
    launcher_logging LilyGoUI::SystemStatus)

add_executable(lilygo-ui-launcher
    src/main.cpp
    src/ui.cpp
    src/launcher_view.cpp)
target_include_directories(lilygo-ui-launcher PRIVATE src)
target_compile_options(lilygo-ui-launcher PRIVATE -Wall -Wextra -Wpedantic)
if(CM0_SIMULATOR)
    target_compile_definitions(lilygo-ui-launcher PRIVATE LAUNCHER_SIMULATOR=1)
endif()
target_link_libraries(lilygo-ui-launcher PRIVATE
    launcher_core LilyGoUI::StatusBar LilyGoUI::UIFonts)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
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
        "${CMAKE_CURRENT_BINARY_DIR}/launcher-home-landscape.ppm")

    add_test(NAME launcher-postinst
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_postinst.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm")

endif()

install(TARGETS lilygo-ui-launcher RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT launcher)
install(FILES packaging/systemd/lilygo-ui-launcher.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
        COMPONENT launcher)
install(PROGRAMS packaging/systemd/lilygo-ui-launcher-console
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/lilygo-ui-launcher
        COMPONENT launcher)
lilygo_ui_install_font_licenses(
    "${CMAKE_INSTALL_DATAROOTDIR}/doc/lilygo-ui-launcher" launcher)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGE_NAME lilygo-ui-launcher)
set(CPACK_PACKAGE_CONTACT "LilyGo")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LILYGO UI application launcher")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL launcher)
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_NAME "lilygo-ui-launcher")
set(CPACK_DEBIAN_LAUNCHER_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_LAUNCHER_DESCRIPTION "LILYGO UI application launcher")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_DEPENDS
    "libc6, libstdc++6, libdrm2")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_PROVIDES "cm0-launcher")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_CONFLICTS "cm0-launcher")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_REPLACES "cm0-launcher")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm")
set(CPACK_DEBIAN_LAUNCHER_PACKAGE_CONTROL_STRICT_PERMISSION ON)
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
endif()
include(CPack)
