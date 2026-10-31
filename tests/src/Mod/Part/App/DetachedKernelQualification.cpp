// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DetachedKernelQualification.h"

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/Expression.h>
#include <App/GeoFeature.h>
#include <App/GeoFeatureGroupExtension.h>
#include <App/GroupExtension.h>
#include <App/Link.h>
#include <App/PropertyLinks.h>
#include <Mod/Part/App/AttachExtension.h>
#include <Mod/Part/App/FeaturePartBoolean.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <Mod/Part/App/FeaturePartCommon.h>
#include <Mod/Part/App/FeaturePartCut.h>
#include <Mod/Part/App/FeaturePartFuse.h>

#include <gp_Trsf.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <string_view>

namespace PartQualification
{
namespace
{

bool sameText(std::string_view left, std::string_view right)
{
    return left == right;
}

bool guardSuppressed(const QualificationProbe& probe, AdmissionRefusal reason)
{
    return probe.suppress && probe.guard == reason;
}

void addReason(AdmissionDecision& decision, AdmissionRefusal reason, const QualificationProbe& probe)
{
    if (guardSuppressed(probe, reason) || decision.has(reason)) {
        return;
    }
    decision.reasons.push_back(reason);
    decision.citations.push_back(admissionCitation(reason));
}

bool preferenceReadDuringExecute(std::string_view typeName, const KernelQualificationTemplate& contract)
{
    for (std::size_t index = 0; index < contract.preferenceReadingTypeCount; ++index) {
        if (sameText(typeName, contract.preferenceReadingTypes[index])) {
            return true;
        }
    }
    return false;
}

bool shapeCacheName(std::string_view name)
{
    return name.find("ShapeCache") != std::string_view::npos;
}

bool externalDocument(const App::DocumentObject& object, const App::DocumentObject& other)
{
    const auto* otherDocument = other.getDocument();
    return otherDocument && otherDocument != object.getDocument();
}

bool isShapeCacheProperty(std::string_view name, const App::Property* property)
{
    const bool cacheType = property
        && sameText(property->getTypeId().getName(), "Part::PropertyShapeCache");
    return cacheType || shapeCacheName(name);
}

bool pythonFeature(const App::DocumentObject& object)
{
    const auto typeName = std::string_view(object.getTypeId().getName());
    return object.requiresDocumentThreadExecutionDeclaration()
        || typeName.find("Python") != std::string_view::npos;
}

bool grouped(const App::DocumentObject& object)
{
    return object.hasExtension(App::GroupExtension::getExtensionClassTypeId())
        || App::GroupExtension::getGroupOfObject(&object)
        || App::GeoFeatureGroupExtension::getGroupOfObject(&object);
}

bool nonUnit(double value)
{
    return std::abs(value - 1.0) > 1e-12;
}

bool objectIsDetached(const App::DocumentObject& object)
{
    return !object.isAttachedToDocument();
}

bool objectIsRestoring(const App::DocumentObject& object)
{
    return object.isRestoring();
}

bool documentIsRestoring(const App::Document* document)
{
    return document && document->testStatus(App::Document::Restoring);
}

bool documentIsPartial(const App::Document* document)
{
    return document && document->testStatus(App::Document::PartialDoc);
}

bool documentIsPartialRestore(const App::Document* document)
{
    return document && document->testStatus(App::Document::PartialRestore);
}

bool documentRecomputesOnRestore(const App::Document* document)
{
    return document && document->testStatus(App::Document::RecomputeOnRestore);
}

bool shapeUser1(const App::DocumentObject& object)
{
    const auto* feature = dynamic_cast<const Part::Feature*>(&object);
    return feature && feature->Shape.testStatus(App::Property::User1);
}

bool coldOrRestore(const App::DocumentObject& object)
{
    const auto* document = object.getDocument();
    if (!document || objectIsDetached(object) || objectIsRestoring(object)) {
        return true;
    }
    return documentIsRestoring(document) || documentIsPartial(document)
        || documentIsPartialRestore(document) || documentRecomputesOnRestore(document)
        || shapeUser1(object);
}

bool objectIsFrozen(const App::DocumentObject& object)
{
    return object.isFreezed();
}

bool documentSkipsRecompute(const App::DocumentObject& object)
{
    const auto* document = object.getDocument();
    return document && document->testStatus(App::Document::SkipRecompute);
}

bool freezeOrSkip(const App::DocumentObject& object)
{
    return objectIsFrozen(object) || documentSkipsRecompute(object);
}

bool exactExtensionSchema(const App::DocumentObject& object)
{
    // extensionBegin() is non-const. This walk only reads the type keys.
    auto& readable = const_cast<App::DocumentObject&>(object);
    std::vector<std::string> names;
    for (auto it = readable.extensionBegin(); it != readable.extensionEnd(); ++it) {
        names.emplace_back(it->first.getName());
    }
    std::sort(names.begin(), names.end());
    return names.size() == 1 && names.front() == "Part::AttachExtension";
}

bool unsettledElementMapVersion(const App::DocumentObject& object)
{
    const auto* geo = dynamic_cast<const App::GeoFeature*>(&object);
    if (!geo) {
        return true;
    }
    const auto* geometry = geo->getPropertyOfGeometry();
    if (!geometry) {
        return true;
    }
    const auto stored = geo->_ElementMapVersion.getStrValue();
    const auto current = object.getElementMapVersion(geometry);
    return stored.empty() || stored != current;
}

bool mutatingObserver(const App::DocumentObject& object)
{
    return object.signalChanged.num_slots() > 0 || object.signalBeforeChange.num_slots() > 0;
}

bool activeAttachment(const App::DocumentObject& object)
{
    const auto* extension = object.getExtensionByType<Part::AttachExtension>(/*no_except=*/true);
    if (!extension) {
        return false;
    }
    const auto inactive = [](const App::PropertyEnumeration* mode) {
        if (!mode) {
            return true;
        }
        const char* value = mode->getValueAsString();
        return !value || value[0] == '\0' || sameText(value, "Deactivated");
    };
    const auto supportEmpty = [](const App::PropertyLinkBase* support) {
        return !support || support->linkedObjects(/*all=*/true).empty();
    };
    if (!inactive(&extension->MapMode) || !supportEmpty(&extension->AttachmentSupport)) {
        return true;
    }
    const auto base = extension->getProperties(/*base=*/true);
    return !inactive(base.mapMode) || !supportEmpty(base.attachment);
}

bool linkScale(const App::DocumentObject& object)
{
    const auto* link = dynamic_cast<const App::Link*>(&object);
    if (!link) {
        return false;
    }
    if (nonUnit(link->Scale.getValue())) {
        return true;
    }
    const auto scale = link->getScaleVector();
    return nonUnit(scale.x) || nonUnit(scale.y) || nonUnit(scale.z);
}

bool parentTransform(const App::DocumentObject& object)
{
    const auto* geo = dynamic_cast<const App::GeoFeature*>(&object);
    if (!geo) {
        return false;
    }
    const auto local = geo->Placement.getValue();
    const auto global = App::GeoFeature::getGlobalPlacement(&object);
    return !global.isSame(local, 1e-9);
}

void collectLinkAndExternal(const App::DocumentObject& object,
                            AdmissionDecision& decision,
                            const QualificationProbe& probe,
                            const App::Property* skip0 = nullptr,
                            const App::Property* skip1 = nullptr)
{
    if (object.isLink()) {
        addReason(decision, AdmissionRefusal::Link, probe);
    }
    std::vector<App::Property*> properties;
    object.getPropertyList(properties);
    for (const auto* property : properties) {
        if (!property || property == &object.ExpressionEngine || property == skip0 || property == skip1) {
            continue;
        }
        const auto* link = dynamic_cast<const App::PropertyLinkBase*>(property);
        if (!link) {
            continue;
        }
        for (const auto* other : link->linkedObjects(/*all=*/true)) {
            if (!other) {
                continue;
            }
            if (externalDocument(object, *other)) {
                addReason(decision, AdmissionRefusal::ExternalReference, probe);
            }
            else if (other != &object) {
                addReason(decision, AdmissionRefusal::Link, probe);
            }
        }
    }
    for (const auto* other : object.getOutList()) {
        if (other && externalDocument(object, *other)) {
            addReason(decision, AdmissionRefusal::ExternalReference, probe);
        }
    }
}

void collectExpressions(const App::DocumentObject& object,
                        AdmissionDecision& decision,
                        const QualificationProbe& probe)
{
    for (const auto& entry : object.ExpressionEngine.getExpressions()) {
        const auto* expression = entry.second;
        if (!expression) {
            continue;
        }
        addReason(decision, AdmissionRefusal::ExpressionBinding, probe);
        for (const auto& dep : expression->getDepObjects()) {
            const auto* other = dep.first;
            if (!other || other == &object) {
                continue;
            }
            addReason(decision, AdmissionRefusal::ExpressionReadsOtherObject, probe);
            if (externalDocument(object, *other)) {
                addReason(decision, AdmissionRefusal::ExternalReference, probe);
            }
        }
    }
}

void collectDynamicProperties(const App::DocumentObject& object,
                              AdmissionDecision& decision,
                              const QualificationProbe& probe)
{
    for (const auto& name : object.getDynamicPropertyNames()) {
        const auto* property = object.getDynamicPropertyByName(name.c_str());
        if (isShapeCacheProperty(name, property)) {
            addReason(decision, AdmissionRefusal::DynamicShapeCache, probe);
        }
        else {
            addReason(decision, AdmissionRefusal::DynamicProperty, probe);
        }
    }
}

enum class BooleanKind
{
    None,
    Cut,
    Fuse,
    Common
};

BooleanKind booleanKind(const App::DocumentObject& object)
{
    const auto typeName = std::string_view(object.getTypeId().getName());
    if (sameText(typeName, "Part::Cut") && object.getTypeId() == Part::Cut::getClassTypeId()) {
        return BooleanKind::Cut;
    }
    if (sameText(typeName, "Part::Fuse") && object.getTypeId() == Part::Fuse::getClassTypeId()) {
        return BooleanKind::Fuse;
    }
    if (sameText(typeName, "Part::Common") && object.getTypeId() == Part::Common::getClassTypeId()) {
        return BooleanKind::Common;
    }
    return BooleanKind::None;
}

bool noExtensions(const App::DocumentObject& object)
{
    auto& readable = const_cast<App::DocumentObject&>(object);
    return readable.extensionBegin() == readable.extensionEnd();
}

bool operandIsNativePartFeature(const App::DocumentObject& operand)
{
    if (!operand.isDerivedFrom(Part::Feature::getClassTypeId())) {
        return false;
    }
    const auto name = std::string_view(operand.getTypeId().getName());
    return name.starts_with("Part::") && name.find("Python") == std::string_view::npos;
}

bool knownOperandExtensions(const App::DocumentObject& object)
{
    auto& readable = const_cast<App::DocumentObject&>(object);
    for (auto it = readable.extensionBegin(); it != readable.extensionEnd(); ++it) {
        if (!sameText(it->first.getName(), "Part::AttachExtension")) {
            return false;
        }
    }
    return true;
}

std::vector<const App::DocumentObject*> directSources(const App::DocumentObject& object)
{
    std::vector<const App::DocumentObject*> sources;
    auto add = [&](const App::DocumentObject* other) {
        if (!other || other == &object) {
            return;
        }
        if (std::find(sources.begin(), sources.end(), other) == sources.end()) {
            sources.push_back(other);
        }
    };
    for (const auto* other : object.getOutList()) {
        add(other);
    }
    std::vector<App::Property*> properties;
    object.getPropertyList(properties);
    for (const auto* property : properties) {
        const auto* link = dynamic_cast<const App::PropertyLinkBase*>(property);
        if (!link) {
            continue;
        }
        for (const auto* other : link->linkedObjects(/*all=*/true)) {
            add(other);
        }
    }
    auto& readable = const_cast<App::DocumentObject&>(object);
    add(readable.getLinkedObject(/*recurse=*/false));
    return sources;
}

bool operandScaled(const App::DocumentObject& operand);
bool operandExternal(const App::DocumentObject& owner, const App::DocumentObject& operand);
bool operandShapeCache(const App::DocumentObject& operand);

bool sourceAllowed(const App::DocumentObject& owner,
                   const App::DocumentObject& source,
                   std::set<const App::DocumentObject*>& seen)
{
    if (!seen.insert(&source).second) {
        return true;
    }
    if (!operandIsNativePartFeature(source) || !knownOperandExtensions(source)
        || activeAttachment(source) || grouped(source) || operandExternal(owner, source)
        || operandScaled(source) || pythonFeature(source) || operandShapeCache(source)
        || source.isLink() || dynamic_cast<const App::Link*>(&source)) {
        return false;
    }
    for (const auto* next : directSources(source)) {
        if (next && !sourceAllowed(owner, *next, seen)) {
            return false;
        }
    }
    return true;
}

// Each predicate is the guard itself. A scratch edit of its return is the
// break-to-prove. QualificationProbe only suppresses the emitted reason.
bool operandLinkPredicate(const App::DocumentObject& operand)
{
    const bool linked = operand.isLink() || dynamic_cast<const App::Link*>(&operand) != nullptr;
    return linked;  // prc-predicate OperandLink
}

bool operandGroupPredicate(const App::DocumentObject& operand)
{
    const bool inGroup = grouped(operand);
    return inGroup;  // prc-predicate OperandGroup
}

bool operandExternalPredicate(const App::DocumentObject& owner, const App::DocumentObject& operand)
{
    const bool external = operandExternal(owner, operand);
    return external;  // prc-predicate OperandExternal
}

bool operandScaledPredicate(const App::DocumentObject& operand)
{
    const bool scaled = operandScaled(operand);
    return scaled;  // prc-predicate OperandScaledTransform
}

bool operandPythonPredicate(const App::DocumentObject& operand)
{
    const bool python = pythonFeature(operand);
    return python;  // prc-predicate OperandPython
}

bool operandShapeCachePredicate(const App::DocumentObject& operand)
{
    const bool cached = operandShapeCache(operand);
    return cached;  // prc-predicate OperandShapeCache
}

bool operandNotFeaturePredicate(const App::DocumentObject& operand)
{
    const bool linkOrPython = operand.isLink() || dynamic_cast<const App::Link*>(&operand) != nullptr
        || pythonFeature(operand);
    const bool rejected = !linkOrPython && !operandIsNativePartFeature(operand);
    return rejected;  // prc-predicate OperandNotFeature
}

bool operandExtensionPredicate(const App::DocumentObject& operand)
{
    const bool rejected = operandIsNativePartFeature(operand) && !knownOperandExtensions(operand);
    return rejected;  // prc-predicate OperandExtension
}

bool operandAttachmentPredicate(const App::DocumentObject& operand)
{
    const bool rejected = operandIsNativePartFeature(operand) && activeAttachment(operand);
    return rejected;  // prc-predicate OperandAttachment
}

bool operandSourceClosurePredicate(const App::DocumentObject& owner, const App::DocumentObject& operand)
{
    std::set<const App::DocumentObject*> seen;
    seen.insert(&operand);
    bool rejected = false;
    if (operandIsNativePartFeature(operand)) {
        for (const auto* next : directSources(operand)) {
            if (next && !sourceAllowed(owner, *next, seen)) {
                rejected = true;
                break;
            }
        }
    }
    return rejected;  // prc-predicate OperandSourceClosure
}

bool operandMissingPredicate(const App::DocumentObject* base, const App::DocumentObject* tool)
{
    const bool missing = !base || !tool;
    return missing;  // prc-predicate OperandMissing
}

bool refineEnabledPredicate(const Part::Boolean& boolean)
{
    const bool enabled = boolean.Refine.getValue();
    return enabled;  // prc-predicate RefineEnabled
}

bool historyWrittenPredicate(const Part::Boolean& boolean)
{
    const bool written = boolean.History.getSize() != 0;
    return written;  // prc-predicate HistoryWritten
}

bool locationScaled(const TopoDS_Shape& shape)
{
    if (shape.IsNull()) {
        return false;
    }
    const gp_Trsf& trsf = shape.Location().Transformation();
    if (trsf.Form() == gp_Scale) {
        return true;
    }
    return std::abs(trsf.ScaleFactor() - 1.0) > 1e-12;
}

bool operandScaled(const App::DocumentObject& operand)
{
    if (linkScale(operand)) {
        return true;
    }
    const auto* feature = dynamic_cast<const Part::Feature*>(&operand);
    return feature && locationScaled(feature->Shape.getValue());
}

bool operandExternal(const App::DocumentObject& owner, const App::DocumentObject& operand)
{
    if (externalDocument(owner, operand)) {
        return true;
    }
    const auto* link = dynamic_cast<const App::Link*>(&operand);
    if (!link) {
        return false;
    }
    const auto* linked = link->getLinkedObject(/*recurse=*/false);
    if (linked && linked != &operand && externalDocument(owner, *linked)) {
        return true;
    }
    const auto* value = link->LinkedObject.getValue();
    return value && externalDocument(owner, *value);
}

bool operandShapeCache(const App::DocumentObject& operand)
{
    for (const auto& name : operand.getDynamicPropertyNames()) {
        const auto* property = operand.getDynamicPropertyByName(name.c_str());
        if (isShapeCacheProperty(name, property)) {
            return true;
        }
    }
    return false;
}

void collectOperand(const App::DocumentObject& owner,
                    const App::DocumentObject* operand,
                    AdmissionDecision& decision,
                    const QualificationProbe& probe)
{
    if (!operand) {
        return;
    }
    if (operandNotFeaturePredicate(*operand)) {
        addReason(decision, AdmissionRefusal::OperandNotFeature, probe);
    }
    if (operandLinkPredicate(*operand)) {
        addReason(decision, AdmissionRefusal::OperandLink, probe);
    }
    if (operandGroupPredicate(*operand)) {
        addReason(decision, AdmissionRefusal::OperandGroup, probe);
    }
    if (operandExternalPredicate(owner, *operand)) {
        addReason(decision, AdmissionRefusal::OperandExternal, probe);
    }
    if (operandScaledPredicate(*operand)) {
        addReason(decision, AdmissionRefusal::OperandScaledTransform, probe);
    }
    if (operandPythonPredicate(*operand)) {
        addReason(decision, AdmissionRefusal::OperandPython, probe);
    }
    if (operandShapeCachePredicate(*operand)) {
        addReason(decision, AdmissionRefusal::OperandShapeCache, probe);
    }
    if (operandExtensionPredicate(*operand)) {
        addReason(decision, AdmissionRefusal::OperandExtension, probe);
    }
    if (operandAttachmentPredicate(*operand)) {
        addReason(decision, AdmissionRefusal::OperandAttachment, probe);
    }
    if (operandSourceClosurePredicate(owner, *operand)) {
        addReason(decision, AdmissionRefusal::OperandSourceClosure, probe);
    }
}

void collectBooleanOperands(const Part::Boolean& boolean,
                            AdmissionDecision& decision,
                            const QualificationProbe& probe)
{
    const auto* base = boolean.Base.getValue();
    const auto* tool = boolean.Tool.getValue();
    if (operandMissingPredicate(base, tool)) {
        addReason(decision, AdmissionRefusal::OperandMissing, probe);
    }
    collectOperand(boolean, base, decision, probe);
    collectOperand(boolean, tool, decision, probe);
    if (refineEnabledPredicate(boolean)) {
        addReason(decision, AdmissionRefusal::RefineEnabled, probe);
    }
    if (historyWrittenPredicate(boolean)) {
        addReason(decision, AdmissionRefusal::HistoryWritten, probe);
    }
}

}  // namespace

bool AdmissionDecision::has(AdmissionRefusal reason) const
{
    return std::find(reasons.begin(), reasons.end(), reason) != reasons.end();
}

const char* admissionCitation(AdmissionRefusal reason)
{
    switch (reason) {
        case AdmissionRefusal::NotExactType:
            return "doc/parallel-recompute-m0.md §2: admission is exact Part::Box";
        case AdmissionRefusal::PythonFeature:
            return "doc/parallel-recompute-m0.md §2 shared effects: Python observers stay serial";
        case AdmissionRefusal::Link:
            return "doc/parallel-recompute-m0.md §2 Boolean: no Links/groups/scaling/external "
                   "references";
        case AdmissionRefusal::Group:
            return "doc/parallel-recompute-m0.md §2 Boolean: no Links/groups/scaling/external "
                   "references";
        case AdmissionRefusal::ScalingOrPlacementTransform:
            return "doc/parallel-recompute-m0.md §2 Boolean: no scaling/placement-transform "
                   "resolution; §2 Box: parents requiring transform resolution are ineligible";
        case AdmissionRefusal::ExternalReference:
            return "doc/parallel-recompute-m0.md §2 Boolean: no external references";
        case AdmissionRefusal::ExpressionBinding:
            return "doc/parallel-recompute-m0.md §2 Box: expression bindings are ineligible";
        case AdmissionRefusal::ExpressionReadsOtherObject:
            return "doc/parallel-recompute-m0.md §2 Box: expression bindings are ineligible; "
                   "§2 recompute envelope keeps expressions serial";
        case AdmissionRefusal::PreferenceReadDuringExecute:
            return "doc/parallel-recompute-m0.md §2 Boolean: CheckModel is read through "
                   "App::GetApplication() during execute";
        case AdmissionRefusal::DynamicShapeCache:
            return "doc/parallel-recompute-m0.md §2 Boolean: shape resolution can add a dynamic "
                   "_ShapeCache property";
        case AdmissionRefusal::DynamicProperty:
            return "doc/parallel-recompute-m0.md §2 Box: dynamic properties are ineligible";
        case AdmissionRefusal::ColdOrRestore:
            return "doc/parallel-recompute-m0.md §2 Box: restored Shape User1; §2 shared: reject "
                   "partial restore";
        case AdmissionRefusal::ActiveAttachment:
            return "doc/parallel-recompute-m0.md §2 Box: reject active, pending or base attachment";
        case AdmissionRefusal::IncomingElementReference:
            return "doc/parallel-recompute-m0.md §2 Box: nonempty incoming element references are "
                   "outside the narrow publication";
        case AdmissionRefusal::ExtensionSchema:
            return "doc/parallel-recompute-m0.md §2 Box lines 85 and 89: exact extension schema; "
                   "base execute runs extensions";
        case AdmissionRefusal::UnsettledElementMapVersion:
            return "doc/parallel-recompute-m0.md §2 Box lines 87 and 112-113: require a settled "
                   "_ElementMapVersion";
        case AdmissionRefusal::MutatingObserver:
            return "doc/parallel-recompute-m0.md §2 line 174: mutating observers stay on the owner";
        case AdmissionRefusal::FreezeOrSkip:
            return "doc/parallel-recompute-m0.md §2 lines 199-206: freeze and skip state refuse";
        case AdmissionRefusal::OperandMissing:
            return "doc/parallel-recompute-m0.md §2 Boolean: Base and Tool are required resolved operands";
        case AdmissionRefusal::OperandLink:
            return "doc/parallel-recompute-m0.md §2 Boolean: no Link operands; capture direct Part features";
        case AdmissionRefusal::OperandGroup:
            return "doc/parallel-recompute-m0.md §2 Boolean: no group operands";
        case AdmissionRefusal::OperandExternal:
            return "doc/parallel-recompute-m0.md §2 Boolean: no external-document operands";
        case AdmissionRefusal::OperandScaledTransform:
            return "doc/parallel-recompute-m0.md §2 Boolean: no scaled operand transforms";
        case AdmissionRefusal::OperandPython:
            return "doc/parallel-recompute-m0.md §2 Boolean: shape retrieval can invoke Python; Python operands stay serial";
        case AdmissionRefusal::OperandShapeCache:
            return "doc/parallel-recompute-m0.md §2 Boolean: operand resolution can add _ShapeCache; the property in this tree is _Part_ShapeCache";
        case AdmissionRefusal::RefineEnabled:
            return "doc/parallel-recompute-m0.md §2 Boolean: Refine=true stays excluded";
        case AdmissionRefusal::HistoryWritten:
            return "doc/parallel-recompute-m0.md §2 Boolean: History is not assigned by binary execute and must stay unchanged";
        case AdmissionRefusal::OperandNotFeature:
            return "doc/parallel-recompute-m0.md §2 line 136: operands start as direct native Part features";
        case AdmissionRefusal::OperandExtension:
            return "doc/parallel-recompute-m0.md §2 line 156: an unknown operand extension forces serial";
        case AdmissionRefusal::OperandAttachment:
            return "doc/parallel-recompute-m0.md §2 line 156: operand attachment forces serial";
        case AdmissionRefusal::OperandSourceClosure:
            return "doc/parallel-recompute-m0.md §2 lines 136, 139 and 156: the operand source closure must stay native, same-document and cache-free";
    }
    return "doc/parallel-recompute-m0.md §2";
}

AdmissionDecision qualifyForDetachedExecution(const App::DocumentObject& object, QualificationProbe probe)
{
    const auto kind = booleanKind(object);
    const bool isBoolean = kind != BooleanKind::None;
    const auto typeName = object.getTypeId().getName();
    const auto& contract = isBoolean ? narrowBooleanQualification(typeName) : narrowBoxQualification();
    AdmissionDecision decision;
    decision.liveRecompute = contract.liveRecompute;
    decision.proposedKernel = contract.proposedKernel;
    decision.pending.assign(contract.pending, contract.pending + contract.pendingCount);

    const bool exactBox = !isBoolean && sameText(typeName, "Part::Box")
        && object.getTypeId() == Part::Box::getClassTypeId();
    if (!exactBox && !isBoolean) {
        addReason(decision, AdmissionRefusal::NotExactType, probe);
    }
    if (pythonFeature(object)) {
        addReason(decision, AdmissionRefusal::PythonFeature, probe);
    }
    // CheckModel is a frozen owner input on the Boolean contract. It is still a
    // refusal for every other type, including the Box kernel.
    if (!isBoolean && preferenceReadDuringExecute(typeName, narrowBoxQualification())) {
        addReason(decision, AdmissionRefusal::PreferenceReadDuringExecute, probe);
    }
    if (coldOrRestore(object)) {
        addReason(decision, AdmissionRefusal::ColdOrRestore, probe);
    }
    if (freezeOrSkip(object)) {
        addReason(decision, AdmissionRefusal::FreezeOrSkip, probe);
    }
    if (activeAttachment(object)) {
        addReason(decision, AdmissionRefusal::ActiveAttachment, probe);
    }
    if (grouped(object)) {
        addReason(decision, AdmissionRefusal::Group, probe);
    }
    if (linkScale(object) || parentTransform(object)) {
        addReason(decision, AdmissionRefusal::ScalingOrPlacementTransform, probe);
    }
    const auto* boolean = isBoolean ? dynamic_cast<const Part::Boolean*>(&object) : nullptr;
    if (boolean) {
        collectLinkAndExternal(object, decision, probe, &boolean->Base, &boolean->Tool);
        collectBooleanOperands(*boolean, decision, probe);
    }
    else {
        collectLinkAndExternal(object, decision, probe);
    }
    collectExpressions(object, decision, probe);
    collectDynamicProperties(object, decision, probe);

    const auto& references = App::PropertyLinkBase::getElementReferences(
        const_cast<App::DocumentObject*>(&object));
    if (!references.empty()) {
        addReason(decision, AdmissionRefusal::IncomingElementReference, probe);
    }
    if (exactBox && !exactExtensionSchema(object)) {
        addReason(decision, AdmissionRefusal::ExtensionSchema, probe);
    }
    if (isBoolean && !noExtensions(object)) {
        addReason(decision, AdmissionRefusal::ExtensionSchema, probe);
    }
    if ((exactBox || isBoolean) && unsettledElementMapVersion(object)) {
        addReason(decision, AdmissionRefusal::UnsettledElementMapVersion, probe);
    }
    if (mutatingObserver(object)) {
        addReason(decision, AdmissionRefusal::MutatingObserver, probe);
    }

    decision.admitted = decision.reasons.empty();
    // Pending audit rows are listed above and are not a silent pass.
    decision.detached = decision.admitted && decision.pending.empty();
    return decision;
}

}  // namespace PartQualification
