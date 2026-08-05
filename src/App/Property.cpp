// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2002 Jürgen Riegel <juergen.riegel@web.de>              *
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

#include <cassert>
#include <array>
#include <tuple>

#include <atomic>
#include <Base/Tools.h>
#include <Base/Writer.h>
#include <CXX/Objects.hxx>

#include "Property.h"
#include "Document.h"
#include "private/CollaborationStructuralMutationRecorder.h"
#include "ObjectIdentifier.h"
#include "PropertyContainer.h"
#include "DocumentMutationAuthority.h"
#include "DocumentObject.h"
#include "MutationClassification.h"
#include "PropertyLinks.h"
#include "PropertyStandard.h"

using namespace App;

namespace
{

void publishPropertyMutation(Property& property, bool structural)
{
    auto* container = property.getContainer();
    auto* document = documentFromPropertyContainer(container);
    if (!document) {
        return;
    }

    MutationClassificationInput input;
    input.source = structural ? CollaborationMutationSource::PropertyStatus
                              : CollaborationMutationSource::PropertyValue;
    input.mutationKind = structural ? MutationKind::StructuralProperty
                                    : MutationKind::PropertyWrite;
    input.propertyName = property.getName();
    input.propertyFamily = CollaborationPropertyFamily::NotApplicable;
    if (!structural) {
        if (property.isDerivedFrom(PropertyLinkBase::getClassTypeId())) {
            input.propertyFamily = CollaborationPropertyFamily::Link;
        }
        else {
            const auto type = property.getTypeId();
            const bool exactModelValue = type == PropertyBool::getClassTypeId()
                || type == PropertyInteger::getClassTypeId()
                || type == PropertyFloat::getClassTypeId()
                || type == PropertyString::getClassTypeId();
            input.propertyFamily = exactModelValue ? CollaborationPropertyFamily::ModelValue
                                                   : CollaborationPropertyFamily::Unknown;
        }
    }

    if (const auto* object = dynamic_cast<const DocumentObject*>(container)) {
        // Undo retains removed objects with their former Document pointer so
        // they can be restored.  Mutating such detached storage is not a live
        // document mutation and has neither a document name nor a revision
        // identity to publish.
        if (!object->isAttachedToDocument() || object->getDocument() != document
            || !document->containsObject(object)) {
            return;
        }
        input.containerKind = CollaborationContainerKind::DocumentObject;
        input.objectName = object->getNameInDocument();
        input.stableObjectIdentity = document->collaborationObjectIdentity(*object);
    }
    else if (dynamic_cast<const Document*>(container)) {
        input.containerKind = CollaborationContainerKind::Document;
    }
    const auto effects = classifyMutation(input);
    document->recordCollaborationAtomicPresentationEffects(effects, &property);
    if (structural || input.propertyFamily == CollaborationPropertyFamily::Link) {
        Internal::CollaborationStructuralMutationRecorder::record(*document, effects);
    }
    if (document->collaborationRevisionPublicationSuppressed(&property)) {
        return;
    }
    static_cast<void>(document->collaborationRevisions().publish(effects));
}

}  // namespace


//**************************************************************************
//**************************************************************************
// Property
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE_ABSTRACT(App::Property, Base::Persistence)

//**************************************************************************
// Construction/Destruction

static std::atomic<int64_t> _PropID;

// Here is the implementation! Description should take place in the header file!
Property::Property()
    : _id(++_PropID)
{}

Property::~Property() = default;

const char* Property::getName() const
{
    return myName ? myName : "";
}

bool Property::hasName() const
{
    return isValidName(myName);
}

bool Property::isValidName(const char* name)
{
    return !Base::Tools::isNullOrEmpty(name);
}

std::string Property::getFullName() const
{
    std::string name;
    if (myName) {
        if (father) {
            name = father->getFullName() + ".";
        }
        else {
            name = "?.";
        }
        name += myName;
    }
    else {
        return "?";
    }
    return name;
}

std::string Property::getFileName(const char* postfix, const char* prefix) const
{
    std::ostringstream ss;
    if (prefix) {
        ss << prefix;
    }
    if (!myName) {
        ss << "Property";
    }
    else {
        std::string name = getFullName();
        auto pos = name.find('#');
        if (pos == std::string::npos) {
            ss << name;
        }
        else {
            ss << (name.c_str() + pos + 1);
        }
    }
    if (postfix) {
        ss << postfix;
    }
    return ss.str();
}

// clang-format off
static constexpr auto mapProps = std::to_array<std::tuple<Property::Status, PropertyType>>({
    {App::Property::PropReadOnly,    Prop_ReadOnly},
    {App::Property::PropHidden,      Prop_Hidden},
    {App::Property::PropInput,       Prop_Input},
    {App::Property::PropOutput,      Prop_Output},
    {App::Property::PropTransient,   Prop_Transient},
    {App::Property::PropNoRecompute, Prop_NoRecompute},
    {App::Property::PropNoPersist,   Prop_NoPersist}
});
// clang-format on

short Property::getType() const
{
    short type = 0;
    for (const auto& [propertyStatus, propertyType] : mapProps) {
        if (testStatus(propertyStatus)) {
            type |= propertyType;
        }
    }
    return type;
}

void Property::syncType(unsigned type)
{
    for (const auto& [propertyStatus, propertyType] : mapProps) {
        if (type & propertyType) {
            StatusBits.set((size_t)propertyStatus);
        }
    }
}

const char* Property::getGroup() const
{
    return father->getPropertyGroup(this);
}

const char* Property::getDocumentation() const
{
    return father->getPropertyDocumentation(this);
}

void Property::setContainer(PropertyContainer* father)
{
    this->father = father;
}

void Property::setPathValue(const ObjectIdentifier& path, const boost::any& value)
{
    path.setValue(value);
}

const boost::any Property::getPathValue(const ObjectIdentifier& path) const
{
    return path.getValue();
}

void Property::getPaths(std::vector<ObjectIdentifier>& paths) const
{
    paths.emplace_back(getContainer(), getName());
}

ObjectIdentifier Property::canonicalPath(const ObjectIdentifier& p) const
{
    return p;
}

namespace App
{
/*!
 * \brief The PropertyCleaner struct
 * Make deleting dynamic property safer by postponing its destruction.
 *
 * Dynamic property can be removed at any time, even during triggering of
 * onChanged() signal of the removing property. This patch introduced
 * static function Property::destroy() to make it safer by queueing any
 * removed property, and only deleting them when no onChanged() call is
 * active.
 */
struct PropertyCleaner
{
    explicit PropertyCleaner(Property* p)
        : prop(p)
    {
        ++_PropCleanerCounter;
    }
    ~PropertyCleaner()
    {
        if (--_PropCleanerCounter) {
            return;
        }
        bool found = false;
        while (!_RemovedProps.empty()) {
            auto p = _RemovedProps.back();
            _RemovedProps.pop_back();
            if (p != prop) {
                p->setContainer(nullptr);
                delete p;
            }
            else {
                found = true;
            }
        }

        if (found) {
            _RemovedProps.push_back(prop);
        }
    }
    static void add(Property* prop)
    {
        _RemovedProps.push_back(prop);
    }

    Property* prop;

    static std::vector<Property*> _RemovedProps;
    static int _PropCleanerCounter;
};
}  // namespace App

std::vector<Property*> PropertyCleaner::_RemovedProps;
int PropertyCleaner::_PropCleanerCounter = 0;

void Property::destroy(Property* p)
{
    if (p) {
        // Is it necessary to nullify the container? May cause crash if any
        // onChanged() caller assumes a non-null container.
        //
        // p->setContainer(0);

        PropertyCleaner::add(p);
    }
}

bool Property::enableNotify(bool enable)
{
    bool isNotify = isNotifyEnabled();

    if (enable) {
        StatusBits.reset(DisableNotify);
    }
    else {
        StatusBits.set(DisableNotify);
    }
    return isNotify;
}

bool Property::isNotifyEnabled() const
{
    return !StatusBits.test(DisableNotify);
}

void Property::touch()
{
    PropertyCleaner guard(this);
    if (father) {
        if (Document* doc = documentFromPropertyContainer(father)) {
            const char* objectName = nullptr;
            if (const auto* obj = dynamic_cast<const DocumentObject*>(father)) {
                objectName = obj->getNameInDocument();
            }
            enforceDocumentMutation(doc,
                                    MutationKind::PropertyWrite,
                                    MutationOrigin::Cpp,
                                    objectName,
                                    getName());
        }
    }
    StatusBits.set(Touched);
    publishPropertyMutation(*this, false);
    if (father && isNotifyEnabled()) {
        father->onEarlyChange(this);
        father->onChanged(this);
    }
}

void Property::purgeTouched()
{
    if (!isTouched()) {
        return;
    }
    if (father) {
        if (Document* doc = documentFromPropertyContainer(father)) {
            const char* objectName = nullptr;
            if (const auto* obj = dynamic_cast<const DocumentObject*>(father)) {
                objectName = obj->getNameInDocument();
            }
            enforceDocumentMutation(doc,
                                    MutationKind::PropertyWrite,
                                    MutationOrigin::Cpp,
                                    objectName,
                                    getName());
        }
    }
    StatusBits.reset(Touched);
    publishPropertyMutation(*this, false);
}

void Property::setReadOnly(bool readOnly)
{
    this->setStatus(App::Property::ReadOnly, readOnly);
}

void Property::hasSetValue()
{
    PropertyCleaner guard(this);
    publishPropertyMutation(*this, false);
    if (father) {
        if (isNotifyEnabled()) {
            father->onChanged(this);
        }
        if (!testStatus(Busy)) {
            Base::BitsetLocker<decltype(StatusBits)> guard(StatusBits, Busy);
            if (Document* doc = documentFromPropertyContainer(father)) {
                doc->emitCollaborationPropertyChanged(*this);
            }
            else {
                signalChanged(*this);
            }
        }
    }
    StatusBits.set(Touched);
}

void Property::aboutToSetValue()
{
    if (father) {
        if (Document* doc = documentFromPropertyContainer(father)) {
            const char* objectName = nullptr;
            if (const auto* obj = dynamic_cast<const DocumentObject*>(father)) {
                objectName = obj->getNameInDocument();
            }
            enforceDocumentMutation(doc,
                                    MutationKind::PropertyWrite,
                                    MutationOrigin::Cpp,
                                    objectName,
                                    getName());
        }
        father->onBeforeChange(this);
    }
}

void Property::verifyPath(const ObjectIdentifier& p) const
{
    p.verify(*this);
}

Property* Property::Copy() const
{
    // have to be reimplemented by a subclass!
    assert(0);
    return nullptr;
}

void Property::Paste(const Property& /*from*/)
{
    // have to be reimplemented by a subclass!
    assert(0);
}

void Property::setStatusValue(unsigned long status)
{
    // clang-format off
    static const unsigned long mask =
         (1<<PropDynamic)
        |(1<<PropNoRecompute)
        |(1<<PropReadOnly)
        |(1<<PropTransient)
        |(1<<PropOutput)
        |(1<<PropHidden)
        |(1<<PropNoPersist)
        |(1<<Busy);
    // clang-format on

    status &= ~mask;
    status |= StatusBits.to_ulong() & mask;
    unsigned long oldStatus = StatusBits.to_ulong();
    if (status != oldStatus && father) {
        if (auto* document = documentFromPropertyContainer(father)) {
            const auto* object = dynamic_cast<const DocumentObject*>(father);
            enforceDocumentMutation(document,
                                    MutationKind::StructuralProperty,
                                    MutationOrigin::Cpp,
                                    object ? object->getNameInDocument() : nullptr,
                                    getName());
            Internal::CollaborationStructuralMutationRecorder::
                ensurePropertySchemaMutationAllowed(*document, *father);
        }
    }
    StatusBits = decltype(StatusBits)(status);

    if (father) {
        if (status != oldStatus) {
            publishPropertyMutation(*this, true);
        }
        static unsigned long signalMask = (1 << ReadOnly) | (1 << Hidden);
        if ((status & signalMask) != (oldStatus & signalMask)) {
            father->onPropertyStatusChanged(*this, oldStatus);
        }
    }
}

void Property::setStatus(Status pos, bool on)
{
    auto bits = StatusBits;
    bits.set(pos, on);
    setStatusValue(bits.to_ulong());
}

bool Property::isSame(const Property& other) const
{
    if (&other == this) {
        return true;
    }
    if (other.getTypeId() != getTypeId() || getMemSize() != other.getMemSize()) {
        return false;
    }

    Base::StringWriter writer, writer2;
    Save(writer);
    other.Save(writer2);
    return writer.getString() == writer2.getString();
}

//**************************************************************************
//**************************************************************************
// PropertyListsBase
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void PropertyListsBase::_setPyObject(PyObject* value)
{
    std::vector<int> indices;
    std::vector<PyObject*> vals;
    Py::Object pySeq;

    if (PyDict_Check(value)) {
        Py::Dict dict(value);
        auto size = dict.size();
        vals.reserve(size);
        indices.reserve(size);
        int listSize = getSize();
        for (auto it = dict.begin(); it != dict.end(); ++it) {
            const auto& item = *it;
            PyObject* key = item.first.ptr();
            if (!PyLong_Check(key)) {
                throw Base::TypeError("expect key type to be integer");
            }
            long idx = PyLong_AsLong(key);
            if (idx < -1 || idx > listSize) {
                throw Base::ValueError("index out of bound");
            }
            if (idx == -1 || idx == listSize) {
                idx = listSize;
                ++listSize;
            }
            indices.push_back(idx);
            vals.push_back(item.second.ptr());
        }
    }
    else {
        if (PySequence_Check(value)) {
            pySeq = value;
        }
        else {
            PyObject* iter = PyObject_GetIter(value);
            if (iter) {
                Py::Object pyIter(iter, true);
                pySeq = Py::asObject(PySequence_Fast(iter, ""));
            }
            else {
                PyErr_Clear();
                vals.push_back(value);
            }
        }
        if (!pySeq.isNone()) {
            Py::Sequence seq(pySeq);
            vals.reserve(seq.size());
            for (auto it = seq.begin(); it != seq.end(); ++it) {
                vals.push_back((*it).ptr());
            }
        }
    }
    setPyValues(vals, indices);
}


//**************************************************************************
//**************************************************************************
// PropertyLists
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE_ABSTRACT(App::PropertyLists, App::Property)
