// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstddef>
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
};

struct AdmissionDecision
{
    /** True only for a settled exact Part::Box that hits none of the refusals. */
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

/** Owner-side admission check. Does not recompute, touch, or launch a worker. */
[[nodiscard]] AdmissionDecision qualifyForDetachedExecution(const App::DocumentObject& object);

}  // namespace PartQualification
