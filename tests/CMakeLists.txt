# Copyright (C) 2026 Paul McKinney
# SPDX-License-Identifier: GPL-3.0-only

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

# Helper: declare a QtTest executable and register it with ctest.
function(scheduleio_add_test name)
    qt_add_executable(${name} ${ARGN})
    # Link the object library (internal symbols) rather than the DLL's exported
    # ABI, so unit tests can exercise internal classes directly.
    target_link_libraries(${name} PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR})   # fixtureutils.h shared fixture discovery
    # The dynamic-load test needs the actual .dll/.dylib/.so to exist at run time.
    add_dependencies(${name} ScheduleIO)
    # Make the directory holding the real fixtures and the built shared library
    # path available to tests.
    target_compile_definitions(${name} PRIVATE
        SCHEDULEIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        SCHEDULEIO_LIB_PATH="$<TARGET_FILE:ScheduleIO>")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Layer 1 - unit tests (no real fixtures needed)
scheduleio_add_test(tst_compoundfile    unit/tst_compoundfile.cpp)
scheduleio_add_test(tst_streamquartet   unit/tst_streamquartet.cpp)
scheduleio_add_test(tst_fielddecoders   unit/tst_fielddecoders.cpp)
scheduleio_add_test(tst_versiondispatch unit/tst_versiondispatch.cpp)
scheduleio_add_test(tst_propsreader     unit/tst_propsreader.cpp)
scheduleio_add_test(tst_duration        unit/tst_duration.cpp)
scheduleio_add_test(tst_scheduler       unit/tst_scheduler.cpp)
scheduleio_add_test(tst_workcalendar    unit/tst_workcalendar.cpp)
scheduleio_add_test(tst_taskscheduling  unit/tst_taskscheduling.cpp)
scheduleio_add_test(tst_resourcecalendars unit/tst_resourcecalendars.cpp)
scheduleio_add_test(tst_resourceleveling unit/tst_resourceleveling.cpp)
scheduleio_add_test(tst_mpp14timephased unit/tst_mpp14timephased.cpp)
scheduleio_add_test(tst_materialcosting unit/tst_materialcosting.cpp)
scheduleio_add_test(tst_projectreconciliation unit/tst_projectreconciliation.cpp)
scheduleio_add_test(tst_progressupdating unit/tst_progressupdating.cpp)
scheduleio_add_test(tst_workcontouring unit/tst_workcontouring.cpp)
scheduleio_add_test(tst_paritygaps unit/tst_paritygaps.cpp)
scheduleio_add_test(tst_usageviewsettings unit/tst_usageviewsettings.cpp)

if(WIN32 AND SCHEDULEIO_ORACLE_MSPROJECT)
    scheduleio_add_test(tst_msproject_usage_view interop/tst_msproject_usage_view.cpp)
    target_compile_definitions(tst_msproject_usage_view PRIVATE
        SCHEDULEIO_USAGE_ORACLE_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/interop/msproject_usage_view_oracle.ps1")
endif()

# Layer 2 - round-trip (synthetic now; fixture-driven cases self-skip if absent)
scheduleio_add_test(tst_semantic_roundtrip roundtrip/tst_semantic_roundtrip.cpp)

# Layer 3 - oracle: decoded data must agree with the MS Project XML export
scheduleio_add_test(tst_task_names_oracle oracle/tst_task_names_oracle.cpp)
scheduleio_add_test(tst_task_dates_oracle oracle/tst_task_dates_oracle.cpp)
scheduleio_add_test(tst_task_fields_oracle oracle/tst_task_fields_oracle.cpp)
scheduleio_add_test(tst_entities_oracle oracle/tst_entities_oracle.cpp)
scheduleio_add_test(tst_cost_oracle oracle/tst_cost_oracle.cpp)
scheduleio_add_test(tst_baseline_oracle oracle/tst_baseline_oracle.cpp)
scheduleio_add_test(tst_custom_oracle oracle/tst_custom_oracle.cpp)
scheduleio_add_test(tst_notes_oracle oracle/tst_notes_oracle.cpp)
scheduleio_add_test(tst_costrate_oracle oracle/tst_costrate_oracle.cpp)
scheduleio_add_test(tst_availability_oracle oracle/tst_availability_oracle.cpp)
scheduleio_add_test(tst_calendar_oracle oracle/tst_calendar_oracle.cpp)
scheduleio_add_test(tst_assignment_oracle oracle/tst_assignment_oracle.cpp)
scheduleio_add_test(tst_format_oracle oracle/tst_format_oracle.cpp)
scheduleio_add_test(tst_timeline_oracle oracle/tst_timeline_oracle.cpp)

# XmlIO - MS Project compatible XML (MSPDI) read/write over the shared model
scheduleio_add_test(tst_xml_roundtrip      xml/tst_xml_roundtrip.cpp)
scheduleio_add_test(tst_xml_mpp_crosscheck xml/tst_xml_mpp_crosscheck.cpp)

# Layer 5 - dynamic loading of the built shared library
scheduleio_add_test(tst_dynamicload     loader/tst_dynamicload.cpp)

# Diagnostic - parse real .mpp fixtures with the CFB reader and dump their trees
scheduleio_add_test(tst_fixture_cfb     probe/tst_fixture_cfb.cpp)

# Throwaway byte-dumper (not a ctest test); run manually during RE work.
qt_add_executable(dump_task probe/dump_task.cpp)
target_link_libraries(dump_task PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_task PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_props probe/dump_props.cpp)
target_link_libraries(dump_props PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_props PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(cfb_streams probe/cfb_streams.cpp)
target_link_libraries(cfb_streams PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(cfb_streams PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_dates probe/dump_dates.cpp)
target_link_libraries(dump_dates PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_dates PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_fields probe/dump_fields.cpp)
target_link_libraries(dump_fields PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_fields PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_assn probe/dump_assn.cpp)
target_link_libraries(dump_assn PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_assn PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_calraw probe/dump_calraw.cpp)
target_link_libraries(dump_calraw PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_calraw PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(rename_cal probe/rename_cal.cpp)
target_link_libraries(rename_cal PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(rename_cal PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_cal probe/dump_cal.cpp)
target_link_libraries(dump_cal PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_cal PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_rsc probe/dump_rsc.cpp)
target_link_libraries(dump_rsc PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_rsc PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_taskmeta probe/dump_taskmeta.cpp)
target_link_libraries(dump_taskmeta PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_taskmeta PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_fontbases probe/dump_fontbases.cpp)
target_link_libraries(dump_fontbases PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_fontbases PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_cview probe/dump_cview.cpp)
target_link_libraries(dump_cview PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_cview PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_barexc probe/dump_barexc.cpp)
target_link_libraries(dump_barexc PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_barexc PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_timeline probe/dump_timeline.cpp)
target_link_libraries(dump_timeline PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_timeline PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(resave probe/resave.cpp)
target_link_libraries(resave PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(resave PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(create_task_definition_oracle probe/create_task_definition_oracle.cpp)
target_link_libraries(create_task_definition_oracle PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(create_task_definition_oracle PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(create_parity_oracle probe/create_parity_oracle.cpp)
target_link_libraries(create_parity_oracle PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(create_parity_oracle PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(usage_view_oracle_artifact probe/usage_view_oracle_artifact.cpp)
target_link_libraries(usage_view_oracle_artifact PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(usage_view_oracle_artifact PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(usage_view_oracle_artifact PRIVATE
    SCHEDULEIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

qt_add_executable(dump_model probe/dump_model.cpp)
target_link_libraries(dump_model PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_model PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(xml_diff probe/xml_diff.cpp)
target_link_libraries(xml_diff PRIVATE scheduleio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(xml_diff PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
