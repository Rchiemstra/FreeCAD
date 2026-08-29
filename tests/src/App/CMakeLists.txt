# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(App_tests_run
        AsyncRecompute.cpp
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
        DocumentRevisionIndex.cpp
        DocumentObject.cpp
        DocumentObserver.cpp
        Expression.cpp
        ExpressionParser.cpp
        GeometryJobManager.cpp
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
        StringHasher.cpp
        VarSet.cpp
        VRMLObject.cpp
)

target_compile_definitions(App_tests_run PRIVATE DATADIR="${CMAKE_SOURCE_DIR}/data")
target_compile_definitions(App_tests_run PRIVATE FREECAD_DOCUMENTFILEWRITER_TEST_API)

target_link_libraries(App_tests_run PRIVATE
    GTest::gmock_main
    ${Python3_LIBRARIES}
    FreeCADApp
)

if(WIN32)
    target_link_libraries(App_tests_run PRIVATE advapi32)
endif()
