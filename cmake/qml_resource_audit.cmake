# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

# Every QML file under src/ui/qt/qml/ must be listed in the qt_add_resources()
# FILES block that builds the :/dsdneo prefix.
#
# Nothing else catches a missing entry. The Qt Quick Test suite loads screens
# from the SOURCE directory (DSD_QML_UI_DIR), so an unregistered component
# resolves there and every test passes; the app loads from qrc, so the same
# component is simply not a type and the screen importing it fails to load
# outright. That is a green CI and a blank screen on the device.

get_filename_component(_DSD_NEO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_DSD_NEO_QML_DIR "${_DSD_NEO_ROOT}/src/ui/qt/qml")
set(_DSD_NEO_QML_CMAKE "${_DSD_NEO_ROOT}/src/ui/qt/CMakeLists.txt")

if(NOT EXISTS "${_DSD_NEO_QML_CMAKE}")
    message(FATAL_ERROR "QML_RESOURCE_AUDIT: ${_DSD_NEO_QML_CMAKE} is missing")
endif()

file(READ "${_DSD_NEO_QML_CMAKE}" _DSD_NEO_QML_CMAKE_TEXT)

file(
    GLOB _DSD_NEO_QML_FILES
    LIST_DIRECTORIES FALSE
    "${_DSD_NEO_QML_DIR}/*.qml"
    "${_DSD_NEO_QML_DIR}/*.js"
)
list(APPEND _DSD_NEO_QML_FILES "${_DSD_NEO_QML_DIR}/qmldir")

if(_DSD_NEO_QML_FILES STREQUAL "")
    message(
        FATAL_ERROR
        "QML_RESOURCE_AUDIT: no QML files found under ${_DSD_NEO_QML_DIR}"
    )
endif()

set(_DSD_NEO_QML_MISSING "")
foreach(_qml_path IN LISTS _DSD_NEO_QML_FILES)
    get_filename_component(_qml_name "${_qml_path}" NAME)
    # Match the exact FILES entry, anchored so QmlThing.qml cannot satisfy
    # Thing.qml and a name inside a comment cannot satisfy anything.
    string(
        REGEX MATCH "[\r\n][ \t]*qml/${_qml_name}[ \t]*[\r\n]"
        _hit
        "${_DSD_NEO_QML_CMAKE_TEXT}"
    )
    if(_hit STREQUAL "")
        list(APPEND _DSD_NEO_QML_MISSING "${_qml_name}")
    endif()
endforeach()

if(NOT _DSD_NEO_QML_MISSING STREQUAL "")
    list(JOIN _DSD_NEO_QML_MISSING "\n  " _DSD_NEO_QML_MISSING_TEXT)
    message(
        FATAL_ERROR
        "QML_RESOURCE_AUDIT: these files exist under src/ui/qt/qml/ but are not in the\n"
        "qt_add_resources() FILES list in src/ui/qt/CMakeLists.txt:\n"
        "  ${_DSD_NEO_QML_MISSING_TEXT}\n"
        "\n"
        "They will resolve in the QML tests, which load from the source tree, and fail\n"
        "at runtime in the packaged app, which loads from qrc. Add `qml/<name>` to the\n"
        "FILES list."
    )
endif()

message(STATUS "QML_RESOURCE_AUDIT: ok")
