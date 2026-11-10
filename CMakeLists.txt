# ── GRAPHER ───────────────────────────────────────────────────────────────────
# C++23 data-driven graph canvas. The core (grapher::grapher) is UI-agnostic and dependency-free: a Schema says what
# node and edge types exist, a Graph holds the document, a Canvas turns pointer events into edits and the graph into
# a DrawList. Backends only paint draw lists and forward input:
#   grapher::qt  — QWidget + QPainter (GRAPHER_QT=ON; the consumer provides Qt6)
#
# Consumed via FetchContent; consumers link `grapher::grapher` (+ a backend) and `#include <grapher/Grapher.h>`.
# Layout:
#   API/grapher/      — public headers of the core
#   src/              — the core's implementation
#   qt/API, qt/src    — the Qt backend
#   tests/            — CTest suite (core only)
#
# The MSVC runtime is deliberately NOT set: it inherits the consumer's.

cmake_minimum_required(VERSION 3.28)

project(GRAPHER
    VERSION 0.1.0
    DESCRIPTION "C++23 data-driven, UI-agnostic graph canvas"
    LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(GRAPHER_BUILD_TESTS "Build GRAPHER tests" ${PROJECT_IS_TOP_LEVEL})
option(GRAPHER_QT "Build the Qt backend (grapher::qt)" OFF)

# ── grapher (core) ────────────────────────────────────────────────────────────
file(GLOB_RECURSE GRAPHER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE GRAPHER_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/API/grapher/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

add_library(grapher STATIC ${GRAPHER_SOURCES} ${GRAPHER_HEADERS})
add_library(grapher::grapher ALIAS grapher)
target_include_directories(grapher
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/API>
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(grapher PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(grapher PRIVATE /W4 /utf-8)
endif()
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${GRAPHER_SOURCES} ${GRAPHER_HEADERS})

# ── grapher-qt (backend) ──────────────────────────────────────────────────────
if(GRAPHER_QT)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    set(GRAPHER_QT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/qt/API/grapher/qt/CanvasWidget.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/qt/src/CanvasWidget.cpp")
    add_library(grapher-qt STATIC ${GRAPHER_QT_FILES})
    add_library(grapher::qt ALIAS grapher-qt)
    set_target_properties(grapher-qt PROPERTIES AUTOMOC ON)
    target_include_directories(grapher-qt PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/qt/API>)
    target_link_libraries(grapher-qt PUBLIC grapher::grapher Qt6::Widgets)
    if(MSVC)
        target_compile_options(grapher-qt PRIVATE /W4 /utf-8)
    endif()
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(GRAPHER_BUILD_TESTS)
    enable_testing()
    add_executable(grapher-test tests/grapher_test.cpp)
    target_link_libraries(grapher-test PRIVATE grapher::grapher)
    if(MSVC)
        target_compile_options(grapher-test PRIVATE /utf-8)
    endif()
    add_test(NAME grapher-test COMMAND grapher-test)
endif()
