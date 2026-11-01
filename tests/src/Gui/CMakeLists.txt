# SPDX-License-Identifier: LGPL-2.1-or-later

add_subdirectory(Dialogs)
add_subdirectory(propertyeditor)

# The drag controller has no document dependency. Keep its input/lifetime
# regressions runnable without building the complete native GUI test binary.
add_executable(ViewDropDocking_tests_run ViewDropDocking.cpp)
target_include_directories(ViewDropDocking_tests_run PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ViewDropDocking_tests_run PRIVATE GTest::gtest_main Qt6::Widgets Qt6::Test)

# Standard C++ GTest tests
add_executable(Gui_tests_run
        Assistant.cpp
        Camera.cpp
        BodyMaterial.cpp
        CollaborationCompatibilityAdapter.cpp
        CollaborationGuiTestHelpers.cpp
        CollaborationLinkReplayGuard.cpp
        CollaborationAuthorityRemoval.cpp
        CollaborationDomainIntegration.cpp
        CollaborationResponsiveness.cpp
        CollaborationViewIsolation.cpp
        PersonalViewContext.cpp
        PropertyEditorPaint.cpp
        PropertyEditorTransaction.cpp
        ExpressionDiagnosticsPresentation.cpp
        SharedPresentationCoordinator.cpp
        DocumentPresentationCache.cpp
        DocumentRecovery.cpp
        DockWidget.cpp
        DocumentExecutionWaitTestHelpers.cpp
        GuiPythonGate.cpp
        UpdateGuiThread.cpp
        ColorScaleOverlayTest.cpp
        FileDialog.cpp
        StyleParameters/DiagnosticsCapture.h
        StyleParameters/ValueMatchers.h
        StyleParameters/ValueMatchers.cpp
        StyleParameters/StyleParametersApplicationTest.cpp
        StyleParameters/ParserTest.cpp
        StyleParameters/ParserRobustnessTest.cpp
        StyleParameters/ParameterManagerTest.cpp
        StyleParameters/YamlParameterSourceTest.cpp
        StyleParameters/DiagnosticsTest.cpp
        StyleParameters/ValueAccessTest.cpp
        StyleParameters/EdgesTest.cpp
        StyleParameters/CornersTest.cpp
        StyleParameters/GradientTest.cpp
        StyleParameters/ColorShadingTest.cpp
        InputHintTest.cpp
        MainWindow.cpp
        TreeDocumentRename.cpp
        NotificationClipboardTest.cpp
        View3DInventorViewer.cpp
        SoDatumLabel.cpp
        WindowLayout.cpp
        ViewProviderDoubleClick.cpp
        NativeGesturePinchTest.cpp
        NavigationStylePinchTest.cpp
        NavigationStyleWheelTest.cpp
        SelectionTest.cpp
        SelectionPythonThread.cpp
        SelectionRecomputeDeletion.cpp
        SoMouseWheelEventTest.cpp
)

# Qt tests
setup_qt_test(ObjectSelection)
setup_qt_test(TranslatorLocale)
setup_qt_test(InputField)
setup_qt_test(QuantitySpinBox)
setup_qt_test(NumericLocaleConsumers)

target_include_directories(Gui_tests_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Gui_tests_run PRIVATE
    GTest::gmock_main
    ${QtTest_LIBRARIES}
    FreeCADApp
    FreeCADGui
)

if(TARGET PartGui)
    target_link_libraries(Gui_tests_run PRIVATE PartGui)
    target_compile_definitions(Gui_tests_run PRIVATE FREECAD_TEST_PART_CAPTURE_CACHE)
endif()

if(TARGET PartDesignGui)
    add_dependencies(Gui_tests_run PartDesignGui)
    target_compile_definitions(Gui_tests_run PRIVATE FREECAD_TEST_PARTDESIGN_DATUM_EXTENTS)
endif()

target_compile_definitions(Gui_tests_run PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Collaboration integration tests use the production isolated-recompute
# backend, which resolves only a trusted sibling FreeCADCmd executable.
add_dependencies(Gui_tests_run FreeCADMainCmd)
if(NOT WIN32)
    add_custom_command(TARGET Gui_tests_run POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:FreeCADMainCmd>"
            "$<TARGET_FILE_DIR:Gui_tests_run>/$<TARGET_FILE_NAME:FreeCADMainCmd>")
endif()

if(WIN32)
    target_compile_definitions(Gui_tests_run PRIVATE QUARTER_DLL)
endif()

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
