// SPDX-License-Identifier: LGPL-2.1-or-later

#include "MotionInterferenceGuiPy.h"
#include "TaskInterferenceCheck.h"

#include <App/DocumentObjectPy.h>
#include <Mod/Assembly/App/InterferenceScanPy.h>

namespace AssemblyGui
{
namespace
{
constexpr auto previewName = "AssemblyGui.MotionInterferencePreview";
using Preview = QPointer<TaskInterferenceCheck>;
}

Py::Object showMotionInterferenceResultPy(const Py::Tuple& args)
{
    PyObject* hostPy = nullptr;
    PyObject* snapshot = nullptr;
    int index = 0;
    int raw = 0;
    const char* context = nullptr;
    if (!PyArg_ParseTuple(args.ptr(), "O!Oips", &App::DocumentObjectPy::Type,
                          &hostPy, &snapshot, &index, &raw, &context)) {
        throw Py::Exception();
    }
    auto* host = static_cast<App::DocumentObjectPy*>(hostPy)->getDocumentObjectPtr();
    const auto& result = Assembly::interferenceScanSnapshotFromPy(snapshot);
    if (!host || !host->isAttachedToDocument() || index < 0
        || static_cast<std::size_t>(index) >= result.pairs.size()) {
        throw Py::ValueError("A live assembly and valid event pair are required");
    }
    const auto& pair = result.pairs[static_cast<std::size_t>(index)];
    Assembly::InterferenceComponentOccurrence first, second;
    if (!Assembly::resolveInterferenceComponentOccurrence(
            host, host, result.leaves.at(pair.leafIndexA).occurrenceSubName, first)
        || !Assembly::resolveInterferenceComponentOccurrence(
            host, host, result.leaves.at(pair.leafIndexB).occurrenceSubName, second)) {
        throw Py::ValueError("Event components no longer exist");
    }
    auto* check = showInterferenceCheckPanel(host, first, second);
    if (!check || check->isScanning()) {
        throw Py::RuntimeError("Wait for the current interference check to finish");
    }
    check->presentMotionResult(result, static_cast<std::size_t>(index), raw != 0,
                               QString::fromUtf8(context));
    auto preview = std::make_unique<Preview>(check);
    auto capsule = Py::asObject(PyCapsule_New(preview.get(), previewName, [](PyObject* obj) {
        delete static_cast<Preview*>(PyCapsule_GetPointer(obj, previewName));
    }));
    preview.release();
    return capsule;
}

Py::Object clearMotionInterferenceResultPy(const Py::Tuple& args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args.ptr(), "O", &capsule)) {
        throw Py::Exception();
    }
    auto* preview = static_cast<Preview*>(PyCapsule_GetPointer(capsule, previewName));
    if (!preview) {
        throw Py::Exception();
    }
    if (*preview) {
        (*preview)->clearMotionResult();
        preview->clear();
    }
    return Py::None();
}
}  // namespace AssemblyGui
