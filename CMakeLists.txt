cmake_minimum_required(VERSION 3.28)

project(obs-active-live-delay VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(obs-active-live-delay MODULE
    src/plugin-main.cpp
    src/delay-controller.cpp
    src/active-delay-output.cpp
    src/active-delay-dock.cpp
)

target_include_directories(obs-active-live-delay PRIVATE src)
target_link_libraries(obs-active-live-delay PRIVATE
    OBS::libobs
    OBS::frontend-api
    Qt6::Widgets
)

if (MSVC)
    target_compile_options(obs-active-live-delay PRIVATE /Zc:__cplusplus)
endif()

set_target_properties(obs-active-live-delay PROPERTIES PREFIX "")

include(GNUInstallDirs)
install(TARGETS obs-active-live-delay
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/obs-plugins/64bit")

option(ACTIVE_DELAY_BUILD_TESTS "Build core delay-controller tests" ON)
if (ACTIVE_DELAY_BUILD_TESTS)
    enable_testing()
    add_executable(active-delay-core-tests
        tests/delay-controller-tests.cpp
        src/delay-controller.cpp
    )
    target_include_directories(active-delay-core-tests PRIVATE src)
    add_test(NAME active-delay-core-tests COMMAND active-delay-core-tests)
endif()
