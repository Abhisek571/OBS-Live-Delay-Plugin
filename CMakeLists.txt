cmake_minimum_required(VERSION 3.28)

project(obs-active-live-delay VERSION 0.1.52 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACTIVE_DELAY_BUILD_PLUGIN "Build the OBS/Qt plugin" ON)
option(ACTIVE_DELAY_BUILD_TESTS "Build dependency-free core tests" ON)
option(ACTIVE_DELAY_ENABLE_SCENE_PROBE "Build the P4 isolated-scene capture feasibility probe" OFF)

if (ACTIVE_DELAY_BUILD_PLUGIN OR ACTIVE_DELAY_BUILD_TESTS)
    find_package(Threads REQUIRED)

    add_library(active-delay-core STATIC
        src/delay-controller.cpp
        src/delayed-output-watchdog.cpp
        src/flv-muxer.cpp
        src/released-packet-dispatcher.cpp
        src/network-packet-consumer.cpp
		src/multistream-config.cpp
		src/multi-target-sender.cpp
        src/bounded-sender-queue.cpp
        src/rtmp-connection.cpp
        src/rtmp-sender.cpp
    )
    target_include_directories(active-delay-core PUBLIC src)
    target_link_libraries(active-delay-core PUBLIC Threads::Threads)
    target_compile_options(active-delay-core PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /EHsc /permissive- /Zc:__cplusplus>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    )
    set_target_properties(active-delay-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if (ACTIVE_DELAY_BUILD_PLUGIN)
    find_package(libobs REQUIRED)
    find_package(obs-frontend-api REQUIRED)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)

    find_path(FFMPEG_INCLUDE_DIR NAMES libavformat/avio.h REQUIRED)
    find_library(FFMPEG_AVFORMAT_LIBRARY NAMES avformat REQUIRED)
    find_library(FFMPEG_AVUTIL_LIBRARY NAMES avutil REQUIRED)
    if (NOT TARGET FFmpeg::avformat)
        add_library(FFmpeg::avformat UNKNOWN IMPORTED)
        set_target_properties(FFmpeg::avformat PROPERTIES
            IMPORTED_LOCATION "${FFMPEG_AVFORMAT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
    endif()
    if (NOT TARGET FFmpeg::avutil)
        add_library(FFmpeg::avutil UNKNOWN IMPORTED)
        set_target_properties(FFmpeg::avutil PROPERTIES
            IMPORTED_LOCATION "${FFMPEG_AVUTIL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
    endif()

    add_library(obs-active-live-delay MODULE
        src/plugin-main.cpp
        src/active-delay-output.cpp
        src/active-delay-dock.cpp
        src/ffmpeg-rtmp-connection.cpp
        src/obs-packet-copy.cpp
        src/scene-capture-probe.cpp
    )

    target_include_directories(obs-active-live-delay PRIVATE src)
    target_link_libraries(obs-active-live-delay PRIVATE
        active-delay-core
        OBS::libobs
        OBS::obs-frontend-api
        FFmpeg::avformat
        FFmpeg::avutil
        Qt6::Widgets
    )
    target_compile_options(obs-active-live-delay PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /EHsc /permissive- /Zc:__cplusplus>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    )
    target_compile_definitions(obs-active-live-delay PRIVATE
        ACTIVE_DELAY_ENABLE_SCENE_PROBE=$<BOOL:${ACTIVE_DELAY_ENABLE_SCENE_PROBE}>)

    set_target_properties(obs-active-live-delay PROPERTIES PREFIX "")

    include(GNUInstallDirs)
    if (WIN32)
        install(TARGETS obs-active-live-delay
            LIBRARY DESTINATION "obs-plugins/64bit"
            RUNTIME DESTINATION "obs-plugins/64bit")
    else()
        install(TARGETS obs-active-live-delay
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    endif()
    install(FILES
        data/obs-plugins/obs-active-live-delay/locale/en-US.ini
        data/obs-plugins/obs-active-live-delay/locale/en-GB.ini
        DESTINATION "data/obs-plugins/obs-active-live-delay/locale")
    install(FILES LICENSE NOTICE DESTINATION ".")
endif()

if (ACTIVE_DELAY_BUILD_TESTS)
    enable_testing()
    add_executable(active-delay-core-tests
        tests/delay-controller-tests.cpp
    )
    target_link_libraries(active-delay-core-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-core-tests COMMAND active-delay-core-tests)

    add_executable(active-delay-flv-tests tests/flv-muxer-tests.cpp)
    target_link_libraries(active-delay-flv-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-flv-tests COMMAND active-delay-flv-tests)

    add_executable(active-delay-sender-tests tests/rtmp-sender-tests.cpp)
    target_link_libraries(active-delay-sender-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-sender-tests COMMAND active-delay-sender-tests)

    add_executable(active-delay-output-watchdog-tests tests/delayed-output-watchdog-tests.cpp)
    target_link_libraries(active-delay-output-watchdog-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-output-watchdog-tests COMMAND active-delay-output-watchdog-tests)

    add_executable(active-delay-diagnostic-tests tests/diagnostic-error-tests.cpp)
    target_link_libraries(active-delay-diagnostic-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-diagnostic-tests COMMAND active-delay-diagnostic-tests)

    add_executable(active-delay-pipeline-tests tests/released-packet-dispatcher-tests.cpp)
    target_link_libraries(active-delay-pipeline-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-pipeline-tests COMMAND active-delay-pipeline-tests)

	add_executable(active-delay-multistream-tests tests/multi-target-sender-tests.cpp)
	target_link_libraries(active-delay-multistream-tests PRIVATE active-delay-core)
	add_test(NAME active-delay-multistream-tests COMMAND active-delay-multistream-tests)

    if (ACTIVE_DELAY_BUILD_PLUGIN)
        add_executable(active-delay-obs-packet-tests
            tests/obs-packet-copy-tests.cpp
            src/obs-packet-copy.cpp)
        target_include_directories(active-delay-obs-packet-tests PRIVATE src)
        target_link_libraries(active-delay-obs-packet-tests PRIVATE active-delay-core OBS::libobs)
        add_test(NAME active-delay-obs-packet-tests COMMAND active-delay-obs-packet-tests)
        get_filename_component(FFMPEG_PREFIX_DIR "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
        get_filename_component(OBS_BUILD_DIR "${libobs_DIR}" DIRECTORY)
        set_property(TEST active-delay-obs-packet-tests PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>"
            "PATH=path_list_prepend:${OBS_BUILD_DIR}/rundir/$<CONFIG>/bin/64bit"
            "PATH=path_list_prepend:${FFMPEG_PREFIX_DIR}/bin")
    endif()
endif()
