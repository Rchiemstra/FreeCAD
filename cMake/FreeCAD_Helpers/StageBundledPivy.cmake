# FreeCAD's multi-config build keeps Pivy's generated Python wrapper in one
# common directory, but gives each configuration its own importable package.
function(freecad_stage_bundled_pivy package_source wrapper_dir package_root)
    foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(pivy_coin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${package_root}/${config}/Mod/pivy"
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${package_root}/${config}/Mod/pivy")
    endforeach()

    set_property(TARGET pivy_coin APPEND PROPERTY BUILD_RPATH "${package_root}/$<CONFIG>/lib")

    # A custom target runs even when only a Python source changed. The target
    # depends on the extension, which also ensures SWIG has made coin.py.
    add_custom_target(freecad_pivy_stage ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${package_source}"
            "-DWRAPPER_DIR=${wrapper_dir}"
            "-DDEST_DIR=${package_root}/$<CONFIG>/Mod/pivy"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageBundledPivyFiles.cmake"
        DEPENDS pivy_coin
        VERBATIM)
    add_dependencies(pivy freecad_pivy_stage)
endfunction()

# The upstream command assumes a shared importable extension. Run its generator
# after staging, against the selected configuration, and install those products.
function(freecad_generate_bundled_pivy_stubs pivy_source package_root python stubgen install_dir)
    if(NOT IS_ABSOLUTE "${python}" OR NOT IS_ABSOLUTE "${stubgen}")
        message(FATAL_ERROR "Pivy stub generation requires qualified Python and stubgen paths")
    endif()
    set(package_parent "${package_root}/$<CONFIG>/Mod")
    add_custom_target(pivy_stubs ALL
        COMMAND "${CMAKE_COMMAND}" -E env
            "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Coin>:$ENV{LD_LIBRARY_PATH}"
            "${python}" "${pivy_source}/tools/generate_pivy_stubs.py"
            --stubgen "${stubgen}" --output "${package_parent}"
            --stamp "${package_parent}/pivy/.pyi-stamp" --module pivy.coin
        DEPENDS freecad_pivy_stage
        VERBATIM)
    # Named consumers of pivy must also regenerate after the always-run stage.
    # This depends on the stage, not pivy, so the graph remains acyclic.
    add_dependencies(pivy pivy_stubs)
    # Pivy installs its shared source copy first. Remove the two files its
    # generator may remove on failure, then overlay this configuration's stubs.
    install(CODE "file(REMOVE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${install_dir}/coin.pyi\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${install_dir}/py.typed\")")
    install(DIRECTORY "${package_parent}/pivy/" DESTINATION "${install_dir}"
        FILES_MATCHING PATTERN "*.pyi" PATTERN "py.typed")
endfunction()
