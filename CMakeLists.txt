cmake_minimum_required(VERSION 3.22)

project(CappedWeather VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/FS)
endif()

option(CAPPEDWEATHER_BUILD_APP "Build the Qt6 desktop application when Qt6 is available" ON)

# Bundling the Qt runtime (windeployqt/macdeployqt via Qt's deploy script)
# makes portable packages. Turn off when the deploy tooling is unavailable
# (e.g. Linux CI without linuxdeploy) to still get a plain TGZ.
option(CAPPEDWEATHER_DEPLOY_QT "Bundle the Qt runtime into the install/package step" ON)

# Production builds require netCDF-C so the GLM reader cannot silently stub out.
# Dev / CI can override with -DCAPPEDWEATHER_REQUIRE_NETCDF=OFF or by setting
# the CAPPEDWEATHER_REQUIRE_NETCDF environment variable.
if(DEFINED ENV{CAPPEDWEATHER_REQUIRE_NETCDF} AND NOT DEFINED CAPPEDWEATHER_REQUIRE_NETCDF)
    set(_cw_require_netcdf_default "$ENV{CAPPEDWEATHER_REQUIRE_NETCDF}")
else()
    set(_cw_require_netcdf_default "ON")
endif()
option(CAPPEDWEATHER_REQUIRE_NETCDF
    "Fail configuration instead of building the stubbed GLM reader when netCDF-C is unavailable"
    ${_cw_require_netcdf_default})

include(CTest)

add_library(cappedweather_core STATIC
    src/core/derived_products.cpp
    src/core/level2_primitives.cpp
    src/core/level2_decoder.cpp
    src/core/product.cpp
    src/core/radar_geometry.cpp
    src/core/radar_catalog.cpp
    src/core/scan_diagnostics.cpp
    src/core/velocity_processing.cpp
    src/core/lightning_jump.cpp
    src/core/flash_rate_grid.cpp
    src/core/glm_l2_reader.cpp
    src/core/radar_lightning_fusion.cpp
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

# Optional netCDF-C for the GLM L2 LCFA reader (vcpkg: netcdf-c).
# Without it the reader stubs out; S3 key helpers and the LJA core still build.
find_package(netCDF QUIET)
set(CAPPEDWEATHER_NETCDF_TARGET "")
if(TARGET netCDF::netcdf)
    set(CAPPEDWEATHER_NETCDF_TARGET netCDF::netcdf)
elseif(TARGET netcdf)
    set(CAPPEDWEATHER_NETCDF_TARGET netcdf)
endif()
if(CAPPEDWEATHER_NETCDF_TARGET)
    target_link_libraries(cappedweather_core PUBLIC ${CAPPEDWEATHER_NETCDF_TARGET})
    target_compile_definitions(cappedweather_core PRIVATE CAPPEDWEATHER_HAS_NETCDF=1)
    message(STATUS "netCDF found: GLM L2 reader enabled")
elseif(CAPPEDWEATHER_REQUIRE_NETCDF)
    message(FATAL_ERROR
        "netCDF-C is required for this build, but CMake could not find it. "
        "Set netCDF_DIR to the directory containing netCDFConfig.cmake.")
else()
    message(STATUS "netCDF not found: GLM L2 reader stubbed (key helpers still available)")
endif()

if(BUILD_TESTING)
    add_executable(cappedweather_core_tests
        tests/core_tests.cpp
    )
    target_link_libraries(cappedweather_core_tests PRIVATE cappedweather_core)
    if(CAPPEDWEATHER_NETCDF_TARGET)
        target_compile_definitions(cappedweather_core_tests PRIVATE
            CAPPEDWEATHER_EXPECT_NETCDF=1)
    endif()
    add_test(NAME cappedweather_core_tests COMMAND cappedweather_core_tests)
endif()

add_executable(glm_replay tools/glm_replay.cpp)
target_link_libraries(glm_replay PRIVATE cappedweather_core)

if(CAPPEDWEATHER_BUILD_APP)
    find_package(Qt6 6.4 COMPONENTS Core Gui Widgets Network Xml QUIET)
    set(CAPPEDWEATHER_QT_FOUND ${Qt6_FOUND})

    find_package(Qt6 6.4 COMPONENTS Multimedia QUIET)
    set(CAPPEDWEATHER_QT_MULTIMEDIA ${Qt6_MULTIMEDIA_FOUND})

    find_package(BZip2 QUIET)

    if(CAPPEDWEATHER_QT_FOUND AND BZip2_FOUND)
        qt_standard_project_setup()
        get_filename_component(CAPPEDWEATHER_QT_ROOT "${Qt6_DIR}/../../.." ABSOLUTE)

        set(CAPPEDWEATHER_UI_SOURCES
            src/app/archive_level2.cpp
            src/app/archive_level2.hpp
            src/app/archive_source.cpp
            src/app/archive_source.hpp
            src/app/alert_center.cpp
            src/app/alert_center.hpp
            src/app/cross_section_widget.cpp
            src/app/cross_section_widget.hpp
            src/app/gif_writer.cpp
            src/app/gif_writer.hpp
            src/app/http_client.cpp
            src/app/http_client.hpp
            src/app/glm_source.cpp
            src/app/glm_source.hpp
            src/app/main_window.cpp
            src/app/main_window.hpp
            src/app/nexrad_source.cpp
            src/app/nexrad_source.hpp
            src/app/placefile.cpp
            src/app/placefile.hpp
            src/app/placefile_source.cpp
            src/app/placefile_source.hpp
            src/app/radar_map_canvas.cpp
            src/app/radar_map_canvas.hpp
            src/app/s3_listing.cpp
            src/app/s3_listing.hpp
            src/app/theme.cpp
            src/app/theme.hpp
            src/app/radar_list_model.cpp
            src/app/radar_list_model.hpp
            src/app/shortcut_help_dialog.cpp
            src/app/shortcut_help_dialog.hpp
            src/app/weather_overlay_source.cpp
            src/app/weather_overlay_source.hpp
        )

        add_library(cappedweather_ui STATIC ${CAPPEDWEATHER_UI_SOURCES})

        target_link_libraries(cappedweather_ui PUBLIC
            cappedweather_core
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::Network
            Qt6::Xml
            BZip2::BZip2
        )
        if(CAPPEDWEATHER_QT_MULTIMEDIA)
            target_link_libraries(cappedweather_ui PUBLIC Qt6::Multimedia)
            target_compile_definitions(cappedweather_ui PUBLIC CAPPEDWEATHER_HAS_MULTIMEDIA=1)
        endif()

        target_compile_definitions(cappedweather_ui PUBLIC
            CAPPEDWEATHER_HAS_BZIP2=1
            CAPPEDWEATHER_VERSION="${PROJECT_VERSION}"
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )

        qt_add_executable(CappedWeather
            src/app/main.cpp
        )

        target_link_libraries(CappedWeather PRIVATE cappedweather_ui)

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

        qt_add_executable(cappedweather_live_glm_probe
            tests/live_glm_probe.cpp
        )
        target_link_libraries(cappedweather_live_glm_probe PRIVATE cappedweather_ui)

        find_package(Qt6 6.4 COMPONENTS Test QUIET)
        if(TARGET Qt6::Test)
            qt_add_executable(cappedweather_ui_tests
                tests/ui_smoke_test.cpp
            )
            target_link_libraries(cappedweather_ui_tests PRIVATE
                cappedweather_ui
                Qt6::Test
            )
            add_test(NAME cappedweather_ui_tests COMMAND cappedweather_ui_tests -platform offscreen)
            set_tests_properties(cappedweather_ui_tests PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${CAPPEDWEATHER_QT_ROOT}/plugins"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CAPPEDWEATHER_QT_ROOT}/bin"
            )
            if(CAPPEDWEATHER_NETCDF_TARGET)
                set_property(TEST cappedweather_ui_tests APPEND PROPERTY ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:${CAPPEDWEATHER_NETCDF_TARGET}>"
                )
            endif()

            qt_add_executable(cappedweather_live_gui_probe
                tests/live_gui_probe.cpp
            )
            target_link_libraries(cappedweather_live_gui_probe PRIVATE cappedweather_ui)
        endif()

        set_target_properties(CappedWeather PROPERTIES
            WIN32_EXECUTABLE TRUE
            MACOSX_BUNDLE TRUE
        )

        if(WIN32 AND CAPPEDWEATHER_NETCDF_TARGET)
            install(TARGETS CappedWeather
                RUNTIME_DEPENDENCIES
                    DIRECTORIES "$<TARGET_FILE_DIR:${CAPPEDWEATHER_NETCDF_TARGET}>"
                                "${CAPPEDWEATHER_QT_ROOT}/bin"
                    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
                    POST_EXCLUDE_REGEXES ".*[Ss]ystem32/.*"
                BUNDLE DESTINATION .
                RUNTIME DESTINATION bin
            )
        else()
            install(TARGETS CappedWeather
                BUNDLE DESTINATION .
                RUNTIME DESTINATION bin
            )
        endif()
        if(CAPPEDWEATHER_DEPLOY_QT)
            qt_generate_deploy_app_script(
                TARGET CappedWeather
                OUTPUT_SCRIPT CAPPEDWEATHER_DEPLOY_SCRIPT
                NO_UNSUPPORTED_PLATFORM_ERROR
            )
            install(SCRIPT ${CAPPEDWEATHER_DEPLOY_SCRIPT})
        endif()
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
