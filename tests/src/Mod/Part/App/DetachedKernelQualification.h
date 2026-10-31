// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace App
{
class DocumentObject;
}

namespace PartQualification
{

/** Audit classes from doc/parallel-recompute-m0.md §2. Safe is S, NeedsSerialization is N,
 *  Unknown is U. S is an immutable copied value. It does not qualify the OCC call that
 *  consumes the value. */
enum class EffectClass
{
    Safe,
    NeedsSerialization,
    Unknown
};

/** One declared read, output, or forbidden effect. The text is the contract, not a hook. */
struct QualificationField
{
    const char* name;
    EffectClass classification;
    const char* boundary;
};

/** What an exact feature kernel must declare before detached execution is even considered.
 *  This is data. qualifyForDetachedExecution() checks an instance against it. Neither one
 *  schedules work or changes Document::recompute. */
struct KernelQualificationTemplate
{
    const char* exactType;
    EffectClass liveRecompute;
    EffectClass proposedKernel;
    const QualificationField* reads;
    std::size_t readCount;
    const QualificationField* outputs;
    std::size_t outputCount;
    const QualificationField* forbidden;
    std::size_t forbiddenCount;
    const char* failureSemantics;
    const char* const* preferenceReadingTypes;
    std::size_t preferenceReadingTypeCount;
    /** Requirements the audit states that this check cannot yet evaluate.
     *  They are copied onto every AdmissionDecision. While any remain,
     *  detached stays false. */
    const char* const* pending;
    std::size_t pendingCount;
};

inline constexpr QualificationField kNarrowBoxReads[] = {
    {"Length",
     EffectClass::Safe,
     "Owner freezes the internal-unit double. The live getter stays on the owner."},
    {"Width",
     EffectClass::Safe,
     "Owner freezes the internal-unit double. The live getter stays on the owner."},
    {"Height",
     EffectClass::Safe,
     "Owner freezes the internal-unit double. The live getter stays on the owner."},
    {"Precision::Confusion",
     EffectClass::Safe,
     "Owner freezes the tolerance used for the Length, Width, Height checks, in that order."},
    {"Placement",
     EffectClass::NeedsSerialization,
     "Owner copies the value. The worker does not read the live property or a parent transform."},
    {"old Shape map and tag",
     EffectClass::NeedsSerialization,
     "Owner-only publication input. Do not send the document hasher or a shared TopoShape."},
    {"exact type, schema, extensions",
     EffectClass::NeedsSerialization,
     "Exact Part::Box and the built-in Part::AttachExtension only. Any other extension refuses."},
    {"_ElementMapVersion and incoming element references",
     EffectClass::NeedsSerialization,
     "The stored version must be non-empty and equal to the current geometry version. "
     "A nonempty incoming element-reference set is not admitted."},
    {"mutating observers",
     EffectClass::NeedsSerialization,
     "Object signalChanged and signalBeforeChange slots refuse. Document-level callback "
     "write sets stay pending."},
    {"capture and commit invalidation",
     EffectClass::NeedsSerialization,
     "Point-in-time identity, schema, version and freeze/skip are checked. The capture-versus-commit "
     "revision fence is pending and keeps detached false."},
    {"identity, document epoch, touch and restore flags",
     EffectClass::NeedsSerialization,
     "Owner admission. Partial restore, Restoring, RecomputeOnRestore and Shape User1 refuse."},
};

inline constexpr QualificationField kNarrowBoxOutputs[] = {
    {"Shape",
     EffectClass::NeedsSerialization,
     "Owner publication only, using setValue(shape, resetElementMap=false) while recomputing."},
    {"recompute status and diagnostics",
     EffectClass::NeedsSerialization,
     "Owner bookkeeping. Early dimension failure writes no Shape."},
    {"element map and naming",
     EffectClass::NeedsSerialization,
     "Owner publication. A raw OCC kernel does not invent a map or hasher id."},
    {"Placement",
     EffectClass::NeedsSerialization,
     "Not an output. The captured Placement stays unchanged."},
    {"ShapeMaterial",
     EffectClass::NeedsSerialization,
     "Not an output. Construction must not run a material-manager lookup on the worker."},
};

inline constexpr QualificationField kNarrowBoxForbidden[] = {
    {"live Document, DocumentObject or property access",
     EffectClass::NeedsSerialization,
     "Worker kernel sees frozen values only. No live getters."},
    {"Python interpreter, GIL or proxy",
     EffectClass::NeedsSerialization,
     "FeaturePython stays on the owner. supportsAsyncRecompute is not this contract."},
    {"application preference reads during execute",
     EffectClass::NeedsSerialization,
     "Part::Cut, Part::Fuse and Part::Common read CheckModel while executing. Not admitted."},
    {"process signal policy and global Boolean fuzzy",
     EffectClass::NeedsSerialization,
     "Not used by the Box kernel. Still forbidden for a detached worker."},
    {"shared StringHasher and TopoShape caches",
     EffectClass::NeedsSerialization,
     "Do not share TShapes, caches or the document hasher with the worker."},
    {"creating an App document or object",
     EffectClass::NeedsSerialization,
     "A temporary object would reintroduce ShapeMaterial and signals."},
    {"element-reference registry updates",
     EffectClass::NeedsSerialization,
     "GeoFeature::onChanged updates stay on the owner."},
    {"dynamic properties, including _Part_ShapeCache",
     EffectClass::NeedsSerialization,
     "The audit's _ShapeCache row. The property name in this tree is _Part_ShapeCache."},
    {"expression evaluation",
     EffectClass::NeedsSerialization,
     "Bindings, including expressions that read other objects, stay a serial owner phase."},
    {"attachment reposition or support links",
     EffectClass::NeedsSerialization,
     "Inactive AttachExtension is allowed. Active, pending or base attachment is not."},
    {"links, groups, external documents, parent transforms, non-unit scale",
     EffectClass::NeedsSerialization,
     "Common-framework exclusions. Own Placement is a frozen input, not a parent transform."},
};

inline constexpr const char* kPreferenceReadingExecuteTypes[] = {
    "Part::Cut",
    "Part::Fuse",
    "Part::Common",
};

inline constexpr const char* kNarrowBoxPending[] = {
    "doc/parallel-recompute-m0.md §2 lines 199-206: capture/commit invalidation "
    "(document epoch, property revisions, operand-closure revisions, configuration "
    "generation and the unknown-mutation guard) is not compared yet",
    "doc/parallel-recompute-m0.md §2 line 174: document-level callback write sets are not enumerated",
    "doc/parallel-recompute-m0.md §2 line 85: base-execute extension effects are not replayed; "
    "only the exact extension schema is checked",
};

inline constexpr char kNarrowBoxFailureSemantics[] =
    "Check Length, Width, then Height against the frozen tolerance. A too-small dimension "
    "returns the serial diagnostic and does not replace Shape. An OCC exception is translated "
    "the same way. The worker writes no undo entry. Owner status and touch handling stay on "
    "the owner.";

inline const KernelQualificationTemplate& narrowBoxQualification()
{
    static const KernelQualificationTemplate value = {
        "Part::Box",
        EffectClass::NeedsSerialization,
        EffectClass::Unknown,
        kNarrowBoxReads,
        sizeof(kNarrowBoxReads) / sizeof(kNarrowBoxReads[0]),
        kNarrowBoxOutputs,
        sizeof(kNarrowBoxOutputs) / sizeof(kNarrowBoxOutputs[0]),
        kNarrowBoxForbidden,
        sizeof(kNarrowBoxForbidden) / sizeof(kNarrowBoxForbidden[0]),
        kNarrowBoxFailureSemantics,
        kPreferenceReadingExecuteTypes,
        sizeof(kPreferenceReadingExecuteTypes) / sizeof(kPreferenceReadingExecuteTypes[0]),
        kNarrowBoxPending,
        sizeof(kNarrowBoxPending) / sizeof(kNarrowBoxPending[0]),
    };
    return value;
}

inline constexpr QualificationField kNarrowBooleanReads[] = {
    {"operation kind",
     EffectClass::Safe,
     "Owner freezes Cut, Fuse or Common from the exact type. The worker does not read the live object."},
    {"Base resolved shape, placement and element map",
     EffectClass::NeedsSerialization,
     "Owner capture. Direct same-document Part features only. Freeze the resolved shape, placement and input names."},
    {"Tool resolved shape, placement and element map",
     EffectClass::NeedsSerialization,
     "Same capture as Base, including the operand owner and its element names."},
    {"operand owner and label",
     EffectClass::NeedsSerialization,
     "Failure text reads Label and the shape type. Freeze both on the owner."},
    {"Refine",
     EffectClass::Safe,
     "Owner freezes the property. Refine=true is refused. RefineModel is a construction preference, not an execute read."},
    {"Boolean fuzzy factor",
     EffectClass::Safe,
     "Owner freezes FuzzyHelper::getBooleanFuzzy. The worker must not write it or call withBooleanFuzzy."},
    {"CheckModel",
     EffectClass::Safe,
     "Owner freezes the preference. The worker must not reread App::GetApplication parameters."},
    {"Precision::Confusion",
     EffectClass::Safe,
     "Owner freezes the tolerance in the fuzzy bounding-box formula."},
    {"Base, target and default ShapeMaterial",
     EffectClass::NeedsSerialization,
     "Read the target's current ShapeMaterial, Base's ShapeMaterial and the default material UUID. "
     "Copy Base only when the target still has the default and Base differs. A custom target material "
     "is preserved. A change to the target material between capture and commit invalidates that frozen decision."},
    {"old Shape map and tag",
     EffectClass::NeedsSerialization,
     "Owner-only publication input. Do not send the document hasher or a shared TopoShape."},
    {"exact type, schema, extensions",
     EffectClass::NeedsSerialization,
     "Exact Part::Cut, Part::Fuse or Part::Common and no extensions. Any extension refuses."},
    {"_ElementMapVersion and incoming element references",
     EffectClass::NeedsSerialization,
     "The stored version must be non-empty and equal to the current geometry version. "
     "A nonempty incoming element-reference set is not admitted."},
    {"mutating observers",
     EffectClass::NeedsSerialization,
     "Object signalChanged and signalBeforeChange slots refuse. Document-level callback write sets stay pending."},
    {"capture and commit invalidation",
     EffectClass::NeedsSerialization,
     "Point-in-time identity is checked. The capture-versus-commit fence, including operands, is pending."},
    {"identity, document epoch, touch and restore flags",
     EffectClass::NeedsSerialization,
     "Owner admission. Partial restore, Restoring, RecomputeOnRestore and Shape User1 refuse."},
    {"History",
     EffectClass::NeedsSerialization,
     "Binary execute does not assign History. A non-empty History refuses. Do not invent a History output."},
};

inline constexpr QualificationField kNarrowBooleanOutputs[] = {
    {"Shape",
     EffectClass::NeedsSerialization,
     "Owner publication only. Early failure does not call the Shape setter."},
    {"element map and history-derived names",
     EffectClass::NeedsSerialization,
     "Owner publication. A raw BRep has no map. History-derived names are applied on the owner."},
    {"recompute status and diagnostics",
     EffectClass::NeedsSerialization,
     "Owner bookkeeping. The worker returns a diagnostic string and writes no document status."},
    {"ShapeMaterial",
     EffectClass::NeedsSerialization,
     "Owner write. Copy Base when the target's current material is still the default. Preserve a custom target material."},
    {"History",
     EffectClass::NeedsSerialization,
     "Not an output of binary execute. The property stays unchanged."},
    {"Placement",
     EffectClass::NeedsSerialization,
     "Not an output. The captured Placement stays unchanged."},
};

inline constexpr QualificationField kNarrowBooleanForbidden[] = {
    {"live Document, DocumentObject or property access",
     EffectClass::NeedsSerialization,
     "Worker kernel sees frozen shapes and policy values only."},
    {"Python interpreter, GIL or proxy during operand resolution",
     EffectClass::NeedsSerialization,
     "getTopoShape can take the GIL and call proxies. Resolution stays on the owner."},
    {"rereading CheckModel or any application preference",
     EffectClass::NeedsSerialization,
     "The frozen CheckModel bool is the worker's policy. No GetApplication read."},
    {"process signal policy",
     EffectClass::NeedsSerialization,
     "Base::SignalException and Part::SignalException stay unqualified for a worker."},
    {"global Boolean fuzzy writes",
     EffectClass::NeedsSerialization,
     "Freeze the factor. withBooleanFuzzy and setBooleanFuzzy are forbidden on the worker."},
    {"OCC parallel-mode and progress globals",
     EffectClass::NeedsSerialization,
     "Do not call BOPAlgo_Options::SetParallelMode, OSD_Parallel::SetUseOcctThreads, or the global sequencer."},
    {"shared TShape, TopoShape cache and StringHasher",
     EffectClass::NeedsSerialization,
     "Deep-copy operand geometry. Do not share TShapes, _Part_ShapeCache or the document hasher."},
    {"creating an App document or object",
     EffectClass::NeedsSerialization,
     "A temporary object would reintroduce ShapeMaterial and signals."},
    {"element-reference registry updates",
     EffectClass::NeedsSerialization,
     "GeoFeature::onChanged updates stay on the owner."},
    {"dynamic properties, including _Part_ShapeCache",
     EffectClass::NeedsSerialization,
     "Operand resolution must not create _Part_ShapeCache on the worker."},
    {"expression evaluation",
     EffectClass::NeedsSerialization,
     "Bindings stay a serial owner phase."},
    {"attachment reposition or support links",
     EffectClass::NeedsSerialization,
     "No extensions are admitted on the Boolean. Active attachment is not."},
    {"operand Link, group, external document or non-unit scale",
     EffectClass::NeedsSerialization,
     "Direct native same-document Part features only. Refuse a non-feature, an unknown extension, "
     "active attachment, and a source closure that is not itself that native set. Own Placement is a frozen input."},
    {"Refine=true",
     EffectClass::NeedsSerialization,
     "Refine stays excluded until its static classifiers are audited."},
};

inline constexpr const char* kNarrowBooleanPending[] = {
    "doc/parallel-recompute-m0.md §2 lines 145 and 199-206: capture/commit invalidation, "
    "including operand-closure revisions and a target ShapeMaterial change before commit, is not compared yet",
    "doc/parallel-recompute-m0.md §2 line 174: document-level callback write sets are not enumerated",
    "doc/parallel-recompute-m0.md §2 line 177: Base::SignalException and Part::SignalException "
    "remain unqualified for a worker",
    "doc/parallel-recompute-m0.md §2 line 179: OCC kernel TShape and cache independence stays U",
    "doc/parallel-recompute-m0.md §2 line 180: StringHasher allocation stays owner-only",
    "doc/parallel-recompute-m0.md §2 line 183: material-manager mutation stays owner-only",
    "doc/parallel-recompute-m0.md §2 line 147: failure after the Shape or material setter is not compared",
    "doc/parallel-recompute-m0.md §2 line 141: BRepCheck internals stay U",
    "doc/parallel-recompute-m0.md §2 line 185: Refine stays excluded",
    "doc/parallel-recompute-m0.md §2 line 178: FCBRepAlgoAPI still reads BooleanFuzzy during Build; "
    "the harness only proves that read does not change the value",
    "doc/parallel-recompute-m0.md M0.2b Boolean venues: PropertyPartShape::Save omits element "
    "maps while isExporting, so a process venue requires owner-side re-naming. That M1 "
    "requirement stays pending",
};

inline constexpr char kNarrowBooleanFailureSemantics[] =
    "Missing operands, null shapes, a Boolean that is not done, a null result and a "
    "CheckModel-invalid result return before the Shape setter. History is not assigned. "
    "ShapeMaterial is not written on those paths. A failure after the Shape or material "
    "setter is pending and is not treated as write-free.";

inline const KernelQualificationTemplate& narrowBooleanQualification(std::string_view exactType)
{
    const auto make = [](const char* type) {
        return KernelQualificationTemplate {
            type,
            EffectClass::NeedsSerialization,
            EffectClass::Unknown,
            kNarrowBooleanReads,
            sizeof(kNarrowBooleanReads) / sizeof(kNarrowBooleanReads[0]),
            kNarrowBooleanOutputs,
            sizeof(kNarrowBooleanOutputs) / sizeof(kNarrowBooleanOutputs[0]),
            kNarrowBooleanForbidden,
            sizeof(kNarrowBooleanForbidden) / sizeof(kNarrowBooleanForbidden[0]),
            kNarrowBooleanFailureSemantics,
            kPreferenceReadingExecuteTypes,
            0,
            kNarrowBooleanPending,
            sizeof(kNarrowBooleanPending) / sizeof(kNarrowBooleanPending[0]),
        };
    };
    static const KernelQualificationTemplate cut = make("Part::Cut");
    static const KernelQualificationTemplate fuse = make("Part::Fuse");
    static const KernelQualificationTemplate common = make("Part::Common");
    if (exactType == "Part::Fuse") {
        return fuse;
    }
    if (exactType == "Part::Common") {
        return common;
    }
    return cut;
}

enum class AdmissionRefusal
{
    NotExactType,
    PythonFeature,
    Link,
    Group,
    ScalingOrPlacementTransform,
    ExternalReference,
    ExpressionBinding,
    ExpressionReadsOtherObject,
    PreferenceReadDuringExecute,
    DynamicShapeCache,
    DynamicProperty,
    ColdOrRestore,
    ActiveAttachment,
    IncomingElementReference,
    ExtensionSchema,
    UnsettledElementMapVersion,
    MutatingObserver,
    FreezeOrSkip,
    OperandMissing,
    OperandLink,
    OperandGroup,
    OperandExternal,
    OperandScaledTransform,
    OperandPython,
    OperandShapeCache,
    RefineEnabled,
    HistoryWritten,
    OperandNotFeature,
    OperandExtension,
    OperandAttachment,
    OperandSourceClosure,
};

/** Test-only. suppress skips exactly one guard so a negative fixture can prove it. */
struct QualificationProbe
{
    bool suppress {false};
    AdmissionRefusal guard {AdmissionRefusal::NotExactType};
};

struct AdmissionDecision
{
    /** True when no refusal fired. Pending rows still force detached false. */
    bool admitted {false};
    /** This check never selects a worker. Production scheduling is unchanged. */
    bool detached {false};
    EffectClass liveRecompute {EffectClass::NeedsSerialization};
    EffectClass proposedKernel {EffectClass::Unknown};
    std::vector<AdmissionRefusal> reasons;
    std::vector<const char*> citations;
    /** Unchecked audit requirements. Non-empty forces detached false. */
    std::vector<const char*> pending;

    [[nodiscard]] bool has(AdmissionRefusal reason) const;
};

[[nodiscard]] const char* admissionCitation(AdmissionRefusal reason);

/** Owner-side admission check. Does not recompute, touch, or launch a worker.
 *  probe is test-only. The default runs every guard. */
[[nodiscard]] AdmissionDecision qualifyForDetachedExecution(
    const App::DocumentObject& object,
    QualificationProbe probe = {});

}  // namespace PartQualification
