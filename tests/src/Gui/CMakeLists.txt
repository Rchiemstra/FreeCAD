# SPDX-License-Identifier: LGPL-2.1-or-later

add_subdirectory(Dialogs)
add_subdirectory(propertyeditor)

# Standard C++ GTest tests
add_executable(Gui_tests_run
        Assistant.cpp
        Camera.cpp
        CollaborationCompatibilityAdapter.cpp
        CollaborationAuthorityRemoval.cpp
        CollaborationDomainIntegration.cpp
        CollaborationResponsiveness.cpp
        CollaborationViewIsolation.cpp
        PersonalViewContext.cpp
        SharedPresentationCoordinator.cpp
        DocumentRecovery.cpp
        ColorScaleOverlayTest.cpp
        FileDialog.cpp
        StyleParameters/StyleParametersApplicationTest.cpp
        StyleParameters/ParserTest.cpp
        StyleParameters/ParameterManagerTest.cpp
        StyleParameters/YamlParameterSourceTest.cpp
        InputHintTest.cpp
        MainWindow.cpp
        NotificationClipboardTest.cpp
        View3DInventorViewer.cpp
        WindowLayout.cpp
        SelectionTest.cpp
)

# Qt tests
setup_qt_test(ObjectSelection)
setup_qt_test(QuantitySpinBox)

target_link_libraries(Gui_tests_run PRIVATE
    GTest::gmock_main
    FreeCADApp
    FreeCADGui
)

target_compile_definitions(Gui_tests_run PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Kept in its own process because it deliberately destroys the one supported
# MainWindow instance and verifies the QObject child-teardown path.
add_executable(GuiShutdown_tests_run
    MainWindowShutdown.cpp
)

target_link_libraries(GuiShutdown_tests_run PRIVATE
    GTest::gmock_main
    FreeCADApp
    FreeCADGui
)
