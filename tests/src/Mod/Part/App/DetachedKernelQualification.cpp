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
#include <Mod/Part/App/FeaturePartBox.h>

#include <algorithm>
#include <cmath>
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

void addReason(AdmissionDecision& decision, AdmissionRefusal reason)
{
    if (decision.has(reason)) {
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

void collectLinkAndExternal(const App::DocumentObject& object, AdmissionDecision& decision)
{
    if (object.isLink()) {
        addReason(decision, AdmissionRefusal::Link);
    }
    std::vector<App::Property*> properties;
    object.getPropertyList(properties);
    for (const auto* property : properties) {
        if (!property || property == &object.ExpressionEngine) {
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
                addReason(decision, AdmissionRefusal::ExternalReference);
            }
            else if (other != &object) {
                addReason(decision, AdmissionRefusal::Link);
            }
        }
    }
    for (const auto* other : object.getOutList()) {
        if (other && externalDocument(object, *other)) {
            addReason(decision, AdmissionRefusal::ExternalReference);
        }
    }
}

void collectExpressions(const App::DocumentObject& object, AdmissionDecision& decision)
{
    for (const auto& entry : object.ExpressionEngine.getExpressions()) {
        const auto* expression = entry.second;
        if (!expression) {
            continue;
        }
        addReason(decision, AdmissionRefusal::ExpressionBinding);
        for (const auto& dep : expression->getDepObjects()) {
            const auto* other = dep.first;
            if (!other || other == &object) {
                continue;
            }
            addReason(decision, AdmissionRefusal::ExpressionReadsOtherObject);
            if (externalDocument(object, *other)) {
                addReason(decision, AdmissionRefusal::ExternalReference);
            }
        }
    }
}

void collectDynamicProperties(const App::DocumentObject& object, AdmissionDecision& decision)
{
    for (const auto& name : object.getDynamicPropertyNames()) {
        const auto* property = object.getDynamicPropertyByName(name.c_str());
        if (isShapeCacheProperty(name, property)) {
            addReason(decision, AdmissionRefusal::DynamicShapeCache);
        }
        else {
            addReason(decision, AdmissionRefusal::DynamicProperty);
        }
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
    }
    return "doc/parallel-recompute-m0.md §2";
}

AdmissionDecision qualifyForDetachedExecution(const App::DocumentObject& object)
{
    const auto& contract = narrowBoxQualification();
    AdmissionDecision decision;
    decision.liveRecompute = contract.liveRecompute;
    decision.proposedKernel = contract.proposedKernel;
    decision.pending.assign(contract.pending, contract.pending + contract.pendingCount);

    const auto typeName = object.getTypeId().getName();
    const bool exactBox = sameText(typeName, contract.exactType)
        && object.getTypeId() == Part::Box::getClassTypeId();
    if (!exactBox) {
        addReason(decision, AdmissionRefusal::NotExactType);
    }
    if (pythonFeature(object)) {
        addReason(decision, AdmissionRefusal::PythonFeature);
    }
    if (preferenceReadDuringExecute(typeName, contract)) {
        addReason(decision, AdmissionRefusal::PreferenceReadDuringExecute);
    }
    if (coldOrRestore(object)) {
        addReason(decision, AdmissionRefusal::ColdOrRestore);
    }
    if (freezeOrSkip(object)) {
        addReason(decision, AdmissionRefusal::FreezeOrSkip);
    }
    if (activeAttachment(object)) {
        addReason(decision, AdmissionRefusal::ActiveAttachment);
    }
    if (grouped(object)) {
        addReason(decision, AdmissionRefusal::Group);
    }
    if (linkScale(object) || parentTransform(object)) {
        addReason(decision, AdmissionRefusal::ScalingOrPlacementTransform);
    }
    collectLinkAndExternal(object, decision);
    collectExpressions(object, decision);
    collectDynamicProperties(object, decision);

    const auto& references = App::PropertyLinkBase::getElementReferences(
        const_cast<App::DocumentObject*>(&object));
    if (!references.empty()) {
        addReason(decision, AdmissionRefusal::IncomingElementReference);
    }
    if (exactBox && !exactExtensionSchema(object)) {
        addReason(decision, AdmissionRefusal::ExtensionSchema);
    }
    if (exactBox && unsettledElementMapVersion(object)) {
        addReason(decision, AdmissionRefusal::UnsettledElementMapVersion);
    }
    if (mutatingObserver(object)) {
        addReason(decision, AdmissionRefusal::MutatingObserver);
    }

    decision.admitted = decision.reasons.empty();
    // Pending audit rows are listed above and are not a silent pass.
    decision.detached = decision.admitted && decision.pending.empty();
    return decision;
}

}  // namespace PartQualification
