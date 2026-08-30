# Copyright (C) 2026 Paul McKinney
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.16)

project(ScheduleIO VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(SCHEDULEIO_BUILD_TESTS "Build the ScheduleIO test suite" ON)
option(SCHEDULEIO_ORACLE_MSPROJECT "Enable the MS Project COM writer-acceptance oracle (Windows + Project required)" OFF)

# Match the ProjectNotes Qt discovery pattern (Qt6 preferred, Qt5 fallback).
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

set(SCHEDULEIO_SOURCES
    mppio.cpp
    xmlio.cpp
    src/xml/xmlserializer.cpp
    src/ole/compoundfile.cpp
    src/codec/streamquartet.cpp
    src/codec/fielddecoders.cpp
    src/codec/fieldmap.cpp
    src/codec/bkndvardata.cpp
    src/codec/propsreader.cpp
    src/codec/mppfieldids.cpp
    src/codec/viewformat.cpp
    src/codec/mpp14timephased.cpp
    src/model/project.cpp
    src/model/task.cpp
    src/model/resource.cpp
    src/model/assignment.cpp
    src/model/timephasedvalue.cpp
    src/model/calendar.cpp
    src/model/relation.cpp
    src/model/baseline.cpp
    src/model/customfield.cpp
    src/model/customfieldlogic.cpp
    src/model/costrate.cpp
    src/model/availability.cpp
    src/model/duration.cpp
      src/model/scheduler.cpp
      src/model/workcalendar.cpp
      src/model/schedulingcalendar.cpp
      src/model/taskscheduling.cpp
    src/model/materialcosting.cpp
    src/model/projectreconciliation.cpp
    src/model/progressupdating.cpp
    src/model/workcontouring.cpp
    src/model/resourceleveling.cpp
    src/serializer/docserializer.cpp
    src/serializer/mpp12serializer.cpp
    src/serializer/mpp14serializer.cpp
    src/serializer/mpp14manifest.cpp
    src/serializer/mpp14writer.cpp
    scheduleio.qrc
)

set(SCHEDULEIO_HEADERS
    mppio.h
    xmlio.h
    scheduleio_export.h
    src/xml/xmlserializer.h
    src/ole/compoundfile.h
    src/codec/streamquartet.h
    src/codec/fielddecoders.h
    src/codec/fieldmap.h
    src/codec/bkndvardata.h
    src/codec/propsreader.h
    src/codec/propskeys.h
    src/codec/mppfieldids.h
    src/codec/viewformat.h
    src/model/project.h
    src/model/task.h
    src/model/resource.h
    src/model/assignment.h
    src/model/calendar.h
    src/model/relation.h
    src/model/baseline.h
    src/model/customfield.h
    src/model/customfieldlogic.h
    src/model/viewstyles.h
    src/model/usageviewsettings.h
    src/model/timelineviewsettings.h
    src/model/costrate.h
    src/model/availability.h
    src/model/duration.h
      src/model/scheduler.h
      src/model/workcalendar.h
      src/model/schedulingcalendar.h
      src/model/taskscheduling.h
    src/model/materialcosting.h
    src/model/projectreconciliation.h
    src/model/progressupdating.h
    src/model/workcontouring.h
    src/model/resourceleveling.h
    src/serializer/docserializer.h
    src/serializer/mpp12serializer.h
    src/serializer/mpp14serializer.h
    src/serializer/mpp14manifest.h
    src/serializer/mpp14writer.h
)

# Compile every source exactly once into an object library. The shared library
# is built from these objects; the unit tests link the same objects directly so
# they can reach internal classes (CompoundFile, StreamQuartet, ...) that are not
# part of the exported public ABI.
add_library(scheduleio_objects OBJECT ${SCHEDULEIO_SOURCES} ${SCHEDULEIO_HEADERS})

set_target_properties(scheduleio_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The export attributes apply only while compiling the library's own code, so
# keep them PRIVATE. Tests that link these objects statically see SCHEDULEIO_EXPORT
# as empty and resolve symbols directly.
target_compile_definitions(scheduleio_objects PRIVATE SCHEDULEIO_DLL SCHEDULEIO_LIBRARY)

target_include_directories(scheduleio_objects
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(scheduleio_objects PUBLIC Qt${QT_VERSION_MAJOR}::Core)

add_library(ScheduleIO SHARED $<TARGET_OBJECTS:scheduleio_objects>)
target_link_libraries(ScheduleIO PUBLIC scheduleio_objects)

# Genuine downstream consumers that link the shared library import its symbols.
target_compile_definitions(ScheduleIO INTERFACE SCHEDULEIO_DLL)

set_target_properties(ScheduleIO PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(SCHEDULEIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
