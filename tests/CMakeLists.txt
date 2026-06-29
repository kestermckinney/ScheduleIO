# Copyright (C) 2026 Paul McKinney
# SPDX-License-Identifier: GPL-3.0-only

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

# Helper: declare a QtTest executable and register it with ctest.
function(mppio_add_test name)
    qt_add_executable(${name} ${ARGN})
    # Link the object library (internal symbols) rather than the DLL's exported
    # ABI, so unit tests can exercise internal classes directly.
    target_link_libraries(${name} PRIVATE mppio_objects Qt${QT_VERSION_MAJOR}::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
    # The dynamic-load test needs the actual .dll/.dylib/.so to exist at run time.
    add_dependencies(${name} MppIO)
    # Make the directory holding the real fixtures and the built shared library
    # path available to tests.
    target_compile_definitions(${name} PRIVATE
        MPPIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        MPPIO_LIB_PATH="$<TARGET_FILE:MppIO>")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Layer 1 - unit tests (no real fixtures needed)
mppio_add_test(tst_compoundfile    unit/tst_compoundfile.cpp)
mppio_add_test(tst_streamquartet   unit/tst_streamquartet.cpp)
mppio_add_test(tst_fielddecoders   unit/tst_fielddecoders.cpp)
mppio_add_test(tst_versiondispatch unit/tst_versiondispatch.cpp)
mppio_add_test(tst_propsreader     unit/tst_propsreader.cpp)

# Layer 2 - round-trip (synthetic now; fixture-driven cases self-skip if absent)
mppio_add_test(tst_semantic_roundtrip roundtrip/tst_semantic_roundtrip.cpp)

# Layer 3 - oracle: decoded data must agree with the MS Project XML export
mppio_add_test(tst_task_names_oracle oracle/tst_task_names_oracle.cpp)
mppio_add_test(tst_task_dates_oracle oracle/tst_task_dates_oracle.cpp)
mppio_add_test(tst_task_fields_oracle oracle/tst_task_fields_oracle.cpp)
mppio_add_test(tst_entities_oracle oracle/tst_entities_oracle.cpp)
mppio_add_test(tst_cost_oracle oracle/tst_cost_oracle.cpp)
mppio_add_test(tst_baseline_oracle oracle/tst_baseline_oracle.cpp)
mppio_add_test(tst_custom_oracle oracle/tst_custom_oracle.cpp)
mppio_add_test(tst_notes_oracle oracle/tst_notes_oracle.cpp)
mppio_add_test(tst_costrate_oracle oracle/tst_costrate_oracle.cpp)
mppio_add_test(tst_calendar_oracle oracle/tst_calendar_oracle.cpp)

# XmlIO - MS Project compatible XML (MSPDI) read/write over the shared model
mppio_add_test(tst_xml_roundtrip      xml/tst_xml_roundtrip.cpp)
mppio_add_test(tst_xml_mpp_crosscheck xml/tst_xml_mpp_crosscheck.cpp)

# Layer 5 - dynamic loading of the built shared library
mppio_add_test(tst_dynamicload     loader/tst_dynamicload.cpp)

# Diagnostic - parse real .mpp fixtures with the CFB reader and dump their trees
mppio_add_test(tst_fixture_cfb     probe/tst_fixture_cfb.cpp)

# Throwaway byte-dumper (not a ctest test); run manually during RE work.
qt_add_executable(dump_task probe/dump_task.cpp)
target_link_libraries(dump_task PRIVATE mppio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_task PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_props probe/dump_props.cpp)
target_link_libraries(dump_props PRIVATE mppio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_props PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_dates probe/dump_dates.cpp)
target_link_libraries(dump_dates PRIVATE mppio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_dates PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(dump_fields probe/dump_fields.cpp)
target_link_libraries(dump_fields PRIVATE mppio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(dump_fields PRIVATE ${CMAKE_SOURCE_DIR}/src)

qt_add_executable(xml_diff probe/xml_diff.cpp)
target_link_libraries(xml_diff PRIVATE mppio_objects Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(xml_diff PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
