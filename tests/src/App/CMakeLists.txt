# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(App_tests_run
        Application.cpp
        ApplicationDirectories.cpp
        BackupPolicy.cpp
        Branding.cpp
        ComplexGeoData.cpp
        CollaborationRegistry.cpp
        CollaborationCompatibilityMutation.cpp
        CollaborativeSetPropertyIndependence.cpp
        CollaborativeSetPropertyOperation.cpp
        DocumentCollaborationConcurrency.cpp
        DocumentCollaborationPython.cpp
        DocumentCollaborationPythonCompatibility.cpp
        DocumentCollaborationRecovery.cpp
        DocumentCollaborationService.cpp
        DocumentCommitCoordinator.cpp
        Document.cpp
        DocumentFileWriter.cpp
        DocumentCollaborationBoundary.cpp
        DocumentMutationAuthority.cpp
        DocumentPropertyRevision.cpp
        DocumentRecomputeCoordinator.cpp
        DocumentRevisionIndex.cpp
        DocumentObject.cpp
        DocumentObserver.cpp
        Expression.cpp
        ExpressionParser.cpp
        GeometryArchive.cpp
        GenericIsolatedRecompute.cpp
        GeometryJobManager.cpp
        GeometryProcessBackend.cpp
        ElementMap.cpp
        ElementNamingUtils.cpp
        IndexedName.cpp
        License.cpp
        Link.cpp
        MappedElement.cpp
        MappedName.cpp
        Metadata.cpp
        ProjectFile.cpp
        PreparedEdit.cpp
        PreparedEditExecutor.cpp
        PropertyFile.cpp
        Property.h
        Property.cpp
        PropertyExpressionEngine.cpp
        RecomputeHandle.cpp
        StringHasher.cpp
        VarSet.cpp
        VRMLObject.cpp
)

target_compile_definitions(App_tests_run PRIVATE DATADIR="${CMAKE_SOURCE_DIR}/data")
target_compile_definitions(App_tests_run PRIVATE FREECAD_DOCUMENTFILEWRITER_TEST_API)

add_executable(GeometryProcessTestWorker GeometryProcessTestWorker.cpp)
if(WIN32)
    set_target_properties(GeometryProcessTestWorker PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
else()
    set_target_properties(GeometryProcessTestWorker PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
endif()
add_dependencies(App_tests_run GeometryProcessTestWorker FreeCADMainCmd)
target_compile_definitions(App_tests_run PRIVATE
    GEOMETRY_PROCESS_TEST_WORKER_PATH="$<TARGET_FILE:GeometryProcessTestWorker>"
    GEOMETRY_INSTALLED_WORKER_PATH="$<TARGET_FILE:FreeCADMainCmd>")

# The production backend deliberately resolves only a trusted sibling
# FreeCADCmd.  Unix test executables live in tests/ while FreeCADCmd lives in
# bin/, so reproduce the installed-tree layout automatically for App tests
# instead of adding a test-only environment override to production code.
if(NOT WIN32)
    add_custom_command(TARGET App_tests_run POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:FreeCADMainCmd>"
            "$<TARGET_FILE_DIR:App_tests_run>/$<TARGET_FILE_NAME:FreeCADMainCmd>")
endif()

target_link_libraries(App_tests_run PRIVATE
    GTest::gmock_main
    ${Python3_LIBRARIES}
    FreeCADApp
)

if(WIN32)
    target_link_libraries(App_tests_run PRIVATE advapi32)
endif()
