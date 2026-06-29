# Copyright (C) 2026 Paul McKinney
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.16)

project(MppIO VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(MPPIO_BUILD_TESTS "Build the MppIO test suite" ON)
option(MPPIO_ORACLE_MSPROJECT "Enable the MS Project COM writer-acceptance oracle (Windows + Project required)" OFF)

# Match the ProjectNotes Qt discovery pattern (Qt6 preferred, Qt5 fallback).
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

set(MPPIO_SOURCES
    mppio.cpp
    xmlio.cpp
    src/xml/xmlserializer.cpp
    src/ole/compoundfile.cpp
    src/codec/streamquartet.cpp
    src/codec/fielddecoders.cpp
    src/codec/bkndvardata.cpp
    src/codec/propsreader.cpp
    src/codec/mppfieldids.cpp
    src/model/mppproject.cpp
    src/model/mpptask.cpp
    src/model/mppresource.cpp
    src/model/mppassignment.cpp
    src/model/mppcalendar.cpp
    src/model/mpprelation.cpp
    src/model/mppbaseline.cpp
    src/model/mppcustomfield.cpp
    src/model/mppcostrate.cpp
    src/serializer/docserializer.cpp
    src/serializer/mpp12serializer.cpp
    src/serializer/mpp14serializer.cpp
)

set(MPPIO_HEADERS
    mppio.h
    xmlio.h
    mppio_export.h
    src/xml/xmlserializer.h
    src/ole/compoundfile.h
    src/codec/streamquartet.h
    src/codec/fielddecoders.h
    src/codec/bkndvardata.h
    src/codec/propsreader.h
    src/codec/mppfieldids.h
    src/model/mppproject.h
    src/model/mpptask.h
    src/model/mppresource.h
    src/model/mppassignment.h
    src/model/mppcalendar.h
    src/model/mpprelation.h
    src/model/mppbaseline.h
    src/model/mppcustomfield.h
    src/model/mppcostrate.h
    src/serializer/docserializer.h
    src/serializer/mpp12serializer.h
    src/serializer/mpp14serializer.h
)

# Compile every source exactly once into an object library. The shared library
# is built from these objects; the unit tests link the same objects directly so
# they can reach internal classes (CompoundFile, StreamQuartet, ...) that are not
# part of the exported public ABI.
add_library(mppio_objects OBJECT ${MPPIO_SOURCES} ${MPPIO_HEADERS})

set_target_properties(mppio_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The export attributes apply only while compiling the library's own code, so
# keep them PRIVATE. Tests that link these objects statically see MPPIO_EXPORT
# as empty and resolve symbols directly.
target_compile_definitions(mppio_objects PRIVATE MPPIO_DLL MPPIO_LIBRARY)

target_include_directories(mppio_objects
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mppio_objects PUBLIC Qt${QT_VERSION_MAJOR}::Core)

add_library(MppIO SHARED $<TARGET_OBJECTS:mppio_objects>)
target_link_libraries(MppIO PUBLIC mppio_objects)

# Genuine downstream consumers that link the shared library import its symbols.
target_compile_definitions(MppIO INTERFACE MPPIO_DLL)

set_target_properties(MppIO PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(MPPIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
