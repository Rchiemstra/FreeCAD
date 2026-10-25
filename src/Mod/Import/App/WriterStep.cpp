// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2023 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of FreeCAD.                                         *
 *                                                                         *
 *   FreeCAD is free software: you can redistribute it and/or modify it    *
 *   under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the  *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful, but        *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU      *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with FreeCAD. If not, see                               *
 *   <https://www.gnu.org/licenses/>.                                      *
 *                                                                         *
 **************************************************************************/

#include <Standard_Version.hxx>
#include <APIHeaderSection_MakeHeader.hxx>
#include <NCollection_Vector.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <StepData_StepModel.hxx>


#include "WriterStep.h"
#include "StepPmiExport.h"
#include "StepAttributesExport.h"
#include <Base/Exception.h>
#include <App/Application.h>
#include <Mod/Part/App/encodeFilename.h>
#include <Mod/Part/App/Interface.h>

using namespace Import;

WriterStep::WriterStep(const Base::FileInfo& file)  // NOLINT
    : file {file}
{}

void WriterStep::write(Handle(TDocStd_Document) hDoc,
                       const StepPmiExport* semanticPMI, StepAttributesExport* attributes) const  // NOLINT
{
    std::string utf8Name = file.filePath();
    std::string name8bit = Part::encodeFilename(utf8Name);

    const std::string originalScheme = Part::Interface::writeStepScheme();
    struct RestoreSchema {
        std::string scheme;
        ~RestoreSchema() { Part::Interface::writeStepScheme(scheme.c_str()); }
    } restore {originalScheme};
    const bool recoverAttributes = attributes && attributes->hasCandidates();
    if (recoverAttributes) {
        Part::Interface::writeStepScheme("AP242DIS");
    }
    const auto attempt = [&](bool fallback) {
        auto activeAttributes = fallback ? nullptr : attributes;
        STEPCAFControl_Writer writer;
        if (semanticPMI) {
            writer.SetDimTolMode(true);
        }
#if OCC_VERSION_HEX >= 0x070900
        // Temporary workaround for OCCT 7.9+, see: https://github.com/Open-Cascade-SAS/OCCT/issues/1327
        // TODO: Remove or refine the guards or remove when the issue is fixed in OCCT 8.0.1
        writer.SetShapeFixParameters(DESTEP_Parameters::GetDefaultShapeFixParameters());
        ShapeProcess::OperationsFlags aFlags;
        aFlags.set(ShapeProcess::Operation::SplitCommonVertex);
        aFlags.set(ShapeProcess::Operation::DirectFaces);
        writer.SetShapeProcessFlags(aFlags);
#endif
        Part::Interface::writeStepAssembly(Part::Interface::Assembly::On);
        try {
            bool transferred = writer.Transfer(hDoc, STEPControl_AsIs);
            const auto model = writer.ChangeWriter().Model();
            if (recoverAttributes && !attributes->testStage(
                    "transfer", fallback, model.IsNull() ? 0 : model->NbEntities())) {
                transferred = false;
            }
            if (!transferred && recoverAttributes) {
                throw Base::FileException("STEP transfer failed: ", file);
            }
        }
        catch (...) {
            if (!recoverAttributes || fallback) { throw; }
            attributes->fail("attribute transfer failed; geometry/PMI kept");
            return false;
        }
        if (semanticPMI) {
            semanticPMI->complete(writer.ChangeWriter().Model());
        }

        if (activeAttributes) {
            try {
                activeAttributes->complete(writer, hDoc);
            }
            catch (...) {
                attributes->fail("attribute emission failed; geometry/PMI kept");
                return false;
            }
        }

        APIHeaderSection_MakeHeader makeHeader(writer.ChangeWriter().Model());
        Base::Reference<ParameterGrp> hGrp = App::GetApplication()
                                                 .GetUserParameter()
                                                 .GetGroup("BaseApp")
                                                 ->GetGroup("Preferences")
                                                 ->GetGroup("Mod/Part")
                                                 ->GetGroup("STEP");

        // Don't set name because STEP doesn't support UTF-8
        // https://forum.freecad.org/viewtopic.php?f=8&t=52967
        makeHeader.SetAuthorValue(
            1,
            new TCollection_HAsciiString(hGrp->GetASCII("Author", "Author").c_str())
        );
        makeHeader.SetOrganizationValue(1, new TCollection_HAsciiString(hGrp->GetASCII("Company").c_str()));
        makeHeader.SetOriginatingSystem(
            new TCollection_HAsciiString(App::Application::getExecutableName().c_str())
        );
        makeHeader.SetDescriptionValue(1, new TCollection_HAsciiString("FreeCAD Model"));
        IFSelect_ReturnStatus ret;
        try {
            ret = recoverAttributes && !attributes->testStage(
                "serialization", fallback, writer.ChangeWriter().Model()->NbEntities())
                ? IFSelect_RetFail : writer.Write(name8bit.c_str());
        }
        catch (...) {
            if (!recoverAttributes || fallback) { throw; }
            attributes->fail("attribute serialization failed; geometry/PMI kept");
            return false;
        }
        if (ret == IFSelect_RetError || ret == IFSelect_RetFail || ret == IFSelect_RetStop
            || (recoverAttributes && ret != IFSelect_RetDone)) {
            if (recoverAttributes && !fallback) {
                attributes->fail("attribute serialization failed; geometry/PMI kept");
                return false;
            }
            throw Base::FileException("Cannot open file: ", file);
        }
        return true;
    };
    if (!attempt(false)) {
        // The partial writer has been destroyed; the XCAF document retains valid PMI.
        Part::Interface::writeStepScheme(originalScheme.c_str());
        try {
            if (!attempt(true)) { throw RecoveryError("STEP attribute fallback failed: ", file); }
        }
        catch (...) {
            PyErr_Clear();
            throw RecoveryError("STEP attribute fallback failed: ", file);
        }
    }
}
