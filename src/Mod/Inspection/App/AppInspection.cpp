// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2004 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#include <Base/Console.h>
#include <Base/PyObjectBase.h>

#include <cstdint>
#include <vector>

#include "InspectionFeature.h"
#include "OpenCVPhotoInspectionCompat.h"
#include "PhotoInspectionImage.h"
#include "PhotoInspectionObject.h"
#include "PhotoInspectionProfiles.h"


namespace Inspection
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("Inspection")
    {
        add_varargs_method(
            "photoInspectionCapabilities",
            &Module::photoInspectionCapabilities,
            "photoInspectionCapabilities() -> dict"
        );
        add_varargs_method(
            "validatePhotoInspectionCameraProfile",
            &Module::validatePhotoInspectionCameraProfile,
            "validatePhotoInspectionCameraProfile(json_bytes_or_text) -> dict"
        );
        add_varargs_method(
            "validatePhotoInspectionPrinterProfile",
            &Module::validatePhotoInspectionPrinterProfile,
            "validatePhotoInspectionPrinterProfile(json_bytes_or_text) -> dict"
        );
        add_varargs_method(
            "preflightPhotoInspectionImage",
            &Module::preflightPhotoInspectionImage,
            "preflightPhotoInspectionImage(jpeg_or_png_bytes) -> dict"
        );
        initialize("This module is the Inspection module.");  // register with Python
    }

private:
    static std::string bytesArgument(const Py::Tuple& args)
    {
        if (args.size() != 1) {
            throw Py::TypeError("expected exactly one JSON bytes or text argument");
        }
        PyObject* value = args[0].ptr();
        if (PyBytes_Check(value)) {
            char* data = nullptr;
            Py_ssize_t size = 0;
            if (PyBytes_AsStringAndSize(value, &data, &size) < 0) {
                throw Py::Exception();
            }
            return {data, static_cast<std::size_t>(size)};
        }
        if (PyUnicode_Check(value)) {
            Py_ssize_t size = 0;
            const char* data = PyUnicode_AsUTF8AndSize(value, &size);
            if (!data) {
                throw Py::Exception();
            }
            return {data, static_cast<std::size_t>(size)};
        }
        throw Py::TypeError("profile argument must be bytes or str");
    }

    static Py::Dict profileResultDictionary(const Photo::ProfileResult& result)
    {
        Py::Dict output;
        output.setItem("valid", Py::Boolean(result.valid));
        output.setItem("decision_capable", Py::Boolean(result.decisionCapable));
        output.setItem("diagnostic_code", Py::String(Photo::toString(result.diagnostic.code)));
        output.setItem("message", Py::String(result.diagnostic.message));
        output.setItem("canonical_json", Py::String(result.canonicalJson));
        output.setItem("sha256", Py::String(result.sha256));
        output.setItem("photo_inspection_schema_version", Py::String("1.0"));
        return output;
    }

    Py::Object photoInspectionCapabilities(const Py::Tuple& args)
    {
        if (args.size() != 0) {
            throw Py::TypeError("photoInspectionCapabilities takes no arguments");
        }
        const Photo::OpenCVCapability capability = Photo::OpenCVPhotoInspectionCompat::capability();
        Py::Dict output;
        output.setItem("opencv_requested", Py::Boolean(capability.requested));
        output.setItem("opencv_available", Py::Boolean(capability.available));
        output.setItem("opencv_version", Py::String(capability.version));
        output.setItem("opencv_components", Py::String(capability.components));
        output.setItem("opencv_compatibility_branch", Py::String(capability.compatibilityBranch));
        output.setItem("opencv_reason", Py::String(capability.reason));
        output.setItem("sheet_schema_version", Py::String("1.0"));
        output.setItem("analysis_schema_version", Py::String("1.0"));
        output.setItem("vector_svg", Py::Boolean(true));
        output.setItem("vector_pdf_gui", Py::Boolean(true));
        output.setItem("headless_pdf", Py::Boolean(false));
        output.setItem("synchronous_analysis", Py::Boolean(true));
        output.setItem("typed_mcp_contract_version", Py::String("1.0"));
        output.setItem("conformance_decision_requires_validated_profiles", Py::Boolean(true));
        return output;
    }

    Py::Object validatePhotoInspectionCameraProfile(const Py::Tuple& args)
    {
        Photo::CameraProfile profile;
        return profileResultDictionary(Photo::parseCameraProfile(bytesArgument(args), profile));
    }

    Py::Object validatePhotoInspectionPrinterProfile(const Py::Tuple& args)
    {
        Photo::PrinterProfile profile;
        return profileResultDictionary(Photo::parsePrinterProfile(bytesArgument(args), profile));
    }

    Py::Object preflightPhotoInspectionImage(const Py::Tuple& args)
    {
        const std::string input = bytesArgument(args);
        const std::vector<std::uint8_t> encoded(input.begin(), input.end());
        Photo::EncodedImageInfo info;
        const Photo::ValidationResult validation = Photo::preflightEncodedImage(encoded, {}, info);
        Py::Dict output;
        output.setItem("valid", Py::Boolean(validation.valid));
        output.setItem("diagnostic_code", Py::String(Photo::toString(validation.diagnostic.code)));
        output.setItem("message", Py::String(validation.diagnostic.message));
        output.setItem(
            "format",
            Py::String(
                info.format == Photo::EncodedImageFormat::Jpeg      ? "jpeg"
                    : info.format == Photo::EncodedImageFormat::Png ? "png"
                                                                    : "unknown"
            )
        );
        output.setItem("width", Py::Long(info.width));
        output.setItem("height", Py::Long(info.height));
        output.setItem("channels", Py::Long(info.channels));
        output.setItem("bits_per_channel", Py::Long(info.bitsPerChannel));
        output.setItem("encoded_bytes", Py::Long(static_cast<long>(info.encodedBytes)));
        output.setItem("decoded_bytes", Py::Long(static_cast<long>(info.decodedBytes)));
        output.setItem("photo_inspection_schema_version", Py::String("1.0"));
        return output;
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace Inspection


/* Python entry */
PyMOD_INIT_FUNC(Inspection)
{
    // ADD YOUR CODE HERE
    //
    //
    PyObject* mod = Inspection::initModule();
    Base::Console().log("Loading Inspection module… done\n");
    // clang-format off
    Inspection::PropertyDistanceList    ::init();
    Inspection::Feature                 ::init();
    Inspection::Group                   ::init();
    Inspection::Photo::PhotoInspectionSheet ::init();
    Inspection::Photo::PhotoInspectionResult::init();
    // clang-format on
    PyMOD_Return(mod);
}
