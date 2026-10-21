// SPDX-License-Identifier: LGPL-2.1-or-later
// The property-level replacement protocol used by Std_LinkReplace, without a GUI.
#include <algorithm>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectPy.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyExpressionEngine.h>
#include <App/Expression.h>

#include "ExtractReferences.h"
#include "ExtractSketchReferences.h"
#include "ExtractExpressionReferences.h"
#include "ExtractXLinkReferences.h"

namespace Assembly
{
namespace
{
using Replacement = ReferenceReplacement;
using Reference = std::pair<App::DocumentObject*, std::string>;

struct Report
{
    std::vector<Reference> redirected;
    std::vector<Reference> leftover;
    std::vector<App::DocumentObject*> referenced;
    std::vector<App::DocumentObject*> shapes;
};

class CopyOwnerScope
{
public:
    CopyOwnerScope(App::Property* property, App::DocumentObject* owner) : property(property)
    {
        if (property) {
            property->setContainer(owner);
        }
    }
    ~CopyOwnerScope()
    {
        if (property) {
            // Detached copies never registered backlinks. Their destructors
            // must not remove the real property's backlinks from the model.
            property->setContainer(nullptr);
        }
    }

private:
    App::Property* property;
};

App::DocumentObject* object(PyObject* value)
{
    if (!PyObject_TypeCheck(value, &App::DocumentObjectPy::Type)) {
        throw Base::TypeError("Expected a document object");
    }
    return static_cast<App::DocumentObjectPy*>(value)->getDocumentObjectPtr();
}

Report replaceReferences(const std::vector<App::DocumentObject*>& owners,
                         const std::vector<Replacement>& replacements,
                         bool apply)
{
    // Property notifications can invoke Python proxies on the document owner.
    Base::PyGILStateLocker lock;
    Report report;
    using Change = std::pair<App::Property*, std::unique_ptr<App::Property>>;
    std::vector<Change> changes;
    struct SketchAttachment
    {
        App::Property* support;
        std::vector<Change> snapshot;
    };
    std::vector<SketchAttachment> attachments;
    for (auto owner : owners) {
        const auto& parents = owner->getInList();
        const bool jointOwner =
            owner->isDerivedFrom(Base::Type::fromName("App::FeaturePython")) &&
            std::any_of(parents.begin(), parents.end(), [](auto parent) {
                return parent->isDerivedFrom(Base::Type::fromName("Assembly::JointGroup"));
            });
        std::vector<App::Property*> properties;
        owner->getPropertyList(properties);
        for (auto prop : properties) {
            if (prop->getContainer() != owner) {
                continue;
            }
            auto link = dynamic_cast<App::PropertyLinkBase*>(prop);
            if (!link) {
                continue;
            }
            std::unique_ptr<App::Property> copy;
            bool unsupported = false;
            std::vector<App::DocumentObject*> targets;
            link->getLinks(targets, true);
            const std::string propertyName(prop->getName());
            const bool needsFeature =
                propertyName == "AttachmentSupport" ||
                (owner->getTypeId() == Base::Type::fromName("PartDesign::ShapeBinder") &&
                 propertyName == "Support") ||
                (owner->isDerivedFrom(Base::Type::fromName("Sketcher::SketchObject")) &&
                 propertyName == "ExternalGeometry");
            auto trace = dynamic_cast<App::PropertyBool*>(owner->getPropertyByName("TraceSupport"));
            auto relative = dynamic_cast<App::PropertyBool*>(owner->getPropertyByName("Relative"));
            const bool globalGeometry =
                propertyName == "AttachmentSupport" ||
                (propertyName == "Support" &&
                 ((trace && trace->getValue()) || (relative && relative->getValue())));
            if (auto expressions = dynamic_cast<App::PropertyExpressionContainer*>(prop)) {
                copy =
                    prepareExpressionReferences(*owner, *expressions, replacements, report.shapes);
                for (const auto& replacement : replacements) {
                    if (std::find(targets.begin(), targets.end(), replacement.oldObject) !=
                        targets.end()) {
                        report.referenced.push_back(replacement.oldObject);
                    }
                }
            } else if (auto sublinks = dynamic_cast<App::PropertyXLinkSubList*>(prop)) {
                copy = prepareXLinkReferences(*sublinks, replacements, report.referenced);
                for (const auto& replacement : replacements) {
                    if (std::find(targets.begin(), targets.end(), replacement.oldObject) !=
                        targets.end()) {
                        report.referenced.push_back(replacement.oldObject);
                    }
                }
            } else {
                for (const auto& replacement : replacements) {
                    auto oldObj = replacement.oldObject;
                    auto newObj = replacement.link;
                    if (propertyName == "AttachmentSupport" && replacement.attachment) {
                        newObj = replacement.attachment;
                    } else if (needsFeature && globalGeometry && replacement.globalGeometry) {
                        newObj = replacement.globalGeometry;
                    } else if (needsFeature && replacement.geometry) {
                        newObj = replacement.geometry;
                    }
                    auto current = copy ? dynamic_cast<App::PropertyLinkBase*>(copy.get()) : link;
                    const bool direct =
                        std::find(targets.begin(), targets.end(), oldObj) != targets.end();
                    std::unique_ptr<App::Property> next;
                    {
                        CopyOwnerScope guard(copy.get(), owner);
                        next.reset(current->CopyOnLinkReplace(owner, oldObj, newObj));
                        if (!next && !direct) {
                            // Ancestor-rooted subnames need the replaced object's parent.
                            for (auto parent : oldObj->getInList()) {
                                next.reset(current->CopyOnLinkReplace(parent, oldObj, newObj));
                                if (next) {
                                    break;
                                }
                            }
                        }
                    }
                    const bool replaced = bool(next);
                    if (direct) {
                        report.referenced.push_back(oldObj);
                        unsupported |= !next && !copy;
                    }
                    if (next) {
                        report.referenced.push_back(oldObj);
                        copy = std::move(next);
                    }
                    // CopyOnLinkReplace validates property storage, not the
                    // consumer's required target type. In preflight old/new
                    // are identical, but extraction will replace the target
                    // with an App::Link. Only consumers with an explicit link
                    // or geometry adapter contract can accept that change.
                    // For example ProfileBased requires a Part::Feature for
                    // Profile, and Body history requires native features.
                    // Ancestor-rooted paths only rewrite subnames: the
                    // property's target object keeps its native type.
                    const bool jointReference = jointOwner &&
                        (propertyName == "Reference1" || propertyName == "Reference2" ||
                         propertyName == "ObjectToGround");
                    const bool acceptsReplacement = !direct || needsFeature || jointReference ||
                        propertyName == "Group" ||
                        oldObj->isDerivedFrom(Base::Type::fromName("App::Link")) ||
                        (owner->isDerivedFrom(Base::Type::fromName("App::Link")) &&
                         propertyName == "LinkedObject");
                    if ((direct || replaced) && !acceptsReplacement) {
                        unsupported = true;
                    }
                }
            }
            if (copy) {
                std::vector<App::DocumentObject*> remaining;
                if (auto expressions =
                        dynamic_cast<App::PropertyExpressionContainer*>(copy.get())) {
                    // Detached expression copies have no dependency cache yet.
                    // Inspect their expressions rather than their cached links.
                    for (const auto& [path, expression] : expressions->getExpressions()) {
                        for (const auto& [target, hidden] : expression->getDepObjects()) {
                            remaining.push_back(target);
                        }
                    }
                } else {
                    static_cast<App::PropertyLinkBase*>(copy.get())->getLinks(remaining, true);
                }
                for (const auto& replacement : replacements) {
                    if (replacement.oldObject != replacement.link &&
                        std::find(remaining.begin(), remaining.end(), replacement.oldObject) !=
                            remaining.end()) {
                        unsupported = true;
                    }
                }
            }
            std::unique_ptr<App::Property> sketchGeometry;
            if (copy && propertyName == "ExternalGeometry") {
                if (auto external = dynamic_cast<App::PropertyLinkSubList*>(prop)) {
                    bool supported = true;
                    sketchGeometry = prepareSketchReferenceIndices(*owner, *external, supported);
                    unsupported |= !supported;
                }
            }
            if (unsupported) {
                report.leftover.emplace_back(owner, prop->getName());
            } else if (copy) {
                report.redirected.emplace_back(owner, prop->getName());
                if (sketchGeometry) {
                    auto geometry = owner->getPropertyByName("ExternalGeo");
                    changes.emplace_back(geometry, std::move(sketchGeometry));
                    report.redirected.emplace_back(owner, "ExternalGeo");
                }
                if (propertyName == "AttachmentSupport" &&
                    owner->isDerivedFrom(Base::Type::fromName("Sketcher::SketchObject"))) {
                    SketchAttachment state{prop, {}};
                    for (const char* name :
                         {"ExternalGeo", "ExternalGeometry", "Geometry", "Constraints"}) {
                        if (auto value = owner->getPropertyByName(name)) {
                            state.snapshot.emplace_back(
                                value, std::unique_ptr<App::Property>(value->Copy()));
                        }
                    }
                    attachments.push_back(std::move(state));
                }
                changes.emplace_back(prop, std::move(copy));
            }
        }
    }
    if (apply && report.leftover.empty()) {
        std::set<App::Property*> applied;
        for (auto& attachment : attachments) {
            // Sketch support changes clear external geometry and its constraints.
            // Restore the original sketch data before applying the prepared
            // external-reference replacements, keeping its geometry IDs intact.
            for (auto& [prop, copy] : changes) {
                if (prop == attachment.support) {
                    prop->Paste(*copy);
                    applied.insert(prop);
                    break;
                }
            }
            for (auto& [prop, copy] : attachment.snapshot) {
                prop->Paste(*copy);
            }
        }
        for (auto& [prop, copy] : changes) {
            if (!applied.contains(prop)) {
                prop->Paste(*copy);
            }
        }
    }
    return report;
}

Py::List references(const std::vector<Reference>& entries)
{
    Py::List list;
    for (const auto& [owner, name] : entries) {
        Py::Tuple entry(2);
        entry[0] = Py::Object(owner->getPyObject(), true);
        entry[1] = Py::String(name);
        list.append(entry);
    }
    return list;
}
} // namespace

PyObject* extractionReferences(PyObject* args)
{
    PyObject *ownersArg, *pairsArg;
    int apply = 0;
    if (!PyArg_ParseTuple(args, "OO|p", &ownersArg, &pairsArg, &apply)) {
        return nullptr;
    }
    try {
        Py::Sequence ownersSequence(ownersArg), pairs(pairsArg);
        std::vector<App::DocumentObject*> owners;
        std::vector<Replacement> replacements;
        App::Document* document = nullptr;
        for (Py_ssize_t i = 0; i < ownersSequence.size(); ++i) {
            auto owner = object(ownersSequence[i].ptr());
            if (document && document != owner->getDocument()) {
                throw Base::ValueError("Reference owners must belong to one document");
            }
            document = owner->getDocument();
            owners.push_back(owner);
        }
        for (Py_ssize_t i = 0; i < pairs.size(); ++i) {
            Py::Sequence pair(pairs[i]);
            replacements.push_back({object(pair[0].ptr()),
                                    object(pair[1].ptr()),
                                    pair.size() > 2 ? object(pair[2].ptr()) : nullptr,
                                    pair.size() > 3 ? object(pair[3].ptr()) : nullptr,
                                    pair.size() > 4 && PyObject_IsTrue(pair[4].ptr()) == 1,
                                    pair.size() > 5 ? object(pair[5].ptr()) : nullptr,
                                    pair.size() > 6 ? object(pair[6].ptr()) : nullptr,
                                    pair.size() > 7 ? object(pair[7].ptr()) : nullptr});
        }
        const auto operation = [&] { return replaceReferences(owners, replacements, apply); };
        auto lane = document ? document->executionLane() : nullptr;
        auto report = lane ? lane->dispatchToOwner(operation) : operation();
        Py::List referenced;
        for (auto target : report.referenced) {
            referenced.append(Py::Object(target->getPyObject(), true));
        }
        Py::Dict result;
        result["redirected"] = references(report.redirected);
        result["leftover"] = references(report.leftover);
        result["referenced"] = referenced;
        Py::List shapes;
        for (auto target : report.shapes) {
            shapes.append(Py::Object(target->getPyObject(), true));
        }
        result["shapes"] = shapes;
        return Py::new_reference_to(result);
    } catch (const Base::Exception& e) {
        e.setPyException();
    } catch (const Py::Exception&) {
        // Preserve the Python exception.
    }
    return nullptr;
}
} // namespace Assembly
