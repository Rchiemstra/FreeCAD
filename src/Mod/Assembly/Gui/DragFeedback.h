// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <CXX/Objects.hxx>

class SoDragger;
namespace App { class DocumentObject; }
namespace AssemblyGui
{
// Observation only: invoked after placements/solver markers have been displayed.
void notifyDragFeedback(App::DocumentObject* assembly, bool ended = false) noexcept;
void finishDragFeedback(void* data, SoDragger* dragger);
Py::Object setDragFeedbackCallbackPy(const Py::Tuple& args);
#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
Py::Object motionDragEventPy(const Py::Tuple& args);
#endif
}
