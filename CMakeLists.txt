cmake_minimum_required(VERSION 3.28)

project(obs-active-live-delay VERSION 0.1.53 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACTIVE_DELAY_BUILD_PLUGIN "Build the OBS/Qt plugin" ON)
option(ACTIVE_DELAY_BUILD_TESTS "Build dependency-free core tests" ON)
option(ACTIVE_DELAY_ENABLE_SCENE_PROBE "Build the P4 isolated-scene capture feasibility probe" OFF)

if (ACTIVE_DELAY_BUILD_PLUGIN OR ACTIVE_DELAY_BUILD_TESTS)
    find_package(Threads REQUIRED)

    add_library(active-delay-core STATIC
        src/transition-coordinator.cpp
        src/pipeline-owner.cpp
        src/holding-feed.cpp
        src/holding-capture.cpp
        src/delay-controller.cpp
        src/delayed-output-watchdog.cpp
        src/flv-muxer.cpp
        src/released-packet-dispatcher.cpp
        src/network-packet-consumer.cpp
		src/multistream-config.cpp
		src/multistream-preflight.cpp
		src/multi-target-sender.cpp
        src/bounded-sender-queue.cpp
        src/rtmp-connection.cpp
        src/rtmp-sender.cpp
		src/scene-switch-policy.cpp
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
        src/holding-obs-audio.cpp
        src/holding-obs-capture.cpp
        src/holding-obs-packet.cpp
        src/active-delay-output.cpp
        src/active-delay-dock.cpp
		src/destination-card-widget.cpp
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
    add_executable(active-delay-codec-splice-tests tests/codec-splice-tests.cpp)
    target_link_libraries(active-delay-codec-splice-tests PRIVATE active-delay-core)
    find_program(SPLICE_TEST_FFMPEG ffmpeg)
    find_program(SPLICE_TEST_PYTHON python)
    if (SPLICE_TEST_FFMPEG AND SPLICE_TEST_PYTHON)
        add_test(NAME active-delay-codec-splice-decode COMMAND "${SPLICE_TEST_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/codec-splice-validation.py"
            "${SPLICE_TEST_FFMPEG}" "$<TARGET_FILE:active-delay-codec-splice-tests>"
            "${CMAKE_CURRENT_BINARY_DIR}/codec-splice-evidence" --require-boundary-silence)
        set_tests_properties(active-delay-codec-splice-decode PROPERTIES TIMEOUT 25)
        foreach(SPLICE_RATE 44100 48000)
            foreach(SPLICE_CHANNELS 1 2)
                foreach(SPLICE_BFRAMES 0 2)
                    if(SPLICE_RATE EQUAL 48000 AND SPLICE_CHANNELS EQUAL 2 AND SPLICE_BFRAMES EQUAL 2)
                        continue() # original strict acceptance above
                    endif()
                    set(SPLICE_CASE "${SPLICE_RATE}-${SPLICE_CHANNELS}-${SPLICE_BFRAMES}")
                    add_test(NAME active-delay-codec-splice-${SPLICE_CASE} COMMAND "${SPLICE_TEST_PYTHON}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/codec-splice-validation.py"
                        "${SPLICE_TEST_FFMPEG}" "$<TARGET_FILE:active-delay-codec-splice-tests>"
                        "${CMAKE_CURRENT_BINARY_DIR}/codec-splice-${SPLICE_CASE}-evidence" --require-boundary-silence
                        "--rate=${SPLICE_RATE}" "--channels=${SPLICE_CHANNELS}" "--bframes=${SPLICE_BFRAMES}")
                    set_tests_properties(active-delay-codec-splice-${SPLICE_CASE} PROPERTIES TIMEOUT 25)
                endforeach()
            endforeach()
        endforeach()
    else()
        message(WARNING "FFmpeg or Python not found: codec-splice and prebuffer media tests are SKIPPED. Strict splice acceptance requires both; set SPLICE_TEST_FFMPEG and SPLICE_TEST_PYTHON")
    endif()
    add_executable(active-delay-epoch-tests tests/epoch-pipeline-tests.cpp)
    target_link_libraries(active-delay-epoch-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-epoch-tests COMMAND active-delay-epoch-tests)
    add_executable(active-delay-owner-tests tests/pipeline-owner-tests.cpp)
    target_link_libraries(active-delay-owner-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-owner-tests COMMAND active-delay-owner-tests)
    add_executable(active-delay-transition-tests tests/transition-coordinator-tests.cpp)
    target_link_libraries(active-delay-transition-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-transition-tests COMMAND active-delay-transition-tests)
    add_executable(active-delay-holding-tests tests/holding-feed-tests.cpp)
    target_link_libraries(active-delay-holding-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-holding-tests COMMAND active-delay-holding-tests)
    add_executable(active-delay-holding-lifecycle-tests tests/holding-lifecycle-tests.cpp)
    target_link_libraries(active-delay-holding-lifecycle-tests PRIVATE active-delay-core)
    add_test(NAME active-delay-holding-lifecycle-tests COMMAND active-delay-holding-lifecycle-tests)
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

	add_executable(active-delay-scene-switch-tests tests/scene-switch-policy-tests.cpp)
	target_link_libraries(active-delay-scene-switch-tests PRIVATE active-delay-core)
	add_test(NAME active-delay-scene-switch-tests COMMAND active-delay-scene-switch-tests)

    if (ACTIVE_DELAY_BUILD_PLUGIN)
		add_executable(active-delay-dock-ui-tests
			tests/destination-card-widget-tests.cpp
			src/destination-card-widget.cpp)
		target_include_directories(active-delay-dock-ui-tests PRIVATE src)
		target_link_libraries(active-delay-dock-ui-tests PRIVATE active-delay-core Qt6::Widgets)
		add_test(NAME active-delay-dock-ui-tests COMMAND active-delay-dock-ui-tests)
		set_property(TEST active-delay-dock-ui-tests PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=minimal")
		set_property(TEST active-delay-dock-ui-tests PROPERTY ENVIRONMENT_MODIFICATION
			"PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>")
		set_property(TEST active-delay-dock-ui-tests PROPERTY TIMEOUT 10)

        add_executable(active-delay-obs-packet-tests
            tests/obs-packet-copy-tests.cpp
            src/obs-packet-copy.cpp)
        target_include_directories(active-delay-obs-packet-tests PRIVATE src)
        target_link_libraries(active-delay-obs-packet-tests PRIVATE active-delay-core OBS::libobs)
        add_test(NAME active-delay-obs-packet-tests COMMAND active-delay-obs-packet-tests)
        get_filename_component(FFMPEG_PREFIX_DIR "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
        get_filename_component(OBS_BUILD_DIR "${libobs_DIR}" DIRECTORY)
        add_executable(active-delay-transition-dock-tests tests/transition-dock-tests.cpp
            src/active-delay-dock.cpp src/destination-card-widget.cpp)
        target_include_directories(active-delay-transition-dock-tests PRIVATE src)
        target_link_libraries(active-delay-transition-dock-tests PRIVATE active-delay-core OBS::libobs OBS::obs-frontend-api Qt6::Widgets)
        add_test(NAME active-delay-transition-dock-tests COMMAND active-delay-transition-dock-tests
            "${CMAKE_CURRENT_SOURCE_DIR}/data/obs-plugins/obs-active-live-delay/locale/en-US.ini")
        set_property(TEST active-delay-transition-dock-tests PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=minimal")
        set_property(TEST active-delay-transition-dock-tests PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Widgets>"
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>"
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::obs-frontend-api>"
            "PATH=path_list_prepend:${OBS_BUILD_DIR}/rundir/$<CONFIG>/bin/64bit"
            "PATH=path_list_prepend:${FFMPEG_PREFIX_DIR}/bin")
        set_tests_properties(active-delay-transition-dock-tests PROPERTIES TIMEOUT 10)
        add_executable(active-delay-holding-obs-tests
            tests/holding-obs-tests.cpp
            src/holding-obs-audio.cpp
            src/holding-obs-capture.cpp
            src/holding-obs-packet.cpp
            src/obs-packet-copy.cpp)
        target_include_directories(active-delay-holding-obs-tests PRIVATE src)
        target_link_libraries(active-delay-holding-obs-tests PRIVATE active-delay-core OBS::libobs)
        add_test(NAME active-delay-holding-obs-tests COMMAND active-delay-holding-obs-tests)
        if (WIN32)
            find_file(HOLDING_TEST_AAC_MODULE obs-ffmpeg.dll
                PATHS "$ENV{ProgramFiles}/obs-studio/obs-plugins/64bit" NO_DEFAULT_PATH)
            if (HOLDING_TEST_AAC_MODULE)
                add_executable(active-delay-holding-aac-tests tests/holding-aac-tests.cpp src/holding-obs-audio.cpp)
                target_include_directories(active-delay-holding-aac-tests PRIVATE src)
                target_link_libraries(active-delay-holding-aac-tests PRIVATE active-delay-core OBS::libobs)
                add_test(NAME active-delay-holding-aac-tests COMMAND active-delay-holding-aac-tests
                    "${HOLDING_TEST_AAC_MODULE}" "$ENV{ProgramFiles}/obs-studio/data/obs-plugins/obs-ffmpeg"
                    "${CMAKE_CURRENT_BINARY_DIR}/holding-silence.aac")
                set_tests_properties(active-delay-holding-aac-tests PROPERTIES FIXTURES_SETUP holding_aac TIMEOUT 10)
                find_program(HOLDING_TEST_FFMPEG ffmpeg)
                if (HOLDING_TEST_FFMPEG)
                    add_test(NAME active-delay-holding-aac-decode COMMAND "${CMAKE_COMMAND}"
                        "-DDECODER=${HOLDING_TEST_FFMPEG}"
                        "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/holding-silence.aac"
                        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/holding-silence.f32le"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/holding-aac-decode.cmake")
                    set_tests_properties(active-delay-holding-aac-decode PROPERTIES FIXTURES_REQUIRED holding_aac TIMEOUT 15)
                endif()
                set_property(TEST active-delay-holding-aac-tests PROPERTY ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$ENV{ProgramFiles}/obs-studio/bin/64bit"
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>"
                    "PATH=path_list_prepend:${OBS_BUILD_DIR}/rundir/$<CONFIG>/bin/64bit"
                    "PATH=path_list_prepend:${FFMPEG_PREFIX_DIR}/bin")
            endif()
        endif()
        set_property(TEST active-delay-holding-obs-tests PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>"
            "PATH=path_list_prepend:${OBS_BUILD_DIR}/rundir/$<CONFIG>/bin/64bit"
            "PATH=path_list_prepend:${FFMPEG_PREFIX_DIR}/bin")
        if (WIN32)
            add_executable(active-delay-prebuffer-obs-tests tests/prebuffer-obs-tests.cpp
                src/active-delay-output.cpp src/holding-obs-capture.cpp src/holding-obs-audio.cpp
                src/holding-obs-packet.cpp src/obs-packet-copy.cpp src/ffmpeg-rtmp-connection.cpp)
            target_include_directories(active-delay-prebuffer-obs-tests PRIVATE src)
            target_link_libraries(active-delay-prebuffer-obs-tests PRIVATE active-delay-core OBS::libobs FFmpeg::avformat FFmpeg::avutil)
            set(PREBUFFER_MODES)
            if (SPLICE_TEST_FFMPEG AND SPLICE_TEST_PYTHON)
                set(PREBUFFER_MODES single multi guard-single guard-multi)
            endif()
            foreach(PREBUFFER_MODE ${PREBUFFER_MODES})
                add_test(NAME active-delay-prebuffer-${PREBUFFER_MODE}-obs-tests COMMAND "${SPLICE_TEST_PYTHON}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/prebuffer-obs-validation.py"
                    "${SPLICE_TEST_FFMPEG}" "$<TARGET_FILE:active-delay-prebuffer-obs-tests>"
                    "$ENV{ProgramFiles}/obs-studio" "${CMAKE_CURRENT_BINARY_DIR}/prebuffer-${PREBUFFER_MODE}-evidence" ${PREBUFFER_MODE})
                set_property(TEST active-delay-prebuffer-${PREBUFFER_MODE}-obs-tests PROPERTY TIMEOUT 30)
                set_property(TEST active-delay-prebuffer-${PREBUFFER_MODE}-obs-tests PROPERTY ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>"
                    "PATH=path_list_prepend:${OBS_BUILD_DIR}/rundir/$<CONFIG>/bin/64bit"
                    "PATH=path_list_prepend:${FFMPEG_PREFIX_DIR}/bin")
            endforeach()
            add_executable(active-delay-ffmpeg-tests
                tests/ffmpeg-rtmp-connection-tests.cpp
                src/ffmpeg-rtmp-connection.cpp)
            target_include_directories(active-delay-ffmpeg-tests PRIVATE src)
            target_link_libraries(active-delay-ffmpeg-tests PRIVATE active-delay-core FFmpeg::avformat FFmpeg::avutil ws2_32)
            add_test(NAME active-delay-ffmpeg-tests COMMAND active-delay-ffmpeg-tests)
            set_property(TEST active-delay-ffmpeg-tests PROPERTY TIMEOUT 15)
            set_property(TEST active-delay-ffmpeg-tests PROPERTY ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${FFMPEG_PREFIX_DIR}/bin")
            add_executable(active-delay-rtmps-probe tests/rtmps-transport-probe.cpp src/ffmpeg-rtmp-connection.cpp)
            target_include_directories(active-delay-rtmps-probe PRIVATE src)
            target_link_libraries(active-delay-rtmps-probe PRIVATE active-delay-core FFmpeg::avformat FFmpeg::avutil)
            find_program(TRANSPORT_TEST_OPENSSL openssl)
            if (SPLICE_TEST_PYTHON AND TRANSPORT_TEST_OPENSSL)
                add_test(NAME active-delay-rtmps-tests COMMAND "${SPLICE_TEST_PYTHON}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/rtmps-transport-validation.py"
                    "$<TARGET_FILE:active-delay-rtmps-probe>" "${TRANSPORT_TEST_OPENSSL}")
                set_tests_properties(active-delay-rtmps-tests PROPERTIES TIMEOUT 30)
                set_property(TEST active-delay-rtmps-tests PROPERTY ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${FFMPEG_PREFIX_DIR}/bin")
            endif()
        endif()
        set_property(TEST active-delay-obs-packet-tests PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:OBS::libobs>"
            "PATH=path_list_prepend:${OBS_BUILD_DIR}/rundir/$<CONFIG>/bin/64bit"
            "PATH=path_list_prepend:${FFMPEG_PREFIX_DIR}/bin")
    endif()
endif()
