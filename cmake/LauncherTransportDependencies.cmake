include_guard(GLOBAL)

# The pinned BSP has the target runtime libraries, but omits the development
# files needed by the file-sharing helper. Keep those files in a separate cache.
function(_launcher_extract_transport_package mirror path sha256 destination)
    get_filename_component(_name "${path}" NAME)
    set(_archive "${_cache}/archives/${_name}")
    set(_actual_hash "")
    if(EXISTS "${_archive}")
        file(SHA256 "${_archive}" _actual_hash)
    endif()
    if(NOT _actual_hash STREQUAL sha256)
        message(STATUS "Downloading ARM64 transport dependency ${_name}")
        file(DOWNLOAD "${mirror}/${path}" "${_archive}"
            TLS_VERIFY ON TIMEOUT 180 INACTIVITY_TIMEOUT 60
            STATUS _status)
        list(GET _status 0 _code)
        list(GET _status 1 _message)
        if(NOT _code EQUAL 0)
            file(REMOVE "${_archive}")
            message(FATAL_ERROR "Cannot download ${_name}: ${_message}")
        endif()
        file(SHA256 "${_archive}" _actual_hash)
        if(NOT _actual_hash STREQUAL sha256)
            file(REMOVE "${_archive}")
            message(FATAL_ERROR "SHA-256 mismatch for ${_name}")
        endif()
    endif()

    # CMake extracts both Debian's ar container and its compressed data tarball,
    # so macOS build hosts do not need dpkg or target-side package installation.
    set(_unpack "${_cache}/package.extracting")
    file(REMOVE_RECURSE "${_unpack}")
    file(MAKE_DIRECTORY "${_unpack}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_unpack}")
    file(GLOB _payloads "${_unpack}/data.tar.*")
    list(LENGTH _payloads _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "No unique data archive in ${_name}")
    endif()
    list(GET _payloads 0 _payload)
    file(ARCHIVE_EXTRACT INPUT "${_payload}" DESTINATION "${destination}")
    file(REMOVE_RECURSE "${_unpack}")
endfunction()

function(launcher_prepare_transport_dependencies)
    # Custom SDKs and native builds supply their own matching development files.
    if(NOT CMAKE_CROSSCOMPILING OR NOT CM0_BSP_VERSION STREQUAL "0.1.0" OR
       NOT CM0_BSP_CACHE_DIR)
        return()
    endif()
    set(_multiarch aarch64-linux-gnu)
    set(_bsp_libdir "${CMAKE_SYSROOT}/usr/lib/${_multiarch}")
    if(EXISTS "${CMAKE_SYSROOT}/usr/include/openssl/ssl.h" AND
       EXISTS "${_bsp_libdir}/libssl.so" AND
       EXISTS "${_bsp_libdir}/pkgconfig/gio-unix-2.0.pc")
        return()
    endif()

    set(_cache "${CM0_BSP_CACHE_DIR}/transport/trixie-arm64-v1")
    set(_root "${_cache}/sysroot")
    file(SHA256 "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" _manifest_hash)
    # Runtime links in this overlay belong to this particular BSP cache.
    string(SHA256 _manifest_hash "${_manifest_hash};${CMAKE_SYSROOT}")
    file(MAKE_DIRECTORY "${_cache}/archives")
    file(LOCK "${_cache}" DIRECTORY GUARD FUNCTION TIMEOUT 180)
    set(_installed_hash "")
    if(EXISTS "${_root}/.manifest-sha256")
        file(READ "${_root}/.manifest-sha256" _installed_hash)
    endif()
    if(NOT _installed_hash STREQUAL _manifest_hash)
        set(_staging "${_cache}/sysroot.extracting")
        file(REMOVE_RECURSE "${_staging}")
        file(MAKE_DIRECTORY "${_staging}")
        # GLib 2.84.4 and OpenSSL 3.5 development files match the BSP's ABI.
        _launcher_extract_transport_package(
            "https://deb.debian.org/debian"
            "pool/main/g/glib2.0/libgio-2.0-dev_2.84.4-3~deb13u5_arm64.deb"
            "1fe9975a28332e4606eab301902e8ab445365ee0ad7c3ddeba56cd0f658fadad" "${_staging}")
        _launcher_extract_transport_package(
            "https://archive.raspberrypi.com/debian"
            "pool/main/o/openssl/libssl-dev_3.5.7-1~deb13u3+rpt1_arm64.deb"
            "696b24efbcb13b3f6654709914a2449fc3c18d6d7d08f2149e2a44e38eb29e20" "${_staging}")
        _launcher_extract_transport_package(
            "https://deb.debian.org/debian"
            "pool/main/s/sysprof/libsysprof-capture-4-dev_48.0-2_arm64.deb"
            "73f30f2a5ff7b378f8d47f1a3e1fcfac74043a4612bf0661c3ea4b96e6e27f98" "${_staging}")
        _launcher_extract_transport_package(
            "https://deb.debian.org/debian"
            "pool/main/z/zlib/zlib1g-dev_1.3.dfsg+really1.3.1-1+b1_arm64.deb"
            "745ebcf1fa115e230af76216355d7290a24f1790fe3780249b519c0e4d30534f" "${_staging}")

        set(_libraries ssl crypto gio-2.0 glib-2.0 gobject-2.0 gmodule-2.0 z)
        set(_sonames 3 3 0 0 0 0 1)
        foreach(_library _soname IN ZIP_LISTS _libraries _sonames)
            set(_runtime "${_bsp_libdir}/lib${_library}.so.${_soname}")
            if(NOT EXISTS "${_runtime}")
                message(FATAL_ERROR "Pinned BSP runtime is missing: ${_runtime}")
            endif()
            set(_link "${_staging}/usr/lib/${_multiarch}/lib${_library}.so")
            file(REMOVE "${_link}")
            file(CREATE_LINK "${_runtime}" "${_link}" SYMBOLIC)
        endforeach()
        foreach(_required IN ITEMS usr/include/openssl/ssl.h
                usr/include/${_multiarch}/openssl/opensslconf.h
                usr/include/glib-2.0/glib.h
                usr/include/gio-unix-2.0/gio/gunixfdmessage.h
                usr/lib/${_multiarch}/glib-2.0/include/glibconfig.h
                usr/lib/${_multiarch}/pkgconfig/gio-2.0.pc
                usr/lib/${_multiarch}/pkgconfig/sysprof-capture-4.pc
                usr/lib/${_multiarch}/pkgconfig/zlib.pc)
            if(NOT EXISTS "${_staging}/${_required}")
                message(FATAL_ERROR "Incomplete transport development files: ${_required}")
            endif()
        endforeach()
        file(WRITE "${_staging}/.manifest-sha256" "${_manifest_hash}")
        file(REMOVE_RECURSE "${_root}")
        file(RENAME "${_staging}" "${_root}")
    endif()

    # pkg-config must resolve overlay headers and libraries without prepending
    # the BSP sysroot a second time. Other .pc dependencies stay in the BSP.
    file(RELATIVE_PATH _relative_root "${CMAKE_SYSROOT}" "${_root}")
    set(_pkgdir "${_cache}/pkgconfig")
    file(MAKE_DIRECTORY "${_pkgdir}")
    file(GLOB _modules "${_root}/usr/lib/${_multiarch}/pkgconfig/*.pc")
    foreach(_module IN LISTS _modules)
        get_filename_component(_name "${_module}" NAME)
        file(READ "${_module}" _pc)
        string(REPLACE "prefix=/usr" "prefix=\${pc_sysrootdir}/${_relative_root}/usr" _pc "${_pc}")
        file(WRITE "${_pkgdir}/${_name}" "${_pc}")
    endforeach()
    set(ENV{PKG_CONFIG_PATH} "${_pkgdir}:$ENV{PKG_CONFIG_PATH}")
    list(PREPEND CMAKE_FIND_ROOT_PATH "${_root}")
    set(CMAKE_FIND_ROOT_PATH "${CMAKE_FIND_ROOT_PATH}" PARENT_SCOPE)
    message(STATUS "Launcher: using cached ARM64 transport development files")
endfunction()
