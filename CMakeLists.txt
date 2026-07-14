cmake_minimum_required(VERSION 3.22)

project(CappedWeather VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/FS)
endif()

option(CAPPEDWEATHER_BUILD_APP "Build the Qt6 desktop application when Qt6 is available" ON)

include(CTest)

add_library(cappedweather_core STATIC
    src/core/level2_primitives.cpp
    src/core/level2_decoder.cpp
    src/core/product.cpp
    src/core/radar_geometry.cpp
    src/core/radar_catalog.cpp
    src/core/scan_diagnostics.cpp
    src/core/velocity_processing.cpp
    src/core/s3_object.cpp
)

target_include_directories(cappedweather_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(cappedweather_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(cappedweather_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(cappedweather_core_tests
        tests/core_tests.cpp
    )
    target_link_libraries(cappedweather_core_tests PRIVATE cappedweather_core)
    add_test(NAME cappedweather_core_tests COMMAND cappedweather_core_tests)
endif()

if(CAPPEDWEATHER_BUILD_APP)
    find_package(Qt6 6.4 COMPONENTS Core Gui Widgets Network Xml QUIET)
    set(CAPPEDWEATHER_QT_FOUND ${Qt6_FOUND})

    find_package(BZip2 QUIET)

    if(CAPPEDWEATHER_QT_FOUND AND BZip2_FOUND)
        qt_standard_project_setup()
        get_filename_component(CAPPEDWEATHER_QT_ROOT "${Qt6_DIR}/../../.." ABSOLUTE)

        set(CAPPEDWEATHER_UI_SOURCES
            src/app/cross_section_widget.cpp
            src/app/cross_section_widget.hpp
            src/app/gif_writer.cpp
            src/app/gif_writer.hpp
            src/app/main_window.cpp
            src/app/main_window.hpp
            src/app/radar_map_canvas.cpp
            src/app/radar_map_canvas.hpp
            src/app/radar_list_model.cpp
            src/app/radar_list_model.hpp
            src/app/nexrad_source.cpp
            src/app/nexrad_source.hpp
            src/app/weather_overlay_source.cpp
            src/app/weather_overlay_source.hpp
        )

        qt_add_executable(CappedWeather
            src/app/main.cpp
            ${CAPPEDWEATHER_UI_SOURCES}
        )

        target_link_libraries(CappedWeather PRIVATE
            cappedweather_core
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::Network
            Qt6::Xml
            BZip2::BZip2
        )

        target_compile_definitions(CappedWeather PRIVATE
            CAPPEDWEATHER_HAS_BZIP2=1
            CAPPEDWEATHER_VERSION="${PROJECT_VERSION}"
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )

        if(WIN32)
            add_custom_command(TARGET CappedWeather POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:CappedWeather>/tls"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CAPPEDWEATHER_QT_ROOT}/plugins/tls/qschannelbackend.dll"
                    "$<TARGET_FILE_DIR:CappedWeather>/tls/qschannelbackend.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CAPPEDWEATHER_QT_ROOT}/plugins/tls/qcertonlybackend.dll"
                    "$<TARGET_FILE_DIR:CappedWeather>/tls/qcertonlybackend.dll"
                VERBATIM
            )
        endif()

        find_package(Qt6 6.4 COMPONENTS Test QUIET)
        if(TARGET Qt6::Test)
            qt_add_executable(cappedweather_ui_tests
                tests/ui_smoke_test.cpp
                ${CAPPEDWEATHER_UI_SOURCES}
            )
            target_link_libraries(cappedweather_ui_tests PRIVATE
                cappedweather_core
                Qt6::Core
                Qt6::Gui
                Qt6::Widgets
                Qt6::Network
                Qt6::Xml
                Qt6::Test
                BZip2::BZip2
            )
            target_compile_definitions(cappedweather_ui_tests PRIVATE
                CAPPEDWEATHER_HAS_BZIP2=1
                CAPPEDWEATHER_VERSION="${PROJECT_VERSION}"
                NOMINMAX
                WIN32_LEAN_AND_MEAN
            )
            add_test(NAME cappedweather_ui_tests COMMAND cappedweather_ui_tests -platform offscreen)
            set_tests_properties(cappedweather_ui_tests PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${CAPPEDWEATHER_QT_ROOT}/plugins;PATH=${CAPPEDWEATHER_QT_ROOT}/bin"
            )
        endif()

        set_target_properties(CappedWeather PROPERTIES
            WIN32_EXECUTABLE TRUE
            MACOSX_BUNDLE TRUE
        )

        install(TARGETS CappedWeather
            BUNDLE DESTINATION .
            RUNTIME DESTINATION bin
        )
        qt_generate_deploy_app_script(
            TARGET CappedWeather
            OUTPUT_SCRIPT CAPPEDWEATHER_DEPLOY_SCRIPT
            NO_UNSUPPORTED_PLATFORM_ERROR
        )
        install(SCRIPT ${CAPPEDWEATHER_DEPLOY_SCRIPT})
    else()
        message(WARNING "Qt6 and/or BZip2 was not found. The CappedWeather GUI target is skipped; core tests remain available.")
    endif()
endif()

set(CPACK_PACKAGE_NAME "CappedWeather")
set(CPACK_PACKAGE_VENDOR "CappedWeather")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Operational real-time Level II radar viewer")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "CappedWeather-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
