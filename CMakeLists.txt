cmake_minimum_required(VERSION 3.15)
project(KuronoOS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES
    kernel.cpp
    linux_bridge.c
    windows_bridge.c
    kcl_interpreter.c
    conflict_resolver.c
    security_supr_engine.c
    package_manager.c
)

add_executable(kurono_os_cpp
    ${SOURCES}
    kurono_os.c
)
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
set_source_files_properties(kurono_os.c PROPERTIES LANGUAGE CXX)

add_executable(test_suite_cpp
    ${SOURCES}
    test_suite.c
)
set_source_files_properties(test_suite.c PROPERTIES LANGUAGE CXX)