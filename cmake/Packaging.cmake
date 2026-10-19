# CPack: the release tarball and .deb. Both
# hold the same install tree and use the system's libraries (the Debian
# release built on, x86_64); only ncnn is bundled, in lib/orbis-reframe/. Built by
# scripts/package.sh, which passes the git-describe version
# (CPACK_PACKAGE_VERSION) and, with ORBIS_MAINTAINER set, another maintainer
# (CPACK_PACKAGE_CONTACT).
#
#   orbis-reframe-<version>-<target>-x86_64.tar.xz
#     orbis-reframe-<version>-<target>-x86_64/
#       orbis-reframe            launcher (packaging/tarball/), runs bin/...
#       bin/ lib/ share/
#   orbis-reframe_<version>[~deb13]_amd64.deb   the same tree under /usr
#                                               (scripts/package.sh renames ~ to .)
#
# <target> (debian-testing, debian-13) is ORBIS_PACKAGE_TARGET, below.

set(CPACK_PACKAGE_NAME orbis-reframe)
set(CPACK_PACKAGE_VENDOR orbis-reframe)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Video editor for Insta360 X5 360-degree footage")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
# The .deb's Maintainer field.
set(CPACK_PACKAGE_CONTACT "Markku Kokko <orbis-reframe@techmonkeys.se>")
set(CPACK_GENERATOR TXZ DEB)
set(CPACK_STRIP_FILES ON)
set(CPACK_THREADS 0) # xz on every core
# 755 directories whatever the builder's umask (a group-writable
# /usr/lib/orbis-reframe from a umask-002 build otherwise).
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# Distribution + architecture in the tarball's name: the binary only runs on
# the distribution it was built for (docs/manual/getting-started.md). The
# build machine's, as scripts/targets.sh names it: a numbered Debian release
# is debian-<n>, Testing (no VERSION_ID) debian-testing, Ubuntu ubuntu-<n.n>.
set(ORBIS_HOST_TARGET debian-testing)
if(EXISTS /etc/os-release)
    file(STRINGS /etc/os-release ORBIS_OS_ID REGEX "^ID=")
    file(STRINGS /etc/os-release ORBIS_OS_VERSION_ID REGEX "^VERSION_ID=")
    if(ORBIS_OS_ID STREQUAL "ID=debian" AND ORBIS_OS_VERSION_ID MATCHES "^VERSION_ID=\"?([0-9]+)\"?$")
        set(ORBIS_HOST_TARGET debian-${CMAKE_MATCH_1})
    elseif(ORBIS_OS_ID STREQUAL "ID=ubuntu" AND ORBIS_OS_VERSION_ID MATCHES "^VERSION_ID=\"?([0-9.]+)\"?$")
        set(ORBIS_HOST_TARGET ubuntu-${CMAKE_MATCH_1})
    endif()
endif()
set(ORBIS_PACKAGE_TARGET ${ORBIS_HOST_TARGET} CACHE STRING
    "Distribution the packages are built for -- goes into the tarball's name")
# A numbered release's .deb gets a backport-style version, 0.1.0~deb13: sorts
# below Testing's 0.1.0, so a system upgraded to Testing takes that .deb as
# an upgrade, and the two .deb files don't share a name. Ubuntu's the same
# way: 0.1.0~ubuntu26.04.
set(ORBIS_DEB_VERSION_SUFFIX "")
if(ORBIS_PACKAGE_TARGET MATCHES "^debian-([0-9]+)$")
    set(ORBIS_DEB_VERSION_SUFFIX "~deb${CMAKE_MATCH_1}")
elseif(ORBIS_PACKAGE_TARGET MATCHES "^ubuntu-([0-9.]+)$")
    set(ORBIS_DEB_VERSION_SUFFIX "~ubuntu${CMAKE_MATCH_1}")
endif()
# The name and the tarball's top-level launcher (packaging/tarball/, TXZ
# only) are set at cpack time, after the version -- packaging/CPackOptions.cmake.in.
# Copied, with fixed permissions, to a directory of its own.
set(ORBIS_TARBALL_EXTRA_DIR ${CMAKE_CURRENT_BINARY_DIR}/packaging-tarball)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/packaging/tarball/orbis-reframe
    DESTINATION ${ORBIS_TARBALL_EXTRA_DIR}
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                     WORLD_READ WORLD_EXECUTE)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPackOptions.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CPackOptions.cmake @ONLY)
set(CPACK_PROJECT_CONFIG_FILE ${CMAKE_CURRENT_BINARY_DIR}/CPackOptions.cmake)

# --- .deb
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT) # orbis-reframe_<deb version>_amd64.deb
set(CPACK_DEBIAN_PACKAGE_SECTION video)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"Video editor for Insta360 X5 360-degree footage
 Opens the camera's .insv files directly: stitching, stabilization, reframing
 to a flat video, subject tracking, privacy blur, audio clean-up and export.
 Needs a Wayland desktop and an NVIDIA graphics card (Vulkan, NVENC).")
# Linked libraries come from dpkg-shlibdeps (it follows the RUNPATH into the
# bundled ncnn, so libgomp1 is found too). It can't see what is loaded at run
# time: Qt's Wayland platform and SVG icon plugins, and ONNX Runtime
# (dlopen()ed for the voice modes, insv::VoiceIsolator).
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# ORBIS_ORT_PACKAGE (CMakeLists.txt) is the ONNX Runtime built against.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-wayland, qt6-svg-plugins")
if(ORBIS_ORT_PACKAGE)
    string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", ${ORBIS_ORT_PACKAGE}")
endif()
# The graphics drivers only as suggestions: a driver installed from NVIDIA's
# .run installer has no Debian packages and would conflict with them. No
# NVIDIA packages on Ubuntu: its driver packages carry the driver version in
# their name (nvidia-driver-595), which would go out of date. Mesa's drivers
# (AMD, Intel) are named the same everywhere; on Ubuntu 26.04
# mesa-libgallium provides mesa-va-drivers.
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "mesa-vulkan-drivers, mesa-va-drivers | intel-media-va-driver")
if(NOT ORBIS_PACKAGE_TARGET MATCHES "^ubuntu-")
    string(PREPEND CPACK_DEBIAN_PACKAGE_SUGGESTS "nvidia-driver, nvidia-vulkan-icd, libnvidia-encode1, ")
endif()

include(CPack)
