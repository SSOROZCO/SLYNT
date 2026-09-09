cmake_minimum_required(VERSION 3.21)

project(slynt
VERSION 0.1.0
LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# ============================================================

# Dependencias del sistema

# ============================================================

pkg_check_modules(GST REQUIRED
gstreamer-1.0
gstreamer-pbutils-1.0
)

pkg_check_modules(TAGLIB REQUIRED taglib)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# ============================================================

# Slint

#

# Slint se descarga automáticamente desde GitHub durante

# la configuración de CMake.

#

# SLYNT 0.1.0 Beta utiliza Slint 1.10.0.

# ============================================================

include(FetchContent)

FetchContent_Declare(
slint
GIT_REPOSITORY https://github.com/slint-ui/slint.git
GIT_TAG v1.10.0
GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(slint)

# ============================================================

# Ejecutable

# ============================================================

add_executable(slynt
src/main.cpp
src/player.cpp
src/core/library/library_store.cpp
src/core/library/library_bridge.cpp
)

# ============================================================

# Recursos Windows

# ============================================================

if(WIN32)
if(EXISTS
${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/windows/slynt.rc
)
target_sources(
slynt
PRIVATE
assets/icons/windows/slynt.rc
)
endif()
endif()

# ============================================================

# Archivos Slint

# ============================================================

slint_target_sources(
slynt
ui/main.slint
)

# ============================================================

# Includes

# ============================================================

target_include_directories(slynt PRIVATE
${GST_INCLUDE_DIRS}
${TAGLIB_INCLUDE_DIRS}
${SQLITE3_INCLUDE_DIRS}


src/
src/core/library/

${CMAKE_CURRENT_BINARY_DIR}


)

# ============================================================

# Librerías

# ============================================================

target_link_libraries(slynt PRIVATE
Slint::Slint


${GST_LIBRARIES}
${TAGLIB_LIBRARIES}
${SQLITE3_LIBRARIES}

pthread


)

# ============================================================

# Opciones de compilación

# ============================================================

target_compile_options(slynt PRIVATE
${GST_CFLAGS_OTHER}
${TAGLIB_CFLAGS_OTHER}
${SQLITE3_CFLAGS_OTHER}
)

# ============================================================

# RPATH Linux para librerías instaladas junto al ejecutable

# ============================================================

if(UNIX AND NOT APPLE)
set_target_properties(slynt PROPERTIES
INSTALL_RPATH "$ORIGIN/../lib"
BUILD_WITH_INSTALL_RPATH FALSE
)
endif()

# ============================================================

# Instalación del binario

# ============================================================

install(
TARGETS slynt
RUNTIME DESTINATION bin
)

# ============================================================

# Iconos Linux

# ============================================================

if(EXISTS
${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/hicolor/scalable/apps/slynt.svg
)
install(
FILES
assets/icons/hicolor/scalable/apps/slynt.svg
DESTINATION share/icons/hicolor/scalable/apps
)
endif()

foreach(size 16 24 32 48 64 128 256 512)


if(EXISTS
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/hicolor/${size}x${size}/apps/slynt.png
)

    install(
        FILES
        assets/icons/hicolor/${size}x${size}/apps/slynt.png
        DESTINATION share/icons/hicolor/${size}x${size}/apps
    )

endif()


endforeach()

# ============================================================

# Desktop file

# ============================================================

if(EXISTS
${CMAKE_CURRENT_SOURCE_DIR}/assets/slynt.desktop
)
install(
FILES
assets/slynt.desktop
DESTINATION share/applications
)
endif()

# ============================================================

# Icono de ventana

# ============================================================

configure_file(
${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/hicolor/256x256/apps/slynt.png
${CMAKE_CURRENT_BINARY_DIR}/slynt.png
COPYONLY
)

# ============================================================

# ============================================================
# Limpiar archivos de desarrollo de Slint del paquete runtime
# ============================================================

install(CODE [[
    file(REMOVE
        "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/slint-compiler"
    )

    file(REMOVE_RECURSE
        "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/include/slint"
        "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/Slint"
    )
]])

# Paquete Debian

#

# SLYNT Beta Release 0.1.0

# ============================================================

set(CPACK_GENERATOR "DEB")

set(CPACK_PACKAGE_NAME "slynt")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "SLYNT")

set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Hi-Fi Music Player")
set(CPACK_PACKAGE_DESCRIPTION
"SLYNT - Hi-Fi Music Player"
)

set(CPACK_PACKAGE_HOMEPAGE_URL "")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "SLYNT")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libgstreamer1.0-0, libgstreamer-plugins-base1.0-0, libglib2.0-0, libtag1v5, libsqlite3-0"
)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

include(CPack)
