# SPDX-License-Identifier: LGPL-2.1-or-later

disable_occt8_deprecation_warnings()

add_subdirectory(App)
if(BUILD_GUI)
    add_subdirectory(Gui)
endif(BUILD_GUI)

#IF (BUILD_GUI)
#    PYSIDE_WRAP_RC(
#        RELATIVE Import_QRC_SRCS_REL
#        ABSOLUTE Import_QRC_SRCS
#        RESOURCESResources/Import.qrc
#    )
#ENDIF (BUILD_GUI)

#ADD_CUSTOM_TARGET(ImportRC ALL
#    DEPENDS ${Import_QRC_SRCS}
#)


#IF (BUILD_GUI)
#    fc_target_copy_resource(ImportRC
#        ${CMAKE_CURRENT_BINARY_DIR}
#        ${CMAKE_BINARY_DIR}/Mod/Import
#        Import_rc.py)
#ENDIF (BUILD_GUI)

SET(DXF_Plate
    DxfPlate/blocks10.rub
    DxfPlate/blocks112.rub
    DxfPlate/blocks114.rub
    DxfPlate/classes14.rub
    DxfPlate/entities0.rub
    DxfPlate/entities12.rub
    DxfPlate/entities14.rub
    DxfPlate/header0.rub
    DxfPlate/header12.rub
    DxfPlate/header14.rub
    DxfPlate/objects14.rub
    DxfPlate/tables10.rub
    DxfPlate/tables112.rub
    DxfPlate/tables114.rub
    DxfPlate/tables20.rub
    DxfPlate/tables212.rub
    DxfPlate/tables214.rub
)

set(Import_Scripts
    Init.py
    stepZ.py
    StepPmi.py
    TestStepViews.py
    TestStepAttributes.py
    StepAttributes.py
    StepViews.py
    StepPmiExport.py
    TestStepPmiExport.py
    TestStepPmi.py
    TestStepPmiExtended.py
)

set(Import_PmiFixtures
    TestData/StepPmi/semantic.step
    TestData/StepPmi/independent-inch.step
    TestData/StepPmi/unmapped-modifier.step
    TestData/StepPmi/positive-deviations.step
    TestData/StepPmi/negative-deviations.step
    TestData/StepPmi/absolute-range.step
    TestData/StepPmi/conflicting-signs.step
    TestData/StepPmi/unmatched-signs.step
    TestData/StepPmi/placed-instances.step
    TestData/StepPmi/shared-compartment.step
    TestData/StepPmi/generate.tcl
    TestData/StepPmi/README.txt
)

set(Import_ViewFixtures
    TestData/StepViews/views-mm.step
    TestData/StepViews/views-inch.step
    TestData/StepViews/views-metre.step
    TestData/StepViews/views-nested.step
    TestData/StepViews/views-centered-mm.step
    TestData/StepViews/views-centered-inch.step
    TestData/StepViews/views-centered-metre.step
    TestData/StepViews/README.txt
)
install(FILES ${Import_ViewFixtures} DESTINATION Mod/Import/TestData/StepViews)

set(Import_AttributeFixtures
    TestData/StepAttributes/README.txt
    TestData/StepAttributes/attributes-mm.step
    TestData/StepAttributes/attributes-inch.step
    TestData/StepAttributes/attributes-metre.step
    TestData/StepAttributes/attributes-assembly.step
    TestData/StepAttributes/attributes-nested.step
)
install(FILES ${Import_AttributeFixtures} DESTINATION Mod/Import/TestData/StepAttributes)

if(BUILD_GUI)
    list (APPEND Import_Scripts InitGui.py StepViewsGui.py)
    list (APPEND Import_Scripts TestImportGui.py TestStepPmiGui.py TestStepViewsGui.py)
endif(BUILD_GUI)

add_custom_target(ImportScripts ALL
    SOURCES ${Import_Scripts} ${DXF_Plate} ${Import_PmiFixtures}
)

fc_target_copy_resource(ImportScripts
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/Mod/Import
    ${Import_AttributeFixtures}
)

fc_target_copy_resource(ImportScripts
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/Mod/Import
    ${Import_ViewFixtures}
)

fc_target_copy_resource(ImportScripts
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/Mod/Import
    ${Import_PmiFixtures}
)

install(FILES ${Import_PmiFixtures} DESTINATION Mod/Import/TestData/StepPmi)

fc_target_copy_resource(ImportScripts
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/Mod/Import
    ${Import_Scripts}
)

fc_target_copy_resource(ImportScripts
    ${CMAKE_SOURCE_DIR}/src/Mod/Import
    ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/Mod/Import
    ${DXF_Plate}
)

INSTALL(
    FILES
        ${Import_Scripts}
        #${Import_QRC_SRCS}
    DESTINATION
        Mod/Import
)

INSTALL(
    DIRECTORY
        DxfPlate
    DESTINATION
        ${CMAKE_INSTALL_DATADIR}/Mod/Import
    FILES_MATCHING
        PATTERN "*.rub*"
)
