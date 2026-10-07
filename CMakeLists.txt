cmake_minimum_required(VERSION 3.22)

project(CappedWeather VERSION 0.7.0 LANGUAGES CXX)

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

# Same fail-fast philosophy for European ODIM volumes: production builds
# need HDF5-C, dev/CI opt out with -DCAPPEDWEATHER_REQUIRE_HDF5=OFF.
if(DEFINED ENV{CAPPEDWEATHER_REQUIRE_HDF5} AND NOT DEFINED CAPPEDWEATHER_REQUIRE_HDF5)
    set(_cw_require_hdf5_default "$ENV{CAPPEDWEATHER_REQUIRE_HDF5}")
else()
    set(_cw_require_hdf5_default "ON")
endif()
option(CAPPEDWEATHER_REQUIRE_HDF5
    "Fail configuration instead of building the stubbed ODIM reader when HDF5 is unavailable"
    ${_cw_require_hdf5_default})

include(CTest)

add_library(cappedweather_core STATIC
    src/core/circulation_detector.cpp
    src/core/shear_grid.cpp
    src/core/derived_products.cpp
    src/core/level2_primitives.cpp
    src/core/level2_decoder.cpp
    src/core/odim_reader.cpp
    src/core/product.cpp
    src/core/radar_geometry.cpp
    src/core/radar_catalog.cpp
    src/core/opera_catalog.cpp
    src/core/scan_diagnostics.cpp
    src/core/velocity_processing.cpp
    src/core/lightning_jump.cpp
    src/core/flash_rate_grid.cpp
    src/core/glm_l2_reader.cpp
    src/core/radar_lightning_fusion.cpp
    src/core/storm_tracker.cpp
    src/core/volume_spill.cpp
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

# HDF5-C for the OPERA ODIM_H5 polar volume reader (vcpkg: hdf5).
# Without it the reader stubs out; the S3 key helpers below are plain
# string code and always build.
find_package(HDF5 QUIET COMPONENTS C)
set(CAPPEDWEATHER_HDF5_TARGET "")
if(TARGET hdf5::hdf5-shared)
    set(CAPPEDWEATHER_HDF5_TARGET hdf5::hdf5-shared)
elseif(TARGET hdf5::hdf5-static)
    set(CAPPEDWEATHER_HDF5_TARGET hdf5::hdf5-static)
elseif(HDF5_FOUND)
    set(CAPPEDWEATHER_HDF5_TARGET ${HDF5_C_LIBRARIES})
endif()
if(CAPPEDWEATHER_HDF5_TARGET)
    target_link_libraries(cappedweather_core PUBLIC ${CAPPEDWEATHER_HDF5_TARGET})
    target_compile_definitions(cappedweather_core PRIVATE CAPPEDWEATHER_HAS_HDF5=1)
    message(STATUS "HDF5 found: ODIM reader enabled")
elseif(CAPPEDWEATHER_REQUIRE_HDF5)
    message(FATAL_ERROR
        "HDF5 is required for this build, but CMake could not find it. "
        "Install it (vcpkg: hdf5) or configure with -DCAPPEDWEATHER_REQUIRE_HDF5=OFF.")
else()
    message(STATUS "HDF5 not found: ODIM reader stubbed")
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

    # zlib for Meteoalarm vector-tile (gzip MVT) decoding; optional like the
    # other codecs — without it European warnings keep working as
    # alerts without map polygons.
    find_package(ZLIB QUIET)
    set(CAPPEDWEATHER_HAS_ZLIB ${ZLIB_FOUND})

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
            src/app/opera_source.cpp
            src/app/opera_source.hpp
            src/app/placefile.cpp
            src/app/placefile.hpp
            src/app/placefile_source.cpp
            src/app/placefile_source.hpp
            src/app/radar_map_canvas.cpp
            src/app/radar_map_canvas.hpp
            src/app/s3_listing.cpp
            src/app/s3_listing.hpp
            src/app/session_store.cpp
            src/app/session_store.hpp
            src/app/theme.cpp
            src/app/theme.hpp
            src/app/command_palette.cpp
            src/app/command_palette.hpp
            src/app/timeline_strip.cpp
            src/app/timeline_strip.hpp
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
        if(CAPPEDWEATHER_HAS_ZLIB)
            target_link_libraries(cappedweather_ui PRIVATE ZLIB::ZLIB)
            target_compile_definitions(cappedweather_ui PUBLIC CAPPEDWEATHER_HAS_ZLIB=1)
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

        qt_add_executable(cappedweather_live_opera_probe
            tests/live_opera_probe.cpp
        )
        target_link_libraries(cappedweather_live_opera_probe PRIVATE cappedweather_ui)

        qt_add_executable(rotation_replay
            tools/rotation_replay.cpp
        )
        target_link_libraries(rotation_replay PRIVATE cappedweather_ui)

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

            qt_add_executable(readme_screenshots
                tools/readme_screenshots.cpp
            )
            target_link_libraries(readme_screenshots PRIVATE
                cappedweather_ui
                Qt6::Test
            )
        endif()

        set_target_properties(CappedWeather PROPERTIES
            WIN32_EXECUTABLE TRUE
            MACOSX_BUNDLE TRUE
        )

        if(WIN32 AND (CAPPEDWEATHER_NETCDF_TARGET OR CAPPEDWEATHER_HDF5_TARGET))
            set(_cw_runtime_dirs "${CAPPEDWEATHER_QT_ROOT}/bin")
            if(CAPPEDWEATHER_NETCDF_TARGET)
                list(APPEND _cw_runtime_dirs "$<TARGET_FILE_DIR:${CAPPEDWEATHER_NETCDF_TARGET}>")
            endif()
            # TARGET_FILE_DIR needs a real target; the HDF5 fallback can be a
            # bare library path list, in which case there is nothing to harvest.
            if(CAPPEDWEATHER_HDF5_TARGET AND TARGET ${CAPPEDWEATHER_HDF5_TARGET})
                list(APPEND _cw_runtime_dirs "$<TARGET_FILE_DIR:${CAPPEDWEATHER_HDF5_TARGET}>")
            endif()
            install(TARGETS CappedWeather
                RUNTIME_DEPENDENCIES
                    DIRECTORIES ${_cw_runtime_dirs}
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

if(NOT WIN32)
    install(FILES packaging/CappedWeather.desktop DESTINATION share/applications)
    install(FILES packaging/CappedWeather.png DESTINATION share/icons/hicolor/256x256/apps)
endif()

# AGPL-3.0-or-later: ship the license text inside every package (CPack shows
# CPACK_RESOURCE_FILE_LICENSE in installer UI but does not pack the file).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" DESTINATION .)
endif()

set(CPACK_PACKAGE_NAME "CappedWeather")
set(CPACK_PACKAGE_VENDOR "Mikifrfr")
set(CPACK_PACKAGE_CONTACT "Mikifrfr@users.noreply.github.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Operational real-time Level II radar viewer")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "CappedWeather-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_LICENSE "AGPL-3.0-or-later")
if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
    set(CPACK_NSIS_PACKAGE_NAME "CappedWeather")
    set(CPACK_NSIS_DISPLAY_NAME "CappedWeather Level II Radar")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "CappedWeather")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MENU_LINKS "bin/CappedWeather.exe" "CappedWeather Level II Radar")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)

# --- Distribution track: build-tree Windows Qt deploy (appended) ---
# Lets build/Release/CappedWeather.exe run without manual PATH setup by
# copying the Qt runtime next to it via windeployqt.
# Existing install()/CPack rules above are untouched; opt in with:
#   cmake --build build --target cappedweather_deploy_build_tree --config Release
if(WIN32 AND Qt6_FOUND)
    if(TARGET CappedWeather)
        find_program(CAPPEDWEATHER_WINDEPLOYQT_EXE windeployqt.exe
            HINTS
                "${CAPPEDWEATHER_QT_ROOT}/bin"
                "$ENV{CMAKE_PREFIX_PATH}/bin"
                "C:/Qt/6.8.3/msvc2022_64/bin"
        )
        if(CAPPEDWEATHER_WINDEPLOYQT_EXE)
            add_custom_target(cappedweather_deploy_build_tree
                COMMAND "${CAPPEDWEATHER_WINDEPLOYQT_EXE}" --release --no-translations "$<TARGET_FILE:CappedWeather>"
                DEPENDS CappedWeather
                COMMENT "Running windeployqt on the build-tree CappedWeather.exe"
                VERBATIM
            )
        else()
            message(STATUS "windeployqt.exe not found: cappedweather_deploy_build_tree target skipped")
        endif()
    endif()
endif()
