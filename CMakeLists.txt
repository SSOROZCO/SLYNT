cmake_minimum_required(VERSION 3.21)
project(slynt LANGUAGES CXX)

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
