// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <QCryptographicHash>

#include <App/Application.h>
#include <App/BackupPolicy.h>
#include <App/CollaborationRegistry.h>
#include <App/Document.h>
#include <App/DocumentFileWriter.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectPy.h>
#include <App/DocumentObjectGroup.h>
#include <App/DocumentRevisionIndex.h>
#include <App/FeatureTest.h>
#include <App/MutationKind.h>
#include <App/PropertyLinks.h>
#include <App/PropertyPythonObject.h>
#include <App/PropertyStandard.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Base/Parameter.h>
#include <Base/Reader.h>
#include <src/App/InitApplication.h>

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
namespace App::Internal
{
class CollaborationRegistryTestAccess
{
public:
    static void setTombstonePreparationHook(void (*hook)())
    {
        CollaborationRegistry::_tombstonePreparationTestHook.store(
            hook, std::memory_order_release);
    }
};
}  // namespace App::Internal
#endif

namespace
{

using App::DocumentRevision;
using App::DocumentRevisionKey;

class DocumentSaveToFileAbiProbe: public App::Document
{
public:
    using LegacySignature = bool (App::Document::*)(const char*) const;
    using App::Document::saveToFile;

    static LegacySignature legacyMember()
    {
        return static_cast<LegacySignature>(
            &DocumentSaveToFileAbiProbe::saveToFile);
    }
};

class DocumentSaveAsWithOutcomeAbiProbe: public App::Document
{
public:
    using LegacySignature = App::DocumentSaveOutcome (App::Document::*)(const char*, bool);
    using App::Document::saveAsWithOutcome;

    static LegacySignature legacyMember()
    {
        return static_cast<LegacySignature>(
            &DocumentSaveAsWithOutcomeAbiProbe::saveAsWithOutcome);
    }
};

class CloseDestructionProbeFeature final: public App::FeatureTest
{
public:
    ~CloseDestructionProbeFeature() override
    {
        if (destroyed) {
            *destroyed = true;
        }
    }

    bool* destroyed {nullptr};
};

class ThrowOnArmedCopy
{
public:
    explicit ThrowOnArmedCopy(std::shared_ptr<std::atomic<bool>> armed)
        : armed(std::move(armed))
    {}

    ThrowOnArmedCopy(const ThrowOnArmedCopy& other)
        : armed(other.armed)
    {
        if (armed->load(std::memory_order_acquire)) {
            throw std::bad_alloc();
        }
    }

    ThrowOnArmedCopy(ThrowOnArmedCopy&&) noexcept = default;

    void operator()(const App::Document&) const {}

private:
    std::shared_ptr<std::atomic<bool>> armed;
};

class ReentrantSignalCopy
{
public:
    using Signal = fastsignals::signal<void(const App::Document&)>;

    ReentrantSignalCopy(
        Signal* signal,
        std::shared_ptr<std::atomic<bool>> armed,
        std::shared_ptr<std::atomic<bool>> reentered)
        : signal(signal)
        , armed(std::move(armed))
        , reentered(std::move(reentered))
    {}

    ReentrantSignalCopy(const ReentrantSignalCopy& other)
        : signal(other.signal)
        , armed(other.armed)
        , reentered(other.reentered)
    {
        if (!armed->load(std::memory_order_acquire)) {
            return;
        }
        static_cast<void>(signal->num_slots());
        auto transient = signal->connect([](const App::Document&) {});
        transient.disconnect();
        reentered->store(true, std::memory_order_release);
    }

    ReentrantSignalCopy(ReentrantSignalCopy&&) noexcept = default;

    void operator()(const App::Document&) const {}

private:
    Signal* signal;
    std::shared_ptr<std::atomic<bool>> armed;
    std::shared_ptr<std::atomic<bool>> reentered;
};

class ThrowOnArmedOutcomeCopy
{
public:
    explicit ThrowOnArmedOutcomeCopy(std::shared_ptr<std::atomic<bool>> armed)
        : armed(std::move(armed))
    {}

    ThrowOnArmedOutcomeCopy(const ThrowOnArmedOutcomeCopy& other)
        : armed(other.armed)
    {
        if (armed->load(std::memory_order_acquire)) {
            throw std::bad_alloc();
        }
    }

    ThrowOnArmedOutcomeCopy(ThrowOnArmedOutcomeCopy&&) noexcept = default;

    void operator()(const App::Document&, const App::DocumentSaveOutcome&) const {}

private:
    std::shared_ptr<std::atomic<bool>> armed;
};

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
void throwTombstonePreparationAllocationFailure()
{
    throw std::bad_alloc();
}

class TombstonePreparationHookReset
{
public:
    ~TombstonePreparationHookReset()
    {
        App::Internal::CollaborationRegistryTestAccess::setTombstonePreparationHook(nullptr);
    }
};
#endif

static_assert(std::is_same_v<
              decltype(DocumentSaveToFileAbiProbe::legacyMember()),
              DocumentSaveToFileAbiProbe::LegacySignature>);
static_assert(std::is_same_v<
              decltype(DocumentSaveAsWithOutcomeAbiProbe::legacyMember()),
              DocumentSaveAsWithOutcomeAbiProbe::LegacySignature>);

TEST(DocumentAbiCompatibility, exposesExactLegacySaveToFileMemberSignature)
{
    EXPECT_TRUE(DocumentSaveToFileAbiProbe::legacyMember() != nullptr);
}

TEST(DocumentAbiCompatibility, exposesExactTwoArgumentSaveAsOutcomeMemberSignature)
{
    EXPECT_TRUE(DocumentSaveAsWithOutcomeAbiProbe::legacyMember() != nullptr);
}

const DocumentRevisionKey& wildcardKey()
{
    static const auto key = DocumentRevisionKey::unknownModelMutation();
    return key;
}

struct ReviewedDelegation
{
    std::string_view key;
    std::string_view requiredEvidence;
    std::size_t expectedCount;
};

struct ExpectedMutatorInventory
{
    std::string_view filename;
    std::string_view category;
    std::size_t expectedCount;
};

// These are wrappers, rejected setters, attachment helpers, or metadata-only configuration.
// Storage writers are intentionally absent: they must bracket, publish, or reject themselves.
constexpr std::array reviewedDelegations {
    ReviewedDelegation {"Property.cpp:Property::setContainer", "this->father = father", 1},
    ReviewedDelegation {"Property.cpp:Property::setPathValue", "path.setValue", 1},
    ReviewedDelegation {"Property.cpp:Property::setReadOnly", "setStatus(", 1},
    ReviewedDelegation {"Property.cpp:Property::setStatus", "setStatusValue(", 1},
    ReviewedDelegation {
        "PropertyContainer.cpp:PropertyContainer::setPropertyStatus",
        "classifyMutation(input)",
        1
    },
    ReviewedDelegation {
        "PropertyContainerPyImp.cpp:PropertyContainerPy::setEditorMode",
        "setStatusValue(",
        1
    },
    ReviewedDelegation {
        "PropertyContainerPyImp.cpp:PropertyContainerPy::setPropertyStatus",
        "setStatusValue(",
        1
    },
    ReviewedDelegation {
        "PropertyContainerPyImp.cpp:PropertyContainerPy::setGroupOfProperty",
        "changeDynamicProperty(",
        1
    },
    ReviewedDelegation {
        "PropertyContainerPyImp.cpp:PropertyContainerPy::setDocumentationOfProperty",
        "changeDynamicProperty(",
        1
    },
    ReviewedDelegation {
        "PropertyContainerPyImp.cpp:PropertyContainerPy::setCustomAttributes",
        "setPyObject(",
        1
    },
    ReviewedDelegation {
        "PropertyExpressionEngine.cpp:PropertyExpressionEngine::setPyObject",
        "throw Base::RuntimeError",
        1
    },
    ReviewedDelegation {
        "PropertyExpressionEngine.cpp:PropertyExpressionEngine::setExpressions",
        "AtomicPropertyChange signaller(*this)",
        1
    },
    ReviewedDelegation {"PropertyFile.cpp:PropertyFileIncluded::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyFile.cpp:PropertyFileIncluded::setFilter", "filter =", 1},
    ReviewedDelegation {"PropertyFile.cpp:PropertyFile::setFilter", "filter =", 1},
    ReviewedDelegation {"PropertyFile.cpp:PropertyFile::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyGeo.cpp:PropertyVector::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyGeo.cpp:PropertyVectorList::setValue", "setValue(", 1},
    ReviewedDelegation {"PropertyGeo.cpp:PropertyMatrix::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyGeo.cpp:PropertyPlacement::setValueIfChanged", "setValue(", 1},
    ReviewedDelegation {"PropertyGeo.cpp:PropertyPlacement::setPathValue", "setValue(", 1},
    ReviewedDelegation {"PropertyGeo.cpp:PropertyPlacement::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyGeo.cpp:PropertyRotation::setValueIfChanged", "setValue(", 1},
    ReviewedDelegation {"PropertyGeo.cpp:PropertyRotation::setPathValue", "setValue(", 1},
    ReviewedDelegation {"PropertyGeo.cpp:PropertyRotation::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkBase::setAllowExternal", "setFlag(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkBase::setSilentRestore", "setFlag(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkBase::setReturnNewElement", "setFlag(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLink::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLink::resetLink", "resetLinkNoNotify()", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkSub::setSyncSubObject", "_Flags.set", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkSub::setValue", "setValue(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkSub::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkSubList::setSyncSubObject", "_Flags.set", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkSubList::setValues", "setValues(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkSubList::setSubListValues", "setValues(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyLinkSubList::setPyObject", "setValues(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLink::setSyncSubObject", "_Flags.set", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLink::setSubName", "setSubValues(", 1},
    ReviewedDelegation {
        "PropertyLinks.cpp:PropertyXLink::setSubValues",
        "setSubValuesNoNotify(",
        1
    },
    ReviewedDelegation {
        "PropertyLinks.cpp:PropertyXLink::setSubValuesNoNotify",
        "_SubList =",
        1
    },
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLink::setValue", "setValue(", 3},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLink::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLink::setAllowPartial", "setFlag(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLinkSubList::setSyncSubObject", "_Flags.set", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLinkSubList::setValue", "setValues(", 2},
    ReviewedDelegation {
        "PropertyLinks.cpp:PropertyXLinkSubList::setValues",
        "setValues(std::move(values))",
        2
    },
    ReviewedDelegation {
        "PropertyLinks.cpp:PropertyXLinkSubList::setValues",
        "setValues(std::map",
        1
    },
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLinkSubList::setSubListValues", "setValues(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLinkSubList::setPyObject", "setValues(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLinkSubList::setAllowPartial", "setFlag(", 1},
    ReviewedDelegation {"PropertyLinks.cpp:PropertyXLinkList::setPyObject", "setValues(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyInteger::setPathValue", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyPath::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyEnumeration::setEnums", "setEnumVector(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyEnumeration::setPathValue", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyEnumeration::setPyPathValue", "setPyObject(", 1},
    ReviewedDelegation {
        "PropertyStandard.cpp:PropertyIntegerConstraint::setConstraints",
        "_ConstStruct =",
        1
    },
    ReviewedDelegation {"PropertyStandard.cpp:PropertyIntegerSet::setPyObject", "setValues(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyFloat::setPathValue", "setValue(", 1},
    ReviewedDelegation {
        "PropertyStandard.cpp:PropertyFloatConstraint::setConstraints",
        "_ConstStruct =",
        1
    },
    ReviewedDelegation {"PropertyStandard.cpp:PropertyString::setValue", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyString::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyString::setPathValue", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyUUID::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyStringList::setValues", "setValues(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyMap::setValue", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyMap::setPathValue", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyMap::setPyObject", "setValues(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyBool::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyBool::setPathValue", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyBoolList::setPyObject", "setValues(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyColor::setPyObject", "setValue(", 1},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyMaterial::setValue", "setDiffuseColor(", 3},
    ReviewedDelegation {"PropertyStandard.cpp:PropertyMaterial::setPyObject", "setValue(", 1},
    ReviewedDelegation {
        "PropertyStandard.cpp:PropertyMaterialList::setMinimumSizeOne",
        "_lValueList.resize(1)",
        1
    },
    ReviewedDelegation {"PropertyStandard.cpp:PropertyMaterialList::setValue", "setValue(", 1},
    ReviewedDelegation {"PropertyUnits.cpp:PropertyQuantity::setPathValue", "setValue(", 1},
    ReviewedDelegation {
        "PropertyUnits.cpp:PropertyQuantityConstraint::setConstraints",
        "_ConstStruct =",
        1
    },
    ReviewedDelegation {"PropertyUnits.cpp:PropertyQuantityConstraint::setPyObject", "setValue(", 1},
    ReviewedDelegation {
        "PropertyPythonObject.cpp:PropertyPythonObject::fromString",
        "applyParsedString(parsed)",
        1
    },
    ReviewedDelegation {
        "PropertyContainer.cpp:PropertyContainer::addDynamicProperty",
        "dynamicProps.addDynamicProperty(",
        1
    },
    ReviewedDelegation {
        "PropertyContainer.cpp:PropertyData::addProperty",
        "Prop->syncType(",
        1
    },
    ReviewedDelegation {
        "PropertyLinks.cpp:PropertyLinkSubList::removeValue",
        "setValues(links, subs)",
        1
    },
    ReviewedDelegation {
        "PropertyLinks.cpp:PropertyLink::resetLinkNoNotify",
        "_pcLink = nullptr",
        1
    },
    ReviewedDelegation {
        "PropertyLinks.cpp:PropertyXLinkSubList::addValue",
        "addValue(obj, std::vector<std::string>(subs), reset)",
        1
    },
    ReviewedDelegation {
        "PropertyLinks.cpp:PropertyXLinkContainer::clearDeps",
        "_Deps.clear()",
        1
    },
    ReviewedDelegation {
        "PropertyStandard.cpp:PropertyMaterialList::resizeByOneIfNeeded",
        "_lValueList.resize(size + 1)",
        1
    },
};

// Header-defined wrappers and configuration-only methods are reviewed separately so that a new
// inline mutator cannot disappear into the much larger out-of-line inventory.
constexpr std::array reviewedHeaderDelegations {
    ReviewedDelegation {"Property.h:setSinglePrecision", "{}", 1},
    ReviewedDelegation {"Property.h:clearTouchList", "_touchList.clear()", 1},
    ReviewedDelegation {"Property.h:setPyValues", "throw Base::NotImplementedError", 1},
    ReviewedDelegation {"Property.h:setPyValues", "atomic_change guard(*this)", 1},
    ReviewedDelegation {"Property.h:setPyObject", "_setPyObject(obj)", 1},
    ReviewedDelegation {"Property.h:setOrderRelevant", "setStatus(", 1},
    ReviewedDelegation {"Property.h:setValue", "setValues(", 2},
    ReviewedDelegation {"Property.h:setPyObject", "setValue(", 1},
    ReviewedDelegation {"Property.h:removeIf", "setValues(", 1},
    ReviewedDelegation {
        "PropertyContainer.h:removeDynamicProperty",
        "dynamicProps.removeDynamicProperty(",
        1
    },
    ReviewedDelegation {
        "PropertyContainer.h:setPropertyPrefix",
        "_propertyPrefix = prefix",
        1
    },
    ReviewedDelegation {"PropertyExpressionEngine.h:setValue", "{}", 1},
    ReviewedDelegation {
        "PropertyExpressionEngine.h:setValidator",
        "validator = f",
        1
    },
    ReviewedDelegation {"PropertyLinks.h:setScope", "_pcScope = scope", 1},
    ReviewedDelegation {"PropertyLinks.h:setAllowPartial", "(void)enable", 1},
    ReviewedDelegation {"PropertyLinks.h:setFlag", "_Flags.set(", 1},
    ReviewedDelegation {"PropertyLinks.h:setPyObject", "_setPyObject(obj)", 1},
    ReviewedDelegation {"PropertyOverrides.h:set", "p.setValue(v)", 1},
    ReviewedDelegation {"PropertyOverrides.h:clear", "_guards.clear()", 1},
    ReviewedDelegation {"PropertyStandard.h:setEditorName", "_editorTypeName = name", 1},
    ReviewedDelegation {"PropertyStandard.h:setDeletable", "candelete = on", 2},
    ReviewedDelegation {"PropertyStandard.h:setValue", "{;}", 1},
    ReviewedDelegation {"PropertyStandard.h:setValues", "setValue(map)", 2},
    ReviewedDelegation {"PropertyStandard.h:set1Value", "setValue(key, value)", 1},
    ReviewedDelegation {
        "PropertyStandard.h:setValue",
        "PropertyListsT<Material>::setValue(",
        1
    },
    ReviewedDelegation {"PropertyUnits.h:setUnit", "_Unit = u", 1},
    ReviewedDelegation {"PropertyUnits.h:setValue", "PropertyFloat::setValue(", 1},
    ReviewedDelegation {"PropertyUnits.h:setFormat", "_Format = fmt", 1},
};

constexpr std::array expectedSourceMutators {
    ExpectedMutatorInventory {"Property.cpp", "setter", 5},
    ExpectedMutatorInventory {"Property.cpp", "purge", 1},
    ExpectedMutatorInventory {"Property.cpp", "touch", 1},
    ExpectedMutatorInventory {"PropertyContainer.cpp", "collection", 2},
    ExpectedMutatorInventory {"PropertyContainer.cpp", "setter", 1},
    ExpectedMutatorInventory {"PropertyContainerPyImp.cpp", "setter", 5},
    ExpectedMutatorInventory {"PropertyExpressionEngine.cpp", "setter", 3},
    ExpectedMutatorInventory {"PropertyFile.cpp", "setter", 5},
    ExpectedMutatorInventory {"PropertyGeo.cpp", "setter", 14},
    ExpectedMutatorInventory {"PropertyLinks.cpp", "collection", 12},
    ExpectedMutatorInventory {"PropertyLinks.cpp", "setter", 46},
    ExpectedMutatorInventory {"PropertyPythonObject.cpp", "conversion", 1},
    ExpectedMutatorInventory {"PropertyPythonObject.cpp", "setter", 2},
    ExpectedMutatorInventory {"PropertyStandard.cpp", "collection", 2},
    ExpectedMutatorInventory {"PropertyStandard.cpp", "setter", 104},
    ExpectedMutatorInventory {"PropertyUnits.cpp", "setter", 4},
};

constexpr std::array expectedHeaderMutators {
    ExpectedMutatorInventory {"Property.h", "collection", 2},
    ExpectedMutatorInventory {"Property.h", "setter", 12},
    ExpectedMutatorInventory {"PropertyContainer.h", "collection", 1},
    ExpectedMutatorInventory {"PropertyContainer.h", "setter", 1},
    ExpectedMutatorInventory {"PropertyExpressionEngine.h", "setter", 2},
    ExpectedMutatorInventory {"PropertyLinks.h", "setter", 4},
    ExpectedMutatorInventory {"PropertyOverrides.h", "collection", 1},
    ExpectedMutatorInventory {"PropertyOverrides.h", "setter", 1},
    ExpectedMutatorInventory {"PropertyStandard.h", "collection", 1},
    ExpectedMutatorInventory {"PropertyStandard.h", "setter", 8},
    ExpectedMutatorInventory {"PropertyUnits.h", "setter", 3},
};

// Restore/RestoreDocFile execute under the document restore publication boundary, while Paste is
// reached by the transaction undo/redo boundary. Keep their complete definition inventory exact so
// a new lifecycle-named storage writer cannot bypass an explicit collaboration review merely
// because it does not use a conventional set/add/remove prefix.
constexpr std::array expectedLifecycleMutators {
    ExpectedMutatorInventory {"Property.cpp", "Paste", 1},
    ExpectedMutatorInventory {"PropertyContainer.cpp", "Restore", 1},
    ExpectedMutatorInventory {"PropertyExpressionEngine.cpp", "Paste", 1},
    ExpectedMutatorInventory {"PropertyExpressionEngine.cpp", "Restore", 1},
    ExpectedMutatorInventory {"PropertyFile.cpp", "Paste", 1},
    ExpectedMutatorInventory {"PropertyFile.cpp", "Restore", 1},
    ExpectedMutatorInventory {"PropertyFile.cpp", "RestoreDocFile", 1},
    ExpectedMutatorInventory {"PropertyGeo.cpp", "Paste", 7},
    ExpectedMutatorInventory {"PropertyGeo.cpp", "Restore", 6},
    ExpectedMutatorInventory {"PropertyGeo.cpp", "RestoreDocFile", 2},
    ExpectedMutatorInventory {"PropertyLinks.cpp", "Paste", 6},
    ExpectedMutatorInventory {"PropertyLinks.cpp", "Restore", 7},
    ExpectedMutatorInventory {"PropertyPythonObject.cpp", "Paste", 1},
    ExpectedMutatorInventory {"PropertyPythonObject.cpp", "Restore", 1},
    ExpectedMutatorInventory {"PropertyPythonObject.cpp", "RestoreDocFile", 1},
    ExpectedMutatorInventory {"PropertyStandard.cpp", "Paste", 18},
    ExpectedMutatorInventory {"PropertyStandard.cpp", "Restore", 20},
    ExpectedMutatorInventory {"PropertyStandard.cpp", "RestoreDocFile", 3},
};

std::filesystem::path locateRepository()
{
    std::vector<std::filesystem::path> candidates {
        std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path(),
        std::filesystem::current_path(),
    };
#ifdef DATADIR
    candidates.emplace_back(std::filesystem::path(DATADIR).parent_path());
#endif
    for (auto cursor : candidates) {
        while (!cursor.empty()) {
            if (std::filesystem::exists(cursor / "src/App/Property.cpp")
                && std::filesystem::exists(cursor / "src/App/PropertyLinks.cpp")
                && std::filesystem::exists(cursor / "src/App/PropertyStandard.cpp")) {
                return cursor;
            }
            const auto parent = cursor.parent_path();
            if (parent == cursor) {
                break;
            }
            cursor = parent;
        }
    }
    return {};
}

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

std::string sha256File(const std::filesystem::path& path)
{
    return QCryptographicHash::hash(QByteArray::fromStdString(readTextFile(path)),
                                    QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

struct ScopedBoolPreference
{
    ScopedBoolPreference(ParameterGrp::handle group,
                         std::string name,
                         const bool value)
        : group(std::move(group))
        , name(std::move(name))
    {
        for (const auto& [key, existing] : this->group->GetBoolMap()) {
            if (key == this->name) {
                wasPresent = true;
                previous = existing;
                break;
            }
        }
        this->group->SetBool(this->name.c_str(), value);
    }

    ~ScopedBoolPreference()
    {
        if (wasPresent) {
            group->SetBool(name.c_str(), previous);
        }
        else {
            group->RemoveBool(name.c_str());
        }
    }

    ParameterGrp::handle group;
    std::string name;
    bool wasPresent {false};
    bool previous {false};
};

struct ScopedIntPreference
{
    ScopedIntPreference(ParameterGrp::handle group,
                        std::string name,
                        const long value)
        : group(std::move(group))
        , name(std::move(name))
    {
        for (const auto& [key, existing] : this->group->GetIntMap()) {
            if (key == this->name) {
                wasPresent = true;
                previous = existing;
                break;
            }
        }
        this->group->SetInt(this->name.c_str(), value);
    }

    ~ScopedIntPreference()
    {
        if (wasPresent) {
            group->SetInt(name.c_str(), previous);
        }
        else {
            group->RemoveInt(name.c_str());
        }
    }

    ParameterGrp::handle group;
    std::string name;
    bool wasPresent {false};
    long previous {0};
};

struct ScopedAsciiPreference
{
    ScopedAsciiPreference(ParameterGrp::handle group,
                          std::string name,
                          std::string value)
        : group(std::move(group))
        , name(std::move(name))
    {
        for (const auto& [key, existing] : this->group->GetASCIIMap()) {
            if (key == this->name) {
                wasPresent = true;
                previous = existing;
                break;
            }
        }
        this->group->SetASCII(this->name.c_str(), value);
    }

    ~ScopedAsciiPreference()
    {
        if (wasPresent) {
            group->SetASCII(name.c_str(), previous);
        }
        else {
            group->RemoveASCII(name.c_str());
        }
    }

    ParameterGrp::handle group;
    std::string name;
    bool wasPresent {false};
    std::string previous;
};

// Preserve byte offsets while removing everything that could make a comment or literal look like
// a guard, notification, delegation, or even a function definition to the source audit.
std::string lexicallySanitized(std::string source)
{
    enum class LexicalState
    {
        Code,
        LineComment,
        BlockComment,
        String,
        Character
    };
    LexicalState state = LexicalState::Code;
    bool escaped = false;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const char current = source[index];
        const char next = index + 1 < source.size() ? source[index + 1] : '\0';
        if (state == LexicalState::Code) {
            if (current == '/' && next == '/') {
                source[index] = source[index + 1] = ' ';
                state = LexicalState::LineComment;
                ++index;
            }
            else if (current == '/' && next == '*') {
                source[index] = source[index + 1] = ' ';
                state = LexicalState::BlockComment;
                ++index;
            }
            else if (current == '"') {
                source[index] = ' ';
                state = LexicalState::String;
                escaped = false;
            }
            else if (current == '\'') {
                source[index] = ' ';
                state = LexicalState::Character;
                escaped = false;
            }
        }
        else if (state == LexicalState::LineComment) {
            if (current == '\n') {
                state = LexicalState::Code;
            }
            else {
                source[index] = ' ';
            }
        }
        else if (state == LexicalState::BlockComment) {
            if (current == '*' && next == '/') {
                source[index] = source[index + 1] = ' ';
                state = LexicalState::Code;
                ++index;
            }
            else if (current != '\n') {
                source[index] = ' ';
            }
        }
        else {
            const char terminator = state == LexicalState::String ? '"' : '\'';
            if (current != '\n') {
                source[index] = ' ';
            }
            if (escaped) {
                escaped = false;
            }
            else if (current == '\\') {
                escaped = true;
            }
            else if (current == terminator) {
                state = LexicalState::Code;
            }
        }
    }
    return source;
}

std::size_t matchingBrace(const std::string& source, std::size_t opening)
{
    enum class LexicalState
    {
        Code,
        LineComment,
        BlockComment,
        String,
        Character
    };
    LexicalState state = LexicalState::Code;
    bool escaped = false;
    int depth = 0;
    for (std::size_t index = opening; index < source.size(); ++index) {
        const char current = source[index];
        const char next = index + 1 < source.size() ? source[index + 1] : '\0';
        if (state == LexicalState::Code) {
            if (current == '/' && next == '/') {
                state = LexicalState::LineComment;
                ++index;
            }
            else if (current == '/' && next == '*') {
                state = LexicalState::BlockComment;
                ++index;
            }
            else if (current == '"') {
                state = LexicalState::String;
                escaped = false;
            }
            else if (current == '\'') {
                state = LexicalState::Character;
                escaped = false;
            }
            else if (current == '{') {
                ++depth;
            }
            else if (current == '}' && --depth == 0) {
                return index;
            }
        }
        else if (state == LexicalState::LineComment) {
            if (current == '\n') {
                state = LexicalState::Code;
            }
        }
        else if (state == LexicalState::BlockComment) {
            if (current == '*' && next == '/') {
                state = LexicalState::Code;
                ++index;
            }
        }
        else {
            const char terminator = state == LexicalState::String ? '"' : '\'';
            if (escaped) {
                escaped = false;
            }
            else if (current == '\\') {
                escaped = true;
            }
            else if (current == terminator) {
                state = LexicalState::Code;
            }
        }
    }
    return std::string::npos;
}

std::string compacted(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    std::ranges::copy_if(text, std::back_inserter(result), [](unsigned char character) {
        return !std::isspace(character);
    });
    return result;
}

bool containsEvidence(std::string_view sanitizedBody, std::string_view evidence)
{
    return compacted(sanitizedBody).find(compacted(evidence)) != std::string::npos;
}

std::size_t probableMutation(std::string_view body, std::size_t begin, std::size_t end)
{
    static const std::regex mutation(
        R"((?:^|[^=!<>])=(?!=)|\.(?:assign|clear|insert|erase|emplace(?:_back)?|push_back|pop_back|resize|swap|[sS]et(?:[A-Z0-9_]\w*)?|reset)\s*\()"
    );
    if (begin >= end || end > body.size()) {
        return std::string_view::npos;
    }
    const std::string range(body.substr(begin, end - begin));
    std::smatch match;
    if (!std::regex_search(range, match, mutation)) {
        return std::string_view::npos;
    }
    return begin + static_cast<std::size_t>(match.position());
}

std::size_t ownedMutation(std::string_view body, std::size_t begin, std::size_t end)
{
    static const std::regex mutation(
        R"(\b_[A-Za-z]\w*(?:\s*\[[^\]]*\])?\s*(?:=(?!=)|\.(?:assign|clear|insert|erase|emplace(?:_back)?|push_back|pop_back|resize|swap|[sS]et(?:[A-Z0-9_]\w*)?|reset)\s*\()|(?:->|\.)_(?:add|remove|delete|clear|insert|erase|set|reset)[A-Z0-9_]\w*\s*\()"
    );
    if (begin >= end || end > body.size()) {
        return std::string_view::npos;
    }
    const std::string range(body.substr(begin, end - begin));
    std::smatch match;
    if (!std::regex_search(range, match, mutation)) {
        return std::string_view::npos;
    }
    return begin + static_cast<std::size_t>(match.position());
}

bool directlyBracketedOrRejected(std::string_view sanitizedBody)
{
    const auto about = sanitizedBody.find("aboutToSetValue");
    const auto notified = sanitizedBody.rfind("hasSetValue");
    if (about != std::string_view::npos && notified != std::string_view::npos
        && about < notified
        && ownedMutation(sanitizedBody, 0, about) == std::string_view::npos
        && probableMutation(sanitizedBody, about, notified) != std::string_view::npos
        && ownedMutation(sanitizedBody, notified, sanitizedBody.size())
            == std::string_view::npos) {
        return true;
    }

    auto atomic = sanitizedBody.find("AtomicPropertyChange");
    const auto typedAtomic = sanitizedBody.find("atomic_change");
    if (atomic == std::string_view::npos
        || (typedAtomic != std::string_view::npos && typedAtomic < atomic)) {
        atomic = typedAtomic;
    }
    const auto invoked = sanitizedBody.rfind("tryInvoke");
    if (atomic != std::string_view::npos && invoked != std::string_view::npos
        && atomic < invoked
        && ownedMutation(sanitizedBody, 0, atomic) == std::string_view::npos
        && probableMutation(sanitizedBody, atomic, invoked) != std::string_view::npos
        && ownedMutation(sanitizedBody, invoked, sanitizedBody.size())
            == std::string_view::npos) {
        return true;
    }

    const auto enforced = sanitizedBody.find("enforceAtomicPresentationMutationTarget");
    const auto published = sanitizedBody.rfind("publishPropertyMutation");
    if (enforced != std::string_view::npos) {
        const auto mutation = probableMutation(sanitizedBody, enforced, sanitizedBody.size());
        return mutation == std::string_view::npos
            || (published != std::string_view::npos && enforced < mutation && mutation < published
                && ownedMutation(sanitizedBody, 0, enforced) == std::string_view::npos
                && ownedMutation(sanitizedBody, published, sanitizedBody.size())
                    == std::string_view::npos);
    }
    return published != std::string_view::npos
        && probableMutation(sanitizedBody, 0, published) != std::string_view::npos
        && ownedMutation(sanitizedBody, published, sanitizedBody.size())
            == std::string_view::npos;
}

std::string_view mutatorCategory(std::string_view name)
{
    if (name == "fromString") {
        return "conversion";
    }
    if (name == "touch") {
        return "touch";
    }
    if (name.starts_with("purge")) {
        return "purge";
    }
    if (name.starts_with("set")) {
        return "setter";
    }
    return "collection";
}

struct ScopedTemporaryDirectory
{
    explicit ScopedTemporaryDirectory(std::string_view prefix)
        : path(std::filesystem::temp_directory_path()
               / (std::string(prefix)
                  + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path);
    }

    ~ScopedTemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

struct ScopedApplicationDocument
{
    explicit ScopedApplicationDocument(std::string_view prefix)
        : name(App::GetApplication().getUniqueDocumentName(std::string(prefix).c_str()))
        , document(App::GetApplication().newDocument(name.c_str(), "collaborationBoundaryUser"))
    {}

    ~ScopedApplicationDocument()
    {
        if (document && App::GetApplication().getDocument(name.c_str())) {
            App::GetApplication().closeDocument(name.c_str());
        }
    }

    std::string name;
    App::Document* document;
};

class DocumentCollaborationBoundaryTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("collaborationBoundary");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(),
            "collaborationBoundaryUser"
        );
        _document->setMaxUndoStackSize(20);
    }

    void TearDown() override
    {
        App::Internal::setBackupPolicyBeforeInstallHookForTesting({});
        App::Internal::setBackupPolicyCheckpointHookForTesting({});
        App::Internal::setDocumentPostDurableSaveCheckpointHookForTesting(nullptr);
        if (_document) {
            if (App::GetApplication().getDocument(_documentName.c_str())) {
                App::GetApplication().closeDocument(_documentName.c_str());
            }
            _document = nullptr;
        }
    }

    App::Document* document() const
    {
        return _document;
    }

    App::DocumentRevisionIndex& revisions() const
    {
        return _document->collaborationRevisions();
    }

    DocumentRevision wildcardRevision() const
    {
        return revisions().current(wildcardKey());
    }

    std::vector<App::DocumentRevisionKey> dependencyKeysFor(std::string_view objectName) const
    {
        return {
            DocumentRevisionKey::objectExistence(std::string(objectName)),
            DocumentRevisionKey::objectModel(std::string(objectName)),
            DocumentRevisionKey::objectStructure(std::string(objectName)),
            DocumentRevisionKey::documentStructure(),
            DocumentRevisionKey::unknownModelMutation(),
        };
    }

    std::vector<App::DocumentRevisionKey> dependencyKeysFor(const App::DocumentObject& object) const
    {
        return dependencyKeysFor(object.getNameInDocument());
    }

    std::vector<App::DocumentRevisionObservation> captureFor(std::string_view objectName) const
    {
        return revisions().capture(dependencyKeysFor(objectName));
    }

    void expectExactDeltasAndConflicts(
        const std::vector<App::DocumentRevisionObservation>& before,
        const std::array<DocumentRevision, 5>& deltas,
        std::string_view ingress
    ) const
    {
        ASSERT_EQ(before.size(), deltas.size()) << ingress;
        const auto after = revisions().capture(dependencyKeysFor(before.front().key.subject));
        ASSERT_EQ(after.size(), before.size()) << ingress;

        std::size_t expectedConflictCount = 0;
        for (std::size_t index = 0; index < before.size(); ++index) {
            EXPECT_EQ(after[index].key, before[index].key) << ingress;
            EXPECT_EQ(after[index].revision, before[index].revision + deltas[index])
                << ingress << " key index " << index;
            expectedConflictCount += deltas[index] != 0 ? 1U : 0U;
        }

        const auto conflicts = revisions().validate(before);
        ASSERT_EQ(conflicts.size(), expectedConflictCount) << ingress;
        std::size_t conflictIndex = 0;
        for (std::size_t index = 0; index < before.size(); ++index) {
            if (deltas[index] == 0) {
                continue;
            }
            EXPECT_EQ(conflicts[conflictIndex].key, before[index].key) << ingress;
            EXPECT_EQ(conflicts[conflictIndex].expected, before[index].revision) << ingress;
            EXPECT_EQ(conflicts[conflictIndex].current, after[index].revision) << ingress;
            ++conflictIndex;
        }
    }

private:
    std::string _documentName;
    App::Document* _document {};
};

}  // namespace

TEST_F(DocumentCollaborationBoundaryTest, emptyNewDocumentHasNoPendingFileChanges)
{
    EXPECT_STREQ(document()->Label.getValue(), "collaborationBoundaryUser");
    EXPECT_EQ(document()->getFileChangeState(), App::DocumentFileState::NotSaved);
    EXPECT_FALSE(document()->hasPendingFileChanges());
    EXPECT_EQ(document()->getPendingFileChanges().toUnderlyingType(), 0);
}

TEST_F(DocumentCollaborationBoundaryTest,
       applicationTransactionCloseAndPublicHistoryPreserveNativeSemantics)
{
    auto* target = document()->addObject<App::FeatureTest>("HistoryTarget");
    ASSERT_NE(target, nullptr);
    target->Label.setValue("Before");
    document()->clearUndos();
    App::GetApplication().setActiveDocument(document());

    const auto commitId = App::GetApplication().setActiveTransaction(
        App::TransactionName {.name = "coordinator application commit", .temporary = false});
    ASSERT_NE(commitId, App::NullTransaction);
    target->Label.setValue("Committed");
    ASSERT_TRUE(document()->hasPendingTransaction());
    EXPECT_TRUE(App::GetApplication().commitTransaction(commitId));
    EXPECT_FALSE(document()->hasPendingTransaction());
    EXPECT_EQ(document()->getAvailableUndos(), 1);
    EXPECT_STREQ(target->Label.getValue(), "Committed");

    ASSERT_TRUE(document()->undo());
    EXPECT_STREQ(target->Label.getValue(), "Before");
    ASSERT_TRUE(document()->redo());
    EXPECT_STREQ(target->Label.getValue(), "Committed");
    document()->clearUndos();

    const auto abortId = App::GetApplication().setActiveTransaction(
        App::TransactionName {.name = "coordinator application abort", .temporary = false});
    ASSERT_NE(abortId, App::NullTransaction);
    target->Label.setValue("Aborted");
    ASSERT_TRUE(document()->hasPendingTransaction());
    EXPECT_TRUE(App::GetApplication().abortTransaction(abortId));
    EXPECT_FALSE(document()->hasPendingTransaction());
    EXPECT_EQ(document()->getAvailableUndos(), 0);
    EXPECT_STREQ(target->Label.getValue(), "Committed");
}

TEST(DocumentCollaborationBoundaryInventory, everyPropertyMutatorIsBracketedRejectedOrReviewed)
{
    const auto repository = locateRepository();
    ASSERT_FALSE(repository.empty()) << "cannot locate source checkout from " << __FILE__;
    const auto appDirectory = repository / "src/App";

    const std::string mutatorName =
        R"((?:(?:set|add|append|remove|delete|clear|insert|erase|resize|reset|purge)(?:[A-Z0-9_]\w*)?|fromString|touch))";
    const std::regex sourceDefinition(
        "((?:[A-Za-z_]\\w*::)*Property[A-Za-z0-9_]*)::(" + mutatorName
        + R"()\s*\([^;{}]*\)\s*(?:const\s*)?(?:override\s*)?(?:final\s*)?(?:noexcept(?:\([^)]*\))?\s*)?\{)"
    );
    const std::regex headerDefinition(
        R"((?:^|\n)[ \t]*(?:template\s*<[^;{}]*>\s*)?(?:(?:inline|virtual|static|constexpr|consteval|explicit|friend)\s+)*(?:[A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*(?:\s*<[^;{}]*>)?(?:[ \t*&]+|\s+))()"
        + mutatorName
        + R"()\s*\([^;{}]*\)\s*(?:const\s*)?(?:override\s*)?(?:final\s*)?(?:noexcept(?:\([^)]*\))?\s*)?\{)"
    );
    std::map<std::string, std::size_t> sourceReviewedCounts;
    std::map<std::string, std::size_t> headerReviewedCounts;
    std::map<std::string, std::size_t> sourceInventory;
    std::map<std::string, std::size_t> headerInventory;
    std::map<std::string, std::size_t> lifecycleInventory;
    const std::regex lifecycleDefinition(
        R"((?:[A-Za-z_]\w*::)*Property[A-Za-z0-9_]*::(Paste|RestoreDocFile|Restore)\s*\([^;{}]*\)\s*(?:const\s*)?(?:override\s*)?(?:final\s*)?\{)"
    );

    std::vector<std::filesystem::path> sources;
    for (const auto& entry : std::filesystem::directory_iterator(appDirectory)) {
        const auto filename = entry.path().filename().string();
        if (entry.is_regular_file() && entry.path().extension() == ".cpp"
            && filename.starts_with("Property")) {
            sources.push_back(entry.path());
        }
    }
    std::ranges::sort(sources);
    ASSERT_GE(sources.size(), 8U);

    for (const auto& sourcePath : sources) {
        const std::string source = lexicallySanitized(readTextFile(sourcePath));
        ASSERT_FALSE(source.empty()) << sourcePath;
        for (std::sregex_iterator match(source.begin(), source.end(), lifecycleDefinition), end;
             match != end;
             ++match) {
            ++lifecycleInventory[sourcePath.filename().string() + ':' + (*match)[1].str()];
        }
        for (std::sregex_iterator match(source.begin(), source.end(), sourceDefinition), end;
             match != end;
             ++match) {
            const std::string category(mutatorCategory((*match)[2].str()));
            ++sourceInventory[sourcePath.filename().string() + ':' + category];
            const auto opening = static_cast<std::size_t>(match->position() + match->length() - 1);
            const auto closing = matchingBrace(source, opening);
            ASSERT_NE(closing, std::string::npos) << sourcePath << ':' << match->str();
            const std::string_view body(source.data() + opening, closing - opening + 1);
            if (directlyBracketedOrRejected(body)) {
                continue;
            }

            const std::string key = sourcePath.filename().string() + ':' + (*match)[1].str()
                + "::" + (*match)[2].str();
            const auto reviewed = std::ranges::find_if(reviewedDelegations, [&](const auto& item) {
                return item.key == key && containsEvidence(body, item.requiredEvidence);
            });
            if (reviewed == reviewedDelegations.end()) {
                ADD_FAILURE()
                    << key << " mutates or delegates without value bracketing; bracket it, reject it "
                    << "while collaboration is active, or add a narrowly reviewed delegation";
                continue;
            }
            ++sourceReviewedCounts[key + ':' + std::string(reviewed->requiredEvidence)];
        }
    }

    for (const auto& reviewed : reviewedDelegations) {
        const std::string countKey = std::string(reviewed.key) + ':'
            + std::string(reviewed.requiredEvidence);
        EXPECT_EQ(sourceReviewedCounts[countKey], reviewed.expectedCount)
            << "reviewed delegation disappeared or gained an unreviewed overload: " << reviewed.key;
    }

    std::vector<std::filesystem::path> headers;
    for (const auto& entry : std::filesystem::directory_iterator(appDirectory)) {
        const auto filename = entry.path().filename().string();
        if (entry.is_regular_file() && entry.path().extension() == ".h"
            && filename.starts_with("Property")) {
            headers.push_back(entry.path());
        }
    }
    std::ranges::sort(headers);
    ASSERT_GE(headers.size(), 8U);

    for (const auto& headerPath : headers) {
        const std::string source = lexicallySanitized(readTextFile(headerPath));
        ASSERT_FALSE(source.empty()) << headerPath;
        for (std::sregex_iterator match(source.begin(), source.end(), headerDefinition), end;
             match != end;
             ++match) {
            const std::string method = (*match)[1].str();
            const std::string category(mutatorCategory(method));
            ++headerInventory[headerPath.filename().string() + ':' + category];
            const auto opening = static_cast<std::size_t>(match->position() + match->length() - 1);
            const auto closing = matchingBrace(source, opening);
            ASSERT_NE(closing, std::string::npos) << headerPath << ':' << match->str();
            const std::string_view body(source.data() + opening, closing - opening + 1);
            if (directlyBracketedOrRejected(body)) {
                continue;
            }

            const std::string key = headerPath.filename().string() + ':' + method;
            const auto reviewed = std::ranges::find_if(
                reviewedHeaderDelegations,
                [&](const auto& item) {
                    return item.key == key && containsEvidence(body, item.requiredEvidence);
                }
            );
            if (reviewed == reviewedHeaderDelegations.end()) {
                ADD_FAILURE()
                    << key
                    << " is an inline mutator without ordered value bracketing, rejection, or "
                    << "a narrowly reviewed delegation";
                continue;
            }
            ++headerReviewedCounts[key + ':' + std::string(reviewed->requiredEvidence)];
        }
    }

    for (const auto& reviewed : reviewedHeaderDelegations) {
        const std::string countKey = std::string(reviewed.key) + ':'
            + std::string(reviewed.requiredEvidence);
        EXPECT_EQ(headerReviewedCounts[countKey], reviewed.expectedCount)
            << "reviewed inline delegation disappeared or gained an unreviewed overload: "
            << reviewed.key;
    }

    const auto expectExactInventory = [](const auto& observed,
                                         const auto& expected,
                                         std::string_view inventoryName) {
        for (const auto& item : expected) {
            const std::string key = std::string(item.filename) + ':'
                + std::string(item.category);
            const auto found = observed.find(key);
            const auto count = found == observed.end() ? 0U : found->second;
            EXPECT_EQ(count, item.expectedCount)
                << inventoryName << " inventory changed for " << key
                << "; review the new or removed definitions individually";
        }
        for (const auto& [key, count] : observed) {
            const auto found = std::ranges::find_if(expected, [&](const auto& item) {
                return key == std::string(item.filename) + ':' + std::string(item.category);
            });
            EXPECT_NE(found, expected.end())
                << inventoryName << " gained an unreviewed file/category " << key << " (" << count
                << " definitions)";
        }
    };
    expectExactInventory(sourceInventory, expectedSourceMutators, "out-of-line");
    expectExactInventory(headerInventory, expectedHeaderMutators, "header-defined");
    expectExactInventory(lifecycleInventory, expectedLifecycleMutators, "lifecycle writer");
}

TEST_F(DocumentCollaborationBoundaryTest, classifiedPropertyPostChangeUsesTypedRevisionOnly)
{
    auto* object = document()->addObject("App::FeatureTest", "PropertyIngress");
    ASSERT_NE(object, nullptr);

    const auto identity = App::GetApplication().collaborationRegistry().identity(*document());
    ASSERT_TRUE(identity.has_value());
    App::DocumentRevisionCursor publicationCursor {
        identity->instanceId,
        identity->lifecycleEpoch,
        0,
    };
    publicationCursor.afterSequence =
        revisions().pollPublications(publicationCursor, 0).latestSequence;
    const auto beforeWrite = captureFor(object->getNameInDocument());
    object->Label.setValue("Accepted");
    expectExactDeltasAndConflicts(beforeWrite, {0, 1, 0, 0, 0}, "Property::hasSetValue");

    const auto publications = revisions().pollPublications(publicationCursor);
    ASSERT_EQ(publications.status, App::DocumentRevisionCursorStatus::Valid);
    ASSERT_FALSE(publications.gap);
    ASSERT_EQ(publications.events.size(), 1U);
    const auto& event = publications.events.front();
    EXPECT_EQ(event.documentInstanceId, identity->instanceId);
    EXPECT_EQ(event.lifecycleEpoch, identity->lifecycleEpoch);
    ASSERT_EQ(event.changes.size(), 2U);
    const std::string expectedStableIdentity = document()->collaborationObjectIdentity(*object);
    const auto objectChange = std::ranges::find_if(event.changes, [](const auto& change) {
        return change.key.kind == App::DocumentRevisionKind::ObjectModel;
    });
    ASSERT_NE(objectChange, event.changes.end());
    EXPECT_EQ(objectChange->stableObjectIdentity, expectedStableIdentity);
    const auto propertyChange = std::ranges::find_if(event.changes, [](const auto& change) {
        return change.key
            == App::DocumentRevisionKey::objectProperty("PropertyIngress", "Label");
    });
    ASSERT_NE(propertyChange, event.changes.end());
    EXPECT_EQ(propertyChange->stableObjectIdentity, expectedStableIdentity);
    const auto serializedEvent = event.toJson();
    EXPECT_EQ(serializedEvent.find("pointer"), std::string::npos);
    EXPECT_EQ(serializedEvent.find("0x"), std::string::npos);

    const auto beforeSecondWrite = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(object->Label.setValue("Second write"));
    expectExactDeltasAndConflicts(
        beforeSecondWrite, {0, 1, 0, 0, 0}, "second Property::hasSetValue");
    EXPECT_STREQ(object->Label.getValue(), "Second write");
}

TEST_F(DocumentCollaborationBoundaryTest, dynamicPropertySchemaUsesTypedStructureRevision)
{
    auto* object = document()->addObject("App::FeatureTest", "DynamicIngress");
    ASSERT_NE(object, nullptr);

    auto before = captureFor(object->getNameInDocument());
    App::Property* property = object->addDynamicProperty("App::PropertyFloat", "DynamicValue");
    ASSERT_NE(property, nullptr);
    expectExactDeltasAndConflicts(before, {0, 0, 1, 0, 0}, "DynamicProperty add");

    before = captureFor(object->getNameInDocument());
    ASSERT_TRUE(object->renameDynamicProperty(property, "RenamedDynamicValue"));
    expectExactDeltasAndConflicts(before, {0, 0, 1, 0, 0}, "DynamicProperty rename");

    before = captureFor(object->getNameInDocument());
    ASSERT_TRUE(object->removeDynamicProperty("RenamedDynamicValue"));
    expectExactDeltasAndConflicts(before, {0, 0, 1, 0, 0}, "DynamicProperty remove");
}

TEST_F(DocumentCollaborationBoundaryTest, recursiveDynamicPropertyRemovalPublishesExactlyOnce)
{
    auto* object = document()->addObject("App::FeatureTest", "RecursiveDynamicRemovalIngress");
    ASSERT_NE(object, nullptr);
    auto* property = object->addDynamicProperty("App::PropertyFloat", "RecursiveValue");
    ASSERT_NE(property, nullptr);

    bool recursiveCallActive = false;
    bool recursiveCallCompleted = false;
    auto connection = App::GetApplication().signalRemoveDynamicProperty.connect(
        [&](const App::Property& removing) {
            if (&removing != property || recursiveCallActive) {
                return;
            }
            recursiveCallActive = true;
            recursiveCallCompleted = object->removeDynamicProperty("RecursiveValue");
        }
    );
    const auto before = captureFor(object->getNameInDocument());
    EXPECT_TRUE(object->removeDynamicProperty("RecursiveValue"));
    connection.disconnect();

    EXPECT_TRUE(recursiveCallCompleted);
    EXPECT_EQ(object->getPropertyByName("RecursiveValue"), nullptr);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 1, 0, 0},
        "recursive dynamic property removal"
    );
}

TEST_F(DocumentCollaborationBoundaryTest, documentAddAndRemoveObjectUseTypedLifecycleRevisions)
{
    const std::string objectName = "ObjectIngress";
    const auto identity = document()->collaborationIdentity();
    App::DocumentRevisionCursor cursor {
        identity.instanceId,
        identity.lifecycleEpoch,
        revisions().pollPublications(
            {identity.instanceId, identity.lifecycleEpoch, 0},
            0
        ).latestSequence,
    };
    auto before = captureFor(objectName);
    auto* object = document()->addObject("App::FeatureTest", "ObjectIngress");
    ASSERT_NE(object, nullptr);
    expectExactDeltasAndConflicts(before, {1, 0, 0, 1, 0}, "Document::addObject");

    const auto added = revisions().pollPublications(cursor);
    ASSERT_EQ(added.status, App::DocumentRevisionCursorStatus::Valid);
    ASSERT_FALSE(added.gap);
    ASSERT_EQ(added.events.size(), 1U);
    ASSERT_FALSE(added.events.front().changes.empty());
    EXPECT_EQ(
        added.events.front().changes.front().key,
        DocumentRevisionKey::objectExistence(objectName)
    );
    EXPECT_EQ(
        added.events.front().changes.front().stableObjectIdentity,
        document()->collaborationObjectIdentity(*object)
    );

    before = captureFor(objectName);
    document()->removeObject("ObjectIngress");
    expectExactDeltasAndConflicts(before, {1, 0, 0, 1, 0}, "Document::removeObject");
    EXPECT_EQ(document()->getObject("ObjectIngress"), nullptr);
}

TEST_F(DocumentCollaborationBoundaryTest, undoRetainedRemovedObjectWritesDoNotPublish)
{
    const std::string objectName = "UndoRetainedRemovalIngress";
    auto* object = document()->addObject("App::FeatureTest", objectName.c_str());
    ASSERT_NE(object, nullptr);
    document()->clearUndos();
    document()->openTransaction("retain removed object");
    document()->removeObject(objectName.c_str());
    document()->commitTransaction();
    ASSERT_EQ(document()->getObject(objectName.c_str()), nullptr);
    ASSERT_FALSE(object->isAttachedToDocument());

    const auto before = captureFor(objectName);
    EXPECT_NO_THROW(object->Label.setValue("Detached mutation"));
    EXPECT_STREQ(object->Label.getValue(), "Detached mutation");
    EXPECT_NE(object->addDynamicProperty("App::PropertyString", "DetachedSchema"), nullptr);
    EXPECT_TRUE(object->removeDynamicProperty("DetachedSchema"));
    EXPECT_EQ(revisions().capture(dependencyKeysFor(objectName)), before);
}

TEST_F(DocumentCollaborationBoundaryTest, directContainerMembershipUsesTypedStructureRevisions)
{
    auto* group = document()->addObject<App::DocumentObjectGroup>("ContainerIngress");
    auto* child = document()->addObject("App::FeatureTest", "ContainerChild");
    ASSERT_NE(group, nullptr);
    ASSERT_NE(child, nullptr);

    auto before = captureFor(group->getNameInDocument());
    ASSERT_FALSE(group->addObject(child).empty());
    expectExactDeltasAndConflicts(before, {0, 0, 1, 1, 0}, "direct group/container add");

    before = captureFor(group->getNameInDocument());
    ASSERT_FALSE(group->removeObject(child).empty());
    expectExactDeltasAndConflicts(before, {0, 0, 1, 1, 0}, "direct group/container remove");
}

TEST_F(DocumentCollaborationBoundaryTest, recomputeResultClassifiesKnownAndUnknownProperties)
{
    auto* feature = document()->addObject<App::FeatureTest>("RecomputeIngress");
    ASSERT_NE(feature, nullptr);
    feature->touch();
    const int executionsBefore = feature->ExecCount.getValue();
    const auto before = captureFor(feature->getNameInDocument());

    ASSERT_GT(document()->recompute(), 0);

    EXPECT_GT(feature->ExecCount.getValue(), executionsBefore);
    expectExactDeltasAndConflicts(before, {0, 4, 0, 0, 2}, "recompute output writes");
}

TEST_F(DocumentCollaborationBoundaryTest, errorOnlyFeatureRecomputePublishesEveryAttempt)
{
    auto* feature = document()->addObject<App::FeatureTestException>("ErrorRecomputeIngress");
    ASSERT_NE(feature, nullptr);
    ASSERT_TRUE(feature->isValid());

    auto before = captureFor(feature->getNameInDocument());
    EXPECT_FALSE(document()->recomputeFeature(feature, false));
    EXPECT_FALSE(feature->isValid());
    expectExactDeltasAndConflicts(
        before,
        {0, 1, 0, 0, 1},
        "error-only feature recompute"
    );

    before = captureFor(feature->getNameInDocument());
    feature->purgeError();
    EXPECT_TRUE(feature->isValid());
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 1}, "purge recompute error");

    before = captureFor(feature->getNameInDocument());
    EXPECT_FALSE(document()->recomputeFeature(feature, false));
    EXPECT_FALSE(feature->isValid());
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 1}, "second error recompute");
}

TEST_F(DocumentCollaborationBoundaryTest, propertyStatusMutationIsStructuralAndExactlyRevisioned)
{
    auto* object = document()->addObject("App::FeatureTest", "PropertyStatusIngress");
    ASSERT_NE(object, nullptr);
    auto* property = object->addDynamicProperty("App::PropertyFloat", "StatusValue");
    ASSERT_NE(property, nullptr);
    const auto before = captureFor(object->getNameInDocument());

    property->setStatus(App::Property::Hidden, true);

    expectExactDeltasAndConflicts(before, {0, 0, 1, 0, 0}, "Property::setStatusValue");
}

TEST_F(DocumentCollaborationBoundaryTest, freezeStatePublishesForEveryMutation)
{
    auto* object = document()->addObject("App::FeatureTest", "FreezeIngress");
    ASSERT_NE(object, nullptr);

    const auto beforeFreeze = captureFor(object->getNameInDocument());
    object->freeze();
    EXPECT_TRUE(object->isFreezed());
    const auto afterFreeze = captureFor(object->getNameInDocument());
    EXPECT_EQ(afterFreeze[1].revision, beforeFreeze[1].revision + 1);
    EXPECT_GT(afterFreeze[4].revision, beforeFreeze[4].revision);

    object->unfreeze(true);
    EXPECT_FALSE(object->isFreezed());
    const auto afterUnfreeze = captureFor(object->getNameInDocument());
    EXPECT_EQ(afterUnfreeze[1].revision, afterFreeze[1].revision + 1);
    EXPECT_GT(afterUnfreeze[4].revision, afterFreeze[4].revision);

    const auto beforeSecondFreeze = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(object->freeze());
    EXPECT_TRUE(object->isFreezed());
    expectExactDeltasAndConflicts(
        beforeSecondFreeze, {0, 1, 0, 0, 1}, "second freeze mutation");
}

TEST_F(DocumentCollaborationBoundaryTest, pythonRecomputeStatusIngressPublishesEveryMutation)
{
    auto* object = document()->addObject("App::FeatureTest", "PythonStatusIngress");
    ASSERT_NE(object, nullptr);
    Base::PyGILStateLocker gil;
    auto* pythonObject = static_cast<App::DocumentObjectPy*>(object->getPyObject());
    ASSERT_NE(pythonObject, nullptr);

    auto before = captureFor(object->getNameInDocument());
    pythonObject->setNoTouch(Py::Boolean(true));
    EXPECT_TRUE(object->testStatus(App::ObjectStatus::NoTouch));
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 1}, "Python NoTouch status");

    Py::Tuple noArguments;
    before = captureFor(object->getNameInDocument());
    Py::Object touchResult(pythonObject->touch(noArguments.ptr()), true);
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 1}, "Python object touch");

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(pythonObject->setNoTouch(Py::Boolean(false)));
    EXPECT_FALSE(object->testStatus(App::ObjectStatus::NoTouch));
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 1}, "Python NoTouch clear");
}

TEST_F(DocumentCollaborationBoundaryTest, dynamicExtensionPublishesForEveryObject)
{
    auto* object = document()->addObject("App::FeatureTest", "DynamicExtensionIngress");
    ASSERT_NE(object, nullptr);
    const auto extensionType = Base::Type::fromName("App::GroupExtensionPython");
    ASSERT_FALSE(extensionType.isBad());
    ASSERT_FALSE(object->hasExtension(extensionType, false));

    Base::PyGILStateLocker gil;
    Py::Object pythonObject(object->getPyObject(), true);
    Py::Tuple args(1);
    args.setItem(0, Py::String("App::GroupExtensionPython"));
    auto before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(pythonObject.callMemberFunction("addExtension", args));
    ASSERT_TRUE(object->hasExtension(extensionType, false));
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 1, 0, 1},
        "ExtensionContainerPy::addExtension"
    );

    auto* second = document()->addObject("App::FeatureTest", "SecondExtensionIngress");
    ASSERT_NE(second, nullptr);
    Py::Object secondPython(second->getPyObject(), true);
    before = captureFor(second->getNameInDocument());
    EXPECT_NO_THROW(secondPython.callMemberFunction("addExtension", args));
    EXPECT_TRUE(second->hasExtension(extensionType, false));
    expectExactDeltasAndConflicts(
        before, {0, 0, 1, 0, 1}, "second ExtensionContainerPy::addExtension");
}

TEST_F(DocumentCollaborationBoundaryTest, pythonObjectPublicSettersAreBracketedAndExactlyRevisioned)
{
    auto* object = document()->addObject("App::FeatureTest", "PythonPropertyIngress");
    ASSERT_NE(object, nullptr);
    auto* property = dynamic_cast<App::PropertyPythonObject*>(
        object->addDynamicProperty("App::PropertyPythonObject", "PythonValue")
    );
    ASSERT_NE(property, nullptr);

    Base::PyGILStateLocker gil;
    Py::List first;
    first.append(Py::Long(1));
    auto before = captureFor(object->getNameInDocument());
    property->setValue(first);
    expectExactDeltasAndConflicts(before, {0, 0, 0, 0, 1}, "PropertyPythonObject::setValue");

    Py::List second;
    second.append(Py::Long(2));
    before = captureFor(object->getNameInDocument());
    property->setPyObject(second.ptr());
    expectExactDeltasAndConflicts(before, {0, 0, 0, 0, 1}, "PropertyPythonObject::setPyObject");

    before = captureFor(object->getNameInDocument());
    property->fromString("[3, 4]");
    expectExactDeltasAndConflicts(before, {0, 0, 0, 0, 1}, "PropertyPythonObject::fromString");

    before = captureFor(object->getNameInDocument());
    const auto valueBeforeMalformedInput = property->toString();
    property->fromString("{");
    EXPECT_EQ(property->toString(), valueBeforeMalformedInput);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before)
        << "malformed input rejected before mutation must not publish";

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(property->fromString("[5]"));
    expectExactDeltasAndConflicts(before, {0, 0, 0, 0, 1}, "second fromString");
    EXPECT_EQ(property->toString(), "[5]");
}

TEST_F(DocumentCollaborationBoundaryTest, pythonObjectDocFileRestorePublishesExactlyOnce)
{
    auto* object = document()->addObject("App::FeatureTest", "PythonRestoreIngress");
    ASSERT_NE(object, nullptr);
    auto* property = dynamic_cast<App::PropertyPythonObject*>(
        object->addDynamicProperty("App::PropertyPythonObject", "PythonValue")
    );
    ASSERT_NE(property, nullptr);

    std::istringstream validInput("[7, 8]");
    Base::Reader validReader(validInput, "PythonValue", 1);
    auto before = captureFor(object->getNameInDocument());
    property->RestoreDocFile(validReader);
    EXPECT_EQ(property->toString(), "[7, 8]");
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 0, 0, 1},
        "PropertyPythonObject::RestoreDocFile"
    );

    std::istringstream invalidInput("{");
    Base::Reader invalidReader(invalidInput, "PythonValue", 1);
    before = captureFor(object->getNameInDocument());
    property->RestoreDocFile(invalidReader);
    EXPECT_EQ(property->toString(), "[7, 8]");
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
}

TEST_F(DocumentCollaborationBoundaryTest, pythonObjectInPlaceExposureFailsClosedForPreparation)
{
    ASSERT_TRUE(document()->collaborationPreparationSupported());
    auto* object = document()->addObject("App::FeatureTest", "PythonInPlaceIngress");
    ASSERT_NE(object, nullptr);
    auto* property = dynamic_cast<App::PropertyPythonObject*>(
        object->addDynamicProperty("App::PropertyPythonObject", "PythonValue")
    );
    ASSERT_NE(property, nullptr);
    EXPECT_FALSE(document()->collaborationPreparationSupported());

    Base::PyGILStateLocker gil;
    Py::List initial;
    initial.append(Py::Long(1));
    property->setValue(initial);

    const auto valueBefore = property->toString();
    const auto revisionsBefore = captureFor(object->getNameInDocument());
    Py::List exposed(property->getValue());
    exposed.append(Py::Long(99));

    EXPECT_NE(property->toString(), valueBefore)
        << "the regression fixture must exercise the mutable in-place exposure";
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), revisionsBefore);
    EXPECT_FALSE(document()->collaborationPreparationSupported())
        << "an unobservable in-place Python mutation must keep prepared edits disabled";

    const auto beforeRemove = captureFor(object->getNameInDocument());
    ASSERT_TRUE(object->removeDynamicProperty("PythonValue"));
    expectExactDeltasAndConflicts(
        beforeRemove,
        {0, 0, 1, 0, 0},
        "remove PropertyPythonObject fail-closed marker"
    );
    EXPECT_TRUE(document()->collaborationPreparationSupported());
}

TEST_F(DocumentCollaborationBoundaryTest, directStatusAndListStorageWritersAreExactlyRevisioned)
{
    auto* object = document()->addObject("App::FeatureTest", "HostileIngress");
    ASSERT_NE(object, nullptr);
    auto* list = dynamic_cast<App::PropertyLinkList*>(
        object->addDynamicProperty("App::PropertyLinkList", "DirectList")
    );
    ASSERT_NE(list, nullptr);
    auto* target = document()->addObject("App::FeatureTest", "DirectListTarget");
    auto* replacement = document()->addObject("App::FeatureTest", "DirectListReplacement");
    ASSERT_NE(target, nullptr);
    ASSERT_NE(replacement, nullptr);

    auto before = captureFor(object->getNameInDocument());
    EXPECT_THROW(list->set1Value(-2, target), Base::RuntimeError);
    EXPECT_EQ(list->getSize(), 0);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    const auto initialTargetInList = target->getInList();
    EXPECT_TRUE(std::ranges::find(initialTargetInList, object) == initialTargetInList.end());

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(list->setSize(-1), Base::ValueError);
    EXPECT_EQ(list->getSize(), 0);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);

    before = captureFor(object->getNameInDocument());
    list->set1Value(-1, target);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 1, 1, 0},
        "PropertyLinkList::set1Value"
    );
    ASSERT_EQ(list->getSize(), 1);
    ASSERT_EQ((*list)[0], target);

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(object->setPropertyStatus(App::Property::Hidden, true));
    expectExactDeltasAndConflicts(before, {0, 0, 1, 0, 0}, "direct property status");

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(list->setSize(2));
    expectExactDeltasAndConflicts(before, {0, 0, 1, 1, 0}, "direct link-list resize");
    EXPECT_EQ(list->getSize(), 2);

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(list->set1Value(0, replacement));
    expectExactDeltasAndConflicts(before, {0, 0, 1, 1, 0}, "direct link-list replace");
    ASSERT_EQ((*list)[0], replacement);

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(list->setValues(std::vector<App::DocumentObject*> {target}));
    expectExactDeltasAndConflicts(before, {0, 0, 1, 1, 0}, "direct link-list values");
    ASSERT_EQ((*list)[0], target);
    const auto targetInList = target->getInList();
    const auto replacementInList = replacement->getInList();
    EXPECT_TRUE(std::ranges::find(targetInList, object) != targetInList.end());
    EXPECT_TRUE(std::ranges::find(replacementInList, object) == replacementInList.end());
}

TEST_F(DocumentCollaborationBoundaryTest, directLinkResetPublishesExactlyOnceEveryTime)
{
    auto* object = document()->addObject("App::FeatureTest", "DirectLinkResetIngress");
    auto* target = document()->addObject("App::FeatureTest", "DirectLinkResetTarget");
    ASSERT_NE(object, nullptr);
    ASSERT_NE(target, nullptr);
    auto* link = dynamic_cast<App::PropertyLink*>(
        object->addDynamicProperty("App::PropertyLink", "DirectLink")
    );
    ASSERT_NE(link, nullptr);
    link->setValue(target);

    auto before = captureFor(object->getNameInDocument());
    link->resetLink();
    EXPECT_EQ(link->getValue(), nullptr);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 1, 1, 0},
        "PropertyLink::resetLink"
    );

    link->setValue(target);
    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(link->resetLink());
    EXPECT_EQ(link->getValue(), nullptr);
    expectExactDeltasAndConflicts(before, {0, 0, 1, 1, 0}, "second PropertyLink::resetLink");
}

TEST_F(DocumentCollaborationBoundaryTest, directXLinkSubValuesPublishEveryMutation)
{
    auto* object = document()->addObject("App::FeatureTest", "DirectXLinkSubIngress");
    auto* target = document()->addObject("App::FeatureTest", "DirectXLinkSubTarget");
    ASSERT_NE(object, nullptr);
    ASSERT_NE(target, nullptr);
    auto* link = dynamic_cast<App::PropertyXLink*>(
        object->addDynamicProperty("App::PropertyXLink", "DirectXLink")
    );
    ASSERT_NE(link, nullptr);
    link->setValue(target);

    auto before = captureFor(object->getNameInDocument());
    link->setSubValues(std::vector<std::string> {"Face1"});
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 1, 1, 0},
        "PropertyXLink::setSubValues"
    );
    ASSERT_EQ(link->getSubValues(), (std::vector<std::string> {"Face1"}));

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(link->setSubValues(std::vector<std::string> {"Face2"}));
    expectExactDeltasAndConflicts(before, {0, 0, 1, 1, 0}, "second XLink sub-values");
    EXPECT_EQ(link->getSubValues(), (std::vector<std::string> {"Face2"}));
}

TEST_F(DocumentCollaborationBoundaryTest, existingXLinkSubListAddPublishesEveryMutation)
{
    auto* object = document()->addObject("App::FeatureTest", "XLinkSubListIngress");
    auto* target = document()->addObject("App::FeatureTest", "XLinkSubListTarget");
    ASSERT_NE(object, nullptr);
    ASSERT_NE(target, nullptr);
    auto* links = dynamic_cast<App::PropertyXLinkSubList*>(
        object->addDynamicProperty("App::PropertyXLinkSubList", "DirectXLinks")
    );
    ASSERT_NE(links, nullptr);
    links->addValue(target, std::vector<std::string> {"Face1"}, false);

    auto before = captureFor(object->getNameInDocument());
    links->addValue(target, std::vector<std::string> {"Face2"}, false);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 1, 1, 0},
        "PropertyXLinkSubList::addValue existing link"
    );
    ASSERT_EQ(
        links->getSubValues(target),
        (std::vector<std::string> {"Face1", "Face2"})
    );

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(links->addValue(target, std::vector<std::string> {"Face3"}, false));
    expectExactDeltasAndConflicts(before, {0, 0, 1, 1, 0}, "third XLinkSubList value");
    EXPECT_EQ(
        links->getSubValues(target),
        (std::vector<std::string> {"Face1", "Face2", "Face3"})
    );
}

TEST_F(DocumentCollaborationBoundaryTest, xLinkSubListRejectsInvalidEndpointsWithoutMutation)
{
    auto* object = document()->addObject("App::FeatureTest", "XLinkValidationIngress");
    auto* first = document()->addObject("App::FeatureTest", "XLinkValidationFirst");
    ASSERT_NE(object, nullptr);
    ASSERT_NE(first, nullptr);
    auto* links = dynamic_cast<App::PropertyXLinkSubList*>(
        object->addDynamicProperty("App::PropertyXLinkSubList", "ValidatedXLinks")
    );
    ASSERT_NE(links, nullptr);
    links->append(first);
    ASSERT_EQ(links->getSize(), 1);

    auto unattached = std::make_unique<App::FeatureTest>();
    const auto before = captureFor(object->getNameInDocument());
    const auto valuesBefore = links->getValues();

    EXPECT_THROW(links->append(unattached.get()), Base::ValueError);
    EXPECT_EQ(links->getValues(), valuesBefore);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);

    EXPECT_THROW(
        links->setValues(std::vector<App::DocumentObject*> {first, unattached.get()}),
        Base::ValueError
    );
    EXPECT_EQ(links->getValues(), valuesBefore);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);

    EXPECT_THROW(
        links->set1Value(-1, unattached.get(), std::vector<std::string> {"Face1"}),
        Base::ValueError
    );
    EXPECT_EQ(links->getValues(), valuesBefore);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);

    EXPECT_THROW(links->appendPair(first, unattached.get()), Base::ValueError);
    EXPECT_EQ(links->getValues(), valuesBefore);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
}

TEST_F(DocumentCollaborationBoundaryTest, listResizeAndNestedAppendPublishExactlyOnce)
{
    auto* object = document()->addObject("App::FeatureTest", "ListMutationIngress");
    ASSERT_NE(object, nullptr);
    auto* list = dynamic_cast<App::PropertyIntegerList*>(
        object->addDynamicProperty("App::PropertyIntegerList", "DynamicIntegerList")
    );
    ASSERT_NE(list, nullptr);

    auto before = captureFor(object->getNameInDocument());
    EXPECT_THROW(list->setSize(-1), Base::ValueError);
    EXPECT_EQ(list->getSize(), 0);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);

    before = captureFor(object->getNameInDocument());
    list->setSize(1);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 0, 0, 1},
        "PropertyListsT::setSize direct resize"
    );
    ASSERT_EQ(list->getSize(), 1);

    before = captureFor(object->getNameInDocument());
    list->set1Value(-1, 42);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 0, 0, 1},
        "PropertyListsT::set1Value nested setSize append"
    );
    ASSERT_EQ(list->getSize(), 2);
    EXPECT_EQ((*list)[1], 42);

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(list->setSize(3));
    expectExactDeltasAndConflicts(before, {0, 0, 0, 0, 1}, "second direct resize");
    EXPECT_EQ(list->getSize(), 3);

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(list->set1Value(-1, 99));
    expectExactDeltasAndConflicts(before, {0, 0, 0, 0, 1}, "second nested append");
    EXPECT_EQ(list->getSize(), 4);
    EXPECT_EQ((*list)[3], 99);
}

TEST_F(DocumentCollaborationBoundaryTest, throwingPropertyObserverPublishesOnceAndGuardRecovers)
{
    auto* object = document()->addObject("App::FeatureTest", "ThrowingObserverIngress");
    ASSERT_NE(object, nullptr);
    auto* list = dynamic_cast<App::PropertyIntegerList*>(
        object->addDynamicProperty("App::PropertyIntegerList", "ObservedList")
    );
    ASSERT_NE(list, nullptr);

    auto connection = document()->signalChangedObject.connect(
        [&](const App::DocumentObject& changedObject, const App::Property& changedProperty) {
            if (&changedObject == object && &changedProperty == list) {
                throw Base::RuntimeError("intentional observer failure");
            }
        }
    );
    auto before = captureFor(object->getNameInDocument());
    EXPECT_THROW(list->set1Value(-1, 42), Base::RuntimeError);
    connection.disconnect();
    ASSERT_EQ(list->getSize(), 1);
    EXPECT_EQ((*list)[0], 42);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 0, 0, 1},
        "throwing property observer"
    );

    before = captureFor(object->getNameInDocument());
    list->set1Value(-1, 43);
    ASSERT_EQ(list->getSize(), 2);
    EXPECT_EQ((*list)[1], 43);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 0, 0, 1},
        "atomic property guard after observer failure"
    );
}

TEST_F(DocumentCollaborationBoundaryTest, touchPublishesBeforeThrowingObserver)
{
    auto* object = document()->addObject("App::FeatureTest", "TouchIngress");
    ASSERT_NE(object, nullptr);
    auto* property = object->addDynamicProperty("App::PropertyInteger", "TouchValue");
    ASSERT_NE(property, nullptr);

    auto connection = document()->signalChangedObject.connect(
        [&](const App::DocumentObject& changedObject, const App::Property& changedProperty) {
            if (&changedObject == object && &changedProperty == property) {
                throw Base::RuntimeError("intentional touch observer failure");
            }
        }
    );
    auto before = captureFor(object->getNameInDocument());
    EXPECT_THROW(property->touch(), Base::RuntimeError);
    connection.disconnect();
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 0}, "throwing touch observer");
}

TEST_F(DocumentCollaborationBoundaryTest, directPropertyPurgePublishesEveryMutation)
{
    auto* object = document()->addObject("App::FeatureTest", "PropertyPurgeIngress");
    ASSERT_NE(object, nullptr);
    auto* property = object->addDynamicProperty("App::PropertyInteger", "PurgeValue");
    ASSERT_NE(property, nullptr);

    property->touch();
    ASSERT_TRUE(property->isTouched());
    auto before = captureFor(object->getNameInDocument());
    property->purgeTouched();
    EXPECT_FALSE(property->isTouched());
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 0}, "Property::purgeTouched");

    property->touch();
    ASSERT_TRUE(property->isTouched());
    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(property->purgeTouched());
    EXPECT_FALSE(property->isTouched());
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 0}, "second property purge");
}

TEST_F(DocumentCollaborationBoundaryTest, materialListAppendPublishesOnceAfterFinalState)
{
    auto* object = document()->addObject("App::FeatureTest", "MaterialListIngress");
    ASSERT_NE(object, nullptr);
    auto* materials = dynamic_cast<App::PropertyMaterialList*>(
        object->addDynamicProperty("App::PropertyMaterialList", "Materials")
    );
    ASSERT_NE(materials, nullptr);
    ASSERT_EQ(materials->getSize(), 1);

    int observerCalls = 0;
    bool observerSawFinalState = false;
    auto connection = document()->signalChangedObject.connect(
        [&](const App::DocumentObject& changedObject, const App::Property& changedProperty) {
            if (&changedObject == object && &changedProperty == materials) {
                ++observerCalls;
                observerSawFinalState = materials->getSize() == 2;
            }
        }
    );
    const auto before = captureFor(object->getNameInDocument());
    materials->setDiffuseColor(-1, Base::Color(0.25F, 0.5F, 0.75F));
    connection.disconnect();

    EXPECT_EQ(materials->getSize(), 2);
    EXPECT_EQ(observerCalls, 1);
    EXPECT_TRUE(observerSawFinalState);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 0, 0, 1},
        "PropertyMaterialList append"
    );
}

TEST_F(DocumentCollaborationBoundaryTest, typedUndoRedoAndAbortAdvanceModelWithoutWildcard)
{
    auto* object = document()->addObject("App::FeatureTest", "TransactionIngress");
    ASSERT_NE(object, nullptr);
    document()->clearUndos();
    const auto modelKey = DocumentRevisionKey::objectModel(object->getNameInDocument());
    const auto structureKey = DocumentRevisionKey::objectStructure(object->getNameInDocument());
    const auto structureBefore = revisions().current(structureKey);

    const std::string originalLabel = object->Label.getValue();
    const auto preparedBeforeEdit = revisions().capture({modelKey, wildcardKey()});
    const auto modelBeforeEdit = revisions().current(modelKey);
    const auto revisionBeforeEdit = wildcardRevision();

    document()->openTransaction("collaboration edit");
    object->Label.setValue("Edited");
    document()->commitTransaction();
    const auto modelAfterEdit = revisions().current(modelKey);
    const auto revisionAfterEdit = wildcardRevision();
    EXPECT_GT(modelAfterEdit, modelBeforeEdit);
    EXPECT_EQ(revisionAfterEdit, revisionBeforeEdit);
    auto conflicts = revisions().validate(preparedBeforeEdit);
    ASSERT_EQ(conflicts.size(), 1U);
    EXPECT_EQ(conflicts[0].expected, modelBeforeEdit);
    EXPECT_EQ(conflicts[0].current, modelAfterEdit);

    const auto preparedBeforeUndo = revisions().capture({modelKey, wildcardKey()});
    ASSERT_TRUE(document()->undo());
    const auto modelAfterUndo = revisions().current(modelKey);
    const auto revisionAfterUndo = wildcardRevision();
    EXPECT_GT(modelAfterUndo, modelAfterEdit);
    EXPECT_EQ(revisionAfterUndo, revisionAfterEdit);
    EXPECT_STREQ(object->Label.getValue(), originalLabel.c_str());
    EXPECT_EQ(revisions().validate(preparedBeforeUndo).size(), 1U);
    EXPECT_FALSE(revisions().validate(preparedBeforeEdit).empty());

    const auto preparedBeforeRedo = revisions().capture({modelKey, wildcardKey()});
    ASSERT_TRUE(document()->redo());
    const auto modelAfterRedo = revisions().current(modelKey);
    const auto revisionAfterRedo = wildcardRevision();
    EXPECT_GT(modelAfterRedo, modelAfterUndo);
    EXPECT_EQ(revisionAfterRedo, revisionAfterUndo);
    EXPECT_STREQ(object->Label.getValue(), "Edited");
    EXPECT_EQ(revisions().validate(preparedBeforeRedo).size(), 1U);
    EXPECT_FALSE(revisions().validate(preparedBeforeEdit).empty());

    const auto preparedBeforeAbortSequence = revisions().capture({modelKey, wildcardKey()});
    document()->openTransaction("collaboration abort");
    object->Label.setValue("AbortedValue");
    const auto modelDuringAbortedEdit = revisions().current(modelKey);
    const auto revisionDuringAbortedEdit = wildcardRevision();
    EXPECT_GT(modelDuringAbortedEdit, modelAfterRedo);
    EXPECT_EQ(revisionDuringAbortedEdit, revisionAfterRedo);
    document()->abortTransaction();
    const auto modelAfterAbort = revisions().current(modelKey);
    const auto revisionAfterAbort = wildcardRevision();
    EXPECT_GT(modelAfterAbort, modelDuringAbortedEdit);
    EXPECT_EQ(revisionAfterAbort, revisionDuringAbortedEdit);
    EXPECT_STREQ(object->Label.getValue(), "Edited");
    EXPECT_EQ(revisions().validate(preparedBeforeAbortSequence).size(), 1U);
    EXPECT_EQ(revisions().current(structureKey), structureBefore);

    const std::array publishedWildcard {
        revisionBeforeEdit,
        revisionAfterEdit,
        revisionAfterUndo,
        revisionAfterRedo,
        revisionDuringAbortedEdit,
        revisionAfterAbort,
    };
    const std::array publishedModel {
        modelBeforeEdit,
        modelAfterEdit,
        modelAfterUndo,
        modelAfterRedo,
        modelDuringAbortedEdit,
        modelAfterAbort,
    };
    for (std::size_t i = 1; i < publishedWildcard.size(); ++i) {
        EXPECT_EQ(publishedWildcard[i], publishedWildcard[i - 1])
            << "typed label mutation unexpectedly advanced the wildcard at step " << i;
        EXPECT_GT(publishedModel[i], publishedModel[i - 1])
            << "object-model revision ABA at step " << i;
    }
}

TEST_F(DocumentCollaborationBoundaryTest, typedDynamicSchemaUndoRedoAndAbortAdvanceStructureOnly)
{
    auto* object = document()->addObject("App::FeatureTest", "StructureTransactionIngress");
    ASSERT_NE(object, nullptr);
    document()->clearUndos();
    const auto structureKey = DocumentRevisionKey::objectStructure(object->getNameInDocument());
    const auto modelKey = DocumentRevisionKey::objectModel(object->getNameInDocument());
    const auto documentStructureKey = DocumentRevisionKey::documentStructure();
    const auto modelBefore = revisions().current(modelKey);
    const auto documentStructureBefore = revisions().current(documentStructureKey);

    const auto preparedBeforeEdit = revisions().capture({structureKey, wildcardKey()});
    const auto structureBeforeEdit = revisions().current(structureKey);
    const auto wildcardBeforeEdit = wildcardRevision();
    document()->openTransaction("dynamic schema edit");
    ASSERT_NE(object->addDynamicProperty("App::PropertyFloat", "TransactionalDynamic"), nullptr);
    document()->commitTransaction();
    const auto structureAfterEdit = revisions().current(structureKey);
    const auto wildcardAfterEdit = wildcardRevision();
    EXPECT_GT(structureAfterEdit, structureBeforeEdit);
    EXPECT_EQ(wildcardAfterEdit, wildcardBeforeEdit);
    EXPECT_EQ(revisions().validate(preparedBeforeEdit).size(), 1U);

    const auto preparedBeforeUndo = revisions().capture({structureKey, wildcardKey()});
    ASSERT_TRUE(document()->undo());
    const auto structureAfterUndo = revisions().current(structureKey);
    const auto wildcardAfterUndo = wildcardRevision();
    EXPECT_GT(structureAfterUndo, structureAfterEdit);
    EXPECT_EQ(wildcardAfterUndo, wildcardAfterEdit);
    EXPECT_EQ(object->getPropertyByName("TransactionalDynamic"), nullptr);
    EXPECT_EQ(revisions().validate(preparedBeforeUndo).size(), 1U);

    const auto preparedBeforeRedo = revisions().capture({structureKey, wildcardKey()});
    ASSERT_TRUE(document()->redo());
    const auto structureAfterRedo = revisions().current(structureKey);
    const auto wildcardAfterRedo = wildcardRevision();
    EXPECT_GT(structureAfterRedo, structureAfterUndo);
    EXPECT_EQ(wildcardAfterRedo, wildcardAfterUndo);
    ASSERT_NE(object->getPropertyByName("TransactionalDynamic"), nullptr);
    EXPECT_EQ(revisions().validate(preparedBeforeRedo).size(), 1U);

    const auto preparedBeforeAbort = revisions().capture({structureKey, wildcardKey()});
    document()->openTransaction("dynamic schema abort");
    ASSERT_TRUE(object->removeDynamicProperty("TransactionalDynamic"));
    const auto structureDuringAbort = revisions().current(structureKey);
    const auto wildcardDuringAbort = wildcardRevision();
    EXPECT_GT(structureDuringAbort, structureAfterRedo);
    EXPECT_EQ(wildcardDuringAbort, wildcardAfterRedo);
    document()->abortTransaction();
    const auto structureAfterAbort = revisions().current(structureKey);
    const auto wildcardAfterAbort = wildcardRevision();
    EXPECT_GT(structureAfterAbort, structureDuringAbort);
    EXPECT_EQ(wildcardAfterAbort, wildcardDuringAbort);
    EXPECT_NE(object->getPropertyByName("TransactionalDynamic"), nullptr);
    EXPECT_EQ(revisions().validate(preparedBeforeAbort).size(), 1U);

    EXPECT_GT(revisions().current(modelKey), modelBefore)
        << "undo/redo/abort may classify recompute-admission status as object model state";
    EXPECT_EQ(revisions().current(documentStructureKey), documentStructureBefore);
}

TEST_F(DocumentCollaborationBoundaryTest, unchangedSaveDoesNotStaleModelOrStructureDependencies)
{
    auto* object = document()->addObject("App::FeatureTest", "SaveIngress");
    ASSERT_NE(object, nullptr);

    ScopedTemporaryDirectory temporary("fc_collaboration_boundary_");
    const auto firstPath = (temporary.path / "first.FCStd").string();
    const auto secondPath = (temporary.path / "second.FCStd").string();

    ASSERT_TRUE(document()->saveAs(firstPath.c_str()));

    const auto beforeSaveAs = revisions().capture(dependencyKeysFor(*object));
    ASSERT_TRUE(document()->saveAs(secondPath.c_str()));
    EXPECT_TRUE(revisions().validate(beforeSaveAs).empty())
        << "save-as bookkeeping must not stale model dependencies";

    const auto beforeSave = revisions().capture(dependencyKeysFor(*object));
    ASSERT_TRUE(document()->save());
    EXPECT_TRUE(revisions().validate(beforeSave).empty())
        << "unchanged save must not stale model dependencies";
}

TEST_F(DocumentCollaborationBoundaryTest, structuredSaveSkipsAnUnchangedCanonicalFile)
{
    ASSERT_NE(document()->addObject("App::FeatureTest", "SaveState"), nullptr);
    ScopedTemporaryDirectory temporary("fc_change_aware_save_");
    const auto path = (temporary.path / "state.FCStd").string();

    const auto first = document()->saveAsWithOutcome(path.c_str());
    ASSERT_EQ(first.disposition, App::DocumentSaveDisposition::Written);
    ASSERT_TRUE(first.fileWritten);
    ASSERT_EQ(document()->getFileChangeState(), App::DocumentFileState::Clean);
    const auto bytesBefore = readTextFile(path);
    const auto timestampBefore = std::filesystem::last_write_time(path);
    const std::string modifiedBefore = document()->LastModifiedDate.getStrValue();

    int starts = 0;
    int finishes = 0;
    auto startConnection = document()->signalStartSave.connect(
        [&](const App::Document&, const std::string&) { ++starts; });
    auto finishConnection = document()->signalFinishSave.connect(
        [&](const App::Document&, const std::string&) { ++finishes; });
    const auto unchanged = document()->saveWithOutcome();
    startConnection.disconnect();
    finishConnection.disconnect();

    EXPECT_EQ(unchanged.disposition, App::DocumentSaveDisposition::Unchanged);
    EXPECT_FALSE(unchanged.fileWritten);
    EXPECT_TRUE(unchanged.succeeded());
    EXPECT_EQ(starts, 0);
    EXPECT_EQ(finishes, 0);
    EXPECT_EQ(readTextFile(path), bytesBefore);
    EXPECT_EQ(std::filesystem::last_write_time(path), timestampBefore);
    EXPECT_EQ(document()->LastModifiedDate.getStrValue(), modifiedBefore);
    EXPECT_FALSE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, canonicalSavepointIsBoundToItsFilePath)
{
    ScopedTemporaryDirectory temporary("fc_canonical_path_binding_");
    const auto original = (temporary.path / "original.FCStd").string();
    const auto replacement = (temporary.path / "replacement.FCStd").string();

    ASSERT_EQ(document()->saveAsWithOutcome(original.c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    {
        std::ofstream occupied(replacement, std::ios::binary);
        occupied << "pre-existing destination";
    }

    int fileStateSignals = 0;
    auto stateConnection = document()->signalFileChangeStateChanged().connect(
        [&](const App::Document&) { ++fileStateSignals; });
    document()->FileName.setValue(replacement);
    EXPECT_GT(fileStateSignals, 0);
    ASSERT_TRUE(document()->hasPendingFileChanges());
    EXPECT_EQ(document()->getFileChangeState(), App::DocumentFileState::Modified);

    const auto outcome = document()->saveWithOutcome();
    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_FALSE(document()->hasPendingFileChanges());
    EXPECT_EQ(document()->getFileChangeState(), App::DocumentFileState::Clean);
    EXPECT_GT(std::filesystem::file_size(replacement),
              std::string_view("pre-existing destination").size());
}

TEST_F(DocumentCollaborationBoundaryTest, missingCanonicalFileAndForceSaveAlwaysWrite)
{
    ASSERT_NE(document()->addObject("App::FeatureTest", "MissingCanonical"), nullptr);
    ScopedTemporaryDirectory temporary("fc_missing_canonical_");
    const auto path = (temporary.path / "missing.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_TRUE(std::filesystem::remove(path));

    const auto recreated = document()->saveWithOutcome();
    EXPECT_EQ(recreated.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(recreated.fileWritten);
    EXPECT_TRUE(std::filesystem::exists(path));

    const auto forced = document()->forceSave();
    EXPECT_EQ(forced.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(forced.fileWritten);
    EXPECT_FALSE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, saveCopyNeverMovesTheCanonicalSavepoint)
{
    auto* object = document()->addObject("App::FeatureTest", "CopyState");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_save_copy_state_");
    const auto canonical = (temporary.path / "canonical.FCStd").string();
    const auto copy = (temporary.path / "copy.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(canonical.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    object->Label.setValue("Dirty copy content");
    ASSERT_TRUE(document()->hasPendingFileChanges());
    const auto outcome = document()->saveCopyWithOutcome(copy.c_str());

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::CopyWritten);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_EQ(document()->FileName.getStrValue(), canonical);
    EXPECT_TRUE(document()->hasPendingFileChanges());
    EXPECT_TRUE(std::filesystem::exists(copy));
    EXPECT_FALSE(document()->lastCanonicalSaveFailed());
}

TEST_F(DocumentCollaborationBoundaryTest, saveCopyRejectsEveryCanonicalPathAliasBeforeWriting)
{
    auto* object = document()->addObject("App::FeatureTest", "CopyAliasState");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_save_copy_alias_");
    const auto canonical = temporary.path / "canonical.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(canonical.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    object->Label.setValue("Pending changes must survive rejected aliases");
    ASSERT_TRUE(document()->hasPendingFileChanges());
    const auto pendingBefore = document()->getPendingFileChanges().toUnderlyingType();
    const auto bytesBefore = readTextFile(canonical);
    const auto timestampBefore = std::filesystem::last_write_time(canonical);
    const std::string fileNameBefore = document()->FileName.getStrValue();
    const std::string modifiedDateBefore = document()->LastModifiedDate.getStrValue();
    int starts = 0;
    int finishes = 0;
    int documentSerializations = 0;
    int globalSerializations = 0;
    auto startConnection = document()->signalStartSave.connect(
        [&](const App::Document&, const std::string&) { ++starts; });
    auto finishConnection = document()->signalFinishSave.connect(
        [&](const App::Document&, const std::string&) { ++finishes; });
    auto documentSaveConnection = document()->signalSaveDocument.connect(
        [&](Base::Writer&) { ++documentSerializations; });
    auto globalSaveConnection = App::GetApplication().signalSaveDocument.connect(
        [&](const App::Document& saved) {
            if (&saved == document()) {
                ++globalSerializations;
            }
        });

    const auto expectRejectedAlias = [&](const std::filesystem::path& alias) {
        const auto outcome = document()->saveCopyWithOutcome(alias.string().c_str());
        EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed) << alias;
        EXPECT_FALSE(outcome.fileWritten) << alias;
        EXPECT_EQ(outcome.errorCode, "COPY_TARGET_IS_CANONICAL") << alias;
        EXPECT_EQ(starts, 0) << alias;
        EXPECT_EQ(finishes, 0) << alias;
        EXPECT_EQ(documentSerializations, 0) << alias;
        EXPECT_EQ(globalSerializations, 0) << alias;
        EXPECT_EQ(document()->FileName.getStrValue(), fileNameBefore) << alias;
        EXPECT_EQ(document()->LastModifiedDate.getStrValue(), modifiedDateBefore) << alias;
        EXPECT_EQ(document()->getPendingFileChanges().toUnderlyingType(), pendingBefore)
            << alias;
        EXPECT_TRUE(document()->hasPendingFileChanges()) << alias;
        EXPECT_FALSE(document()->lastCanonicalSaveFailed()) << alias;
        EXPECT_EQ(readTextFile(canonical), bytesBefore) << alias;
        EXPECT_EQ(std::filesystem::last_write_time(canonical), timestampBefore) << alias;
    };

    expectRejectedAlias(canonical);
    expectRejectedAlias(temporary.path / "." / "canonical.FCStd");

    std::error_code relativeError;
    const auto relativeAlias = std::filesystem::relative(
        canonical, std::filesystem::current_path(), relativeError);
    if (!relativeError) {
        expectRejectedAlias(relativeAlias);
    }

    const auto symlinkAlias = temporary.path / "canonical-symlink.FCStd";
    std::error_code symlinkError;
    std::filesystem::create_symlink(canonical, symlinkAlias, symlinkError);
    if (!symlinkError) {
        expectRejectedAlias(symlinkAlias);
    }

    const auto hardlinkAlias = temporary.path / "canonical-hardlink.FCStd";
    std::error_code hardlinkError;
    std::filesystem::create_hard_link(canonical, hardlinkAlias, hardlinkError);
    if (!hardlinkError) {
        expectRejectedAlias(hardlinkAlias);
    }

#ifdef FC_OS_WIN32
    expectRejectedAlias(temporary.path / "CANONICAL.FCSTD");
#endif

    startConnection.disconnect();
    finishConnection.disconnect();
    documentSaveConnection.disconnect();
    globalSaveConnection.disconnect();
}

TEST_F(DocumentCollaborationBoundaryTest, saveCopyDoesNotDirtyDocumentsLinkedToTheCanonicalSource)
{
    ScopedTemporaryDirectory temporary("fc_save_copy_xlink_");
    ScopedApplicationDocument sourceScope("copySource");
    auto* source = sourceScope.document;
    ASSERT_NE(source, nullptr);
    auto* sourceObject = source->addObject("App::FeatureTest", "SourceObject");
    ASSERT_NE(sourceObject, nullptr);
    const auto sourcePath = temporary.path / "source.FCStd";
    ASSERT_EQ(source->saveAsWithOutcome(sourcePath.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    auto* dependentObject = document()->addObject("App::FeatureTest", "DependentObject");
    ASSERT_NE(dependentObject, nullptr);
    const auto dependentPath = temporary.path / "dependent.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(dependentPath.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    auto* link = dynamic_cast<App::PropertyXLink*>(
        dependentObject->addDynamicProperty("App::PropertyXLink", "ExternalSource"));
    ASSERT_NE(link, nullptr);
    link->setValue(sourceObject);
    ASSERT_EQ(document()->forceSave().disposition, App::DocumentSaveDisposition::Written);
    ASSERT_FALSE(document()->hasPendingFileChanges());

    sourceObject->Label.setValue("Dirty content for a noncanonical copy");
    ASSERT_TRUE(source->hasPendingFileChanges());
    int dependentStateSignals = 0;
    auto stateConnection = document()->signalFileChangeStateChanged().connect(
        [&](const App::Document&) { ++dependentStateSignals; });
    const auto copyPath = temporary.path / "source-copy.FCStd";
    const auto copyOutcome = source->saveCopyWithOutcome(copyPath.string().c_str());

    ASSERT_EQ(copyOutcome.disposition, App::DocumentSaveDisposition::CopyWritten);
    EXPECT_TRUE(copyOutcome.fileWritten);
    EXPECT_TRUE(std::filesystem::exists(copyPath));
    EXPECT_TRUE(source->hasPendingFileChanges());
    EXPECT_EQ(source->getActiveSaveIntent(), App::DocumentSaveIntent::Canonical);
    EXPECT_FALSE(document()->hasPendingFileChanges());
    EXPECT_EQ(document()->getFileChangeState(), App::DocumentFileState::Clean);
    EXPECT_EQ(dependentStateSignals, 0);

    // Canonical writes still notify linked documents about the source
    // timestamp change; only noncanonical output is filtered.
    ASSERT_EQ(source->forceSave().disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(document()->hasPendingFileChanges());
    EXPECT_GT(dependentStateSignals, 0);
    stateConnection.disconnect();
}

TEST_F(DocumentCollaborationBoundaryTest,
       legacyBackupPolicyDisabledStillUsesAtomicRetainedHandleSerialization)
{
    auto* object = document()->addObject("App::FeatureTest", "AtomicWithoutBackups");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_atomic_without_backup_policy_");
    const auto path = temporary.path / "canonical.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    const auto bytesBefore = readTextFile(path);
    object->Label.setValue("Dirty bytes that must not reach the destination");

    auto preferences = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ScopedBoolPreference disableLegacyPolicy(preferences, "BackupPolicy", false);
    auto decorator = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [](App::Internal::DocumentFileReplacementRequest& request) {
            request.testFault =
                App::Internal::DocumentFileWriterTestFault::SerializationSync;
        });
    ASSERT_TRUE(decorator);

    const auto outcome = document()->forceSave();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_FALSE(outcome.fileWritten);
    EXPECT_FALSE(outcome.durabilityVerified);
    EXPECT_EQ(outcome.errorCode, "SERIALIZATION_IO_FAILED");
    EXPECT_EQ(readTextFile(path), bytesBefore);
    EXPECT_TRUE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest,
       saveAsNoReplaceRejectsDestinationCreatedAfterSerialization)
{
    ScopedTemporaryDirectory temporary("fc_save_as_late_no_replace_");
    const auto original = temporary.path / "original.FCStd";
    const auto destination = temporary.path / "late.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(original.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    const std::string originalIdentity = document()->FileName.getStrValue();

    auto decorator = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [&](App::Internal::DocumentFileReplacementRequest& request) {
            request.beforeFinalBoundaryValidation = [&] {
                std::ofstream conflict(destination, std::ios::binary);
                conflict << "late conflicting bytes";
            };
        });
    ASSERT_TRUE(decorator);

    const auto outcome = document()->saveAsWithOutcome(destination.string().c_str(), false);

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_FALSE(outcome.fileWritten);
    EXPECT_FALSE(outcome.durabilityVerified);
    EXPECT_EQ(outcome.errorCode, "DESTINATION_EXISTS");
    EXPECT_EQ(readTextFile(destination), "late conflicting bytes");
    EXPECT_EQ(document()->FileName.getStrValue(), originalIdentity);
}

TEST_F(DocumentCollaborationBoundaryTest, saveAsCompareAndSwapAcceptsMatchingSha256)
{
    auto* object = document()->addObject("App::FeatureTest", "MatchingCas");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_save_as_matching_cas_");
    const auto original = temporary.path / "original.FCStd";
    const auto destination = temporary.path / "external.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(original.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    {
        std::ofstream external(destination, std::ios::binary);
        external << "expected external bytes";
    }
    const auto expectedHash = sha256File(destination);
    object->Label.setValue("Serialized CAS content");

    const auto outcome = document()->saveAsWithOutcome(
        destination.string().c_str(), true, expectedHash);

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_TRUE(outcome.durabilityVerified);
    EXPECT_TRUE(outcome.errorCode.empty());
    EXPECT_EQ(document()->FileName.getStrValue(), destination.string());
    EXPECT_NE(readTextFile(destination), "expected external bytes");
    EXPECT_FALSE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, saveAsCompareAndSwapRejectsMismatchingSha256)
{
    ScopedTemporaryDirectory temporary("fc_save_as_mismatching_cas_");
    const auto original = temporary.path / "original.FCStd";
    const auto destination = temporary.path / "external.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(original.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    {
        std::ofstream external(destination, std::ios::binary);
        external << "externally changed bytes";
    }
    const auto bytesBefore = readTextFile(destination);
    const std::string originalIdentity = document()->FileName.getStrValue();

    const auto outcome = document()->saveAsWithOutcome(
        destination.string().c_str(), true, std::string(64, '0'));

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_FALSE(outcome.fileWritten);
    EXPECT_FALSE(outcome.durabilityVerified);
    EXPECT_EQ(outcome.errorCode, "DESTINATION_CHANGED");
    EXPECT_EQ(readTextFile(destination), bytesBefore);
    EXPECT_EQ(document()->FileName.getStrValue(), originalIdentity);
}

TEST_F(DocumentCollaborationBoundaryTest,
       saveAsExpectedHashWithoutOverwriteUsesCanonicalFailureOverlay)
{
    ScopedTemporaryDirectory temporary("fc_save_as_hash_policy_overlay_");
    const auto original = temporary.path / "original.FCStd";
    const auto attempted = temporary.path / "attempted.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(original.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_FALSE(document()->lastCanonicalSaveFailed());

    int stateNotifications = 0;
    int outcomeNotifications = 0;
    auto stateConnection = document()->signalFileChangeStateChanged().connect(
        [&](const App::Document&) { ++stateNotifications; });
    auto outcomeConnection = document()->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome&) {
            ++outcomeNotifications;
        });

    const auto outcome = document()->saveAsWithOutcome(
        attempted.string().c_str(), false, std::string(64, 'a'));
    outcomeConnection.disconnect();
    stateConnection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_FALSE(outcome.fileWritten);
    EXPECT_FALSE(outcome.durabilityVerified);
    EXPECT_EQ(outcome.errorCode, "EXPECTED_DESTINATION_REQUIRES_OVERWRITE");
    EXPECT_TRUE(outcome.lastCanonicalSaveFailed);
    EXPECT_TRUE(document()->lastCanonicalSaveFailed());
    EXPECT_EQ(stateNotifications, 1);
    EXPECT_EQ(outcomeNotifications, 1);
    EXPECT_EQ(document()->FileName.getStrValue(), original.string());
    EXPECT_FALSE(std::filesystem::exists(attempted));
}

TEST_F(DocumentCollaborationBoundaryTest,
       saveCopyRejectsCanonicalHardlinkInstalledAtFinalBoundary)
{
    auto* object = document()->addObject("App::FeatureTest", "CopyBoundaryAlias");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_save_copy_boundary_alias_");
    const auto canonical = temporary.path / "canonical.FCStd";
    const auto destination = temporary.path / "copy.FCStd";
    const auto probe = temporary.path / "hardlink-probe";
    ASSERT_EQ(document()->saveAsWithOutcome(canonical.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    std::error_code linkError;
    std::filesystem::create_hard_link(canonical, probe, linkError);
    if (linkError) {
        GTEST_SKIP() << "hard-link creation is unavailable: " << linkError.message();
    }
    ASSERT_TRUE(std::filesystem::remove(probe));
    object->Label.setValue("Dirty content must not replace the canonical alias");
    const auto canonicalBytes = readTextFile(canonical);

    auto decorator = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [&](App::Internal::DocumentFileReplacementRequest& request) {
            request.beforeFinalBoundaryValidation = [&] {
                linkError.clear();
                std::filesystem::create_hard_link(canonical, destination, linkError);
            };
        });
    ASSERT_TRUE(decorator);

    const auto outcome = document()->saveCopyWithOutcome(destination.string().c_str());

    ASSERT_FALSE(linkError) << linkError.message();
    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_FALSE(outcome.fileWritten);
    EXPECT_EQ(outcome.errorCode, "DESTINATION_ALIASES_FORBIDDEN_FILE");
    EXPECT_EQ(readTextFile(canonical), canonicalBytes);
    EXPECT_EQ(readTextFile(destination), canonicalBytes);
    EXPECT_TRUE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest,
       postInstallDurabilityFailureReportsWrittenWithoutAdoptingSaveAsIdentity)
{
    ScopedTemporaryDirectory temporary("fc_save_as_durability_failure_");
    const auto original = temporary.path / "original.FCStd";
    const auto attempted = temporary.path / "attempted.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(original.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    const std::string originalIdentity = document()->FileName.getStrValue();
    int fileNameNotifications = 0;
    int labelNotifications = 0;
    auto changeConnection = document()->signalChanged.connect(
        [&](const App::Document& changed, const App::Property& property) {
            fileNameNotifications += &property == &changed.FileName ? 1 : 0;
            labelNotifications += &property == &changed.Label ? 1 : 0;
        });
    auto decorator = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [](App::Internal::DocumentFileReplacementRequest& request) {
            request.testFault =
                App::Internal::DocumentFileWriterTestFault::BeforeDurabilityFlush;
        });
    ASSERT_TRUE(decorator);

    const auto outcome = document()->saveAsWithOutcome(attempted.string().c_str(), true);
    changeConnection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_FALSE(outcome.durabilityVerified);
    EXPECT_EQ(outcome.errorCode, "TEST_INJECTED_DURABILITY_FAILURE");
    EXPECT_TRUE(std::filesystem::exists(attempted));
    EXPECT_EQ(document()->FileName.getStrValue(), originalIdentity);
    EXPECT_EQ(fileNameNotifications, 0);
    EXPECT_EQ(labelNotifications, 0);
    EXPECT_TRUE(document()->lastCanonicalSaveFailed());
}

TEST_F(DocumentCollaborationBoundaryTest,
       failedWrittenRecoveryWarningAllocationCannotEraseTruthOrEvidence)
{
    auto* object = document()->addObject("App::FeatureTest", "FailedWrittenRecovery");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_failed_written_recovery_");
    const auto canonical = temporary.path / "canonical.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(canonical.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    const auto previousBytes = readTextFile(canonical);
    object->Label.setValue("Installed bytes whose durability proof will fail");

    auto decorator = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [](App::Internal::DocumentFileReplacementRequest& request) {
            request.testFault =
                App::Internal::DocumentFileWriterTestFault::BeforeDurabilityFlush;
        });
    ASSERT_TRUE(decorator);
    App::Internal::setDocumentPostDurableSaveCheckpointHookForTesting(
        [](const App::Internal::DocumentPostDurableSaveCheckpoint checkpoint) {
            if (checkpoint
                == App::Internal::DocumentPostDurableSaveCheckpoint::
                    BeforeFailedReplacementRecoveryWarning) {
                throw std::bad_alloc();
            }
        });

    const auto outcome = document()->forceSave();
    App::Internal::setDocumentPostDurableSaveCheckpointHookForTesting(nullptr);

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_FALSE(outcome.durabilityVerified);
    EXPECT_EQ(outcome.errorCode, "TEST_INJECTED_DURABILITY_FAILURE");
    EXPECT_TRUE(document()->lastCanonicalSaveFailed());
    bool previousBytesRetained = false;
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path)) {
        if (entry.path() != canonical && std::filesystem::is_regular_file(entry.path())
            && readTextFile(entry.path()) == previousBytes) {
            previousBytesRetained = true;
            break;
        }
    }
    EXPECT_TRUE(previousBytesRetained)
        << "the exact previous canonical bytes must remain named recovery evidence";
}

TEST_F(DocumentCollaborationBoundaryTest,
       failedReplacementOutcomePromotionFaultPreservesWrittenTruth)
{
    auto* object = document()->addObject("App::FeatureTest", "FailedOutcomePromotion");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_failed_outcome_promotion_");
    const auto canonical = temporary.path / "canonical.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(canonical.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    object->Label.setValue("Installed bytes with an unverified durability boundary");

    auto decorator = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [](App::Internal::DocumentFileReplacementRequest& request) {
            request.testFault =
                App::Internal::DocumentFileWriterTestFault::BeforeDurabilityFlush;
        });
    ASSERT_TRUE(decorator);
    App::Internal::setDocumentPostDurableSaveCheckpointHookForTesting(
        [](const App::Internal::DocumentPostDurableSaveCheckpoint checkpoint) {
            if (checkpoint
                == App::Internal::DocumentPostDurableSaveCheckpoint::
                    BeforeFailedReplacementOutcomePromotion) {
                throw std::bad_alloc();
            }
        });

    bool observerRan = false;
    bool observerFileWritten = false;
    std::string observerError;
    auto outcomeConnection = document()->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome& observed) {
            observerRan = true;
            observerFileWritten = observed.fileWritten;
            observerError = observed.errorCode;
        });
    const auto outcome = document()->forceSave();
    outcomeConnection.disconnect();
    App::Internal::setDocumentPostDurableSaveCheckpointHookForTesting(nullptr);

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_FALSE(outcome.durabilityVerified);
    EXPECT_EQ(outcome.errorCode, "TEST_INJECTED_DURABILITY_FAILURE");
    EXPECT_TRUE(observerRan);
    EXPECT_TRUE(observerFileWritten);
    EXPECT_EQ(observerError, outcome.errorCode);
    EXPECT_FALSE(outcome.warnings.empty());
    EXPECT_TRUE(document()->lastCanonicalSaveFailed());
}

TEST_F(DocumentCollaborationBoundaryTest,
       backupMaintenanceFailureIsAWarningAfterDurableCanonicalWrite)
{
    auto* object = document()->addObject("App::FeatureTest", "BackupWarning");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_backup_warning_outcome_");
    const auto path = temporary.path / "canonical.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    const auto bytesBefore = readTextFile(path);
    object->Label.setValue("New durable content despite backup warning");

    auto preferences = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ScopedBoolPreference enablePolicy(preferences, "BackupPolicy", true);
    ScopedBoolPreference enableBackups(preferences, "CreateBackupFiles", true);
    ScopedIntPreference retainOne(preferences, "CountBackupFiles", 1);
    App::Internal::setBackupPolicyBeforeInstallHookForTesting(
        [](const std::string&) { throw std::runtime_error("injected backup failure"); });

    const auto outcome = document()->saveWithOutcome();
    App::Internal::setBackupPolicyBeforeInstallHookForTesting({});

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_TRUE(outcome.durabilityVerified);
    EXPECT_TRUE(outcome.errorCode.empty());
    EXPECT_FALSE(outcome.warnings.empty());
    EXPECT_NE(readTextFile(path), bytesBefore);
    EXPECT_FALSE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest,
       postCommitAllocationFaultCannotReclassifyDurableCanonicalWrite)
{
    auto* object = document()->addObject("App::FeatureTest", "PostCommitAllocationFault");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_post_commit_allocation_fault_");
    const auto path = temporary.path / "canonical.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    const auto bytesBefore = readTextFile(path);
    object->Label.setValue("Durable bytes despite allocation fault");

    App::Internal::setDocumentPostDurableSaveCheckpointHookForTesting(
        [](const App::Internal::DocumentPostDurableSaveCheckpoint checkpoint) {
            if (checkpoint
                == App::Internal::DocumentPostDurableSaveCheckpoint::BeforeBackupMaintenance) {
                throw std::bad_alloc();
            }
        });
    const auto outcome = document()->forceSave();
    App::Internal::setDocumentPostDurableSaveCheckpointHookForTesting(nullptr);

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_TRUE(outcome.durabilityVerified);
    EXPECT_TRUE(outcome.errorCode.empty());
    EXPECT_FALSE(outcome.warnings.empty());
    EXPECT_NE(readTextFile(path), bytesBefore);
    EXPECT_FALSE(document()->hasPendingFileChanges());
    EXPECT_FALSE(document()->lastCanonicalSaveFailed());
}

TEST_F(DocumentCollaborationBoundaryTest,
       postCommitProgrammingFaultCannotRollbackDurableSaveAsIdentity)
{
    auto* object = document()->addObject("App::FeatureTest", "PostCommitProgrammingFault");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_post_commit_programming_fault_");
    const auto original = temporary.path / "original.FCStd";
    const auto attempted = temporary.path / "adopted.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(original.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    object->Label.setValue("Durable Save As bytes despite programming fault");

    App::Internal::setDocumentPostDurableSaveCheckpointHookForTesting(
        [](const App::Internal::DocumentPostDurableSaveCheckpoint checkpoint) {
            if (checkpoint
                == App::Internal::DocumentPostDurableSaveCheckpoint::BeforeProgramVersionUpdate) {
                throw std::logic_error("injected post-commit programming fault");
            }
        });
    const auto outcome = document()->saveAsWithOutcome(attempted.string().c_str(), true);
    App::Internal::setDocumentPostDurableSaveCheckpointHookForTesting(nullptr);

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_TRUE(outcome.durabilityVerified);
    EXPECT_TRUE(outcome.errorCode.empty());
    EXPECT_FALSE(outcome.warnings.empty());
    EXPECT_EQ(outcome.canonicalPath, attempted.string());
    EXPECT_EQ(document()->FileName.getStrValue(), attempted.string());
    EXPECT_EQ(document()->Label.getStrValue(), "adopted");
    EXPECT_TRUE(std::filesystem::exists(attempted));
    EXPECT_FALSE(document()->hasPendingFileChanges());
    EXPECT_FALSE(document()->lastCanonicalSaveFailed());
}

TEST_F(DocumentCollaborationBoundaryTest,
       failedSaveAsEmitsNoProvisionalIdentityOrMetadataPropertyEvents)
{
    ScopedTemporaryDirectory temporary("fc_failed_save_as_notification_boundary_");
    const auto original = temporary.path / "first" / "original.FCStd";
    const auto attempted = temporary.path / "second" / "attempted.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(original.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    auto* tip = document()->addObject("App::FeatureTest", "PendingTip");
    ASSERT_NE(tip, nullptr);
    document()->Tip.setValue(tip);
    document()->LastModifiedDate.setValue("2000-01-01T00:00:00Z");
    document()->LastModifiedBy.setValue("Provisional author baseline");
    const std::string originalFileName = document()->FileName.getStrValue();
    const std::string originalLabel = document()->Label.getStrValue();
    const std::string originalUid = document()->Uid.getValueStr();
    const std::string originalTransientDir = document()->TransientDir.getStrValue();
    const std::string originalTipName = document()->TipName.getStrValue();
    const std::string originalModifiedDate = document()->LastModifiedDate.getStrValue();
    const std::string originalModifiedBy = document()->LastModifiedBy.getStrValue();

    int beforeEvents = 0;
    int changedEvents = 0;
    int propertyChangedEvents = 0;
    int globalRelabelNotifications = 0;
    std::vector<std::string> cachedPaths;
    auto beforeConnection = document()->signalBeforeChange.connect(
        [&](const App::Document& changed, const App::Property&) {
            ++beforeEvents;
            cachedPaths.push_back(changed.FileName.getStrValue());
        });
    auto changeConnection = document()->signalChanged.connect(
        [&](const App::Document& changed, const App::Property&) {
            ++changedEvents;
            cachedPaths.push_back(changed.FileName.getStrValue());
        });
    auto relabelConnection = App::GetApplication().signalRelabelDocument.connect(
        [&](const App::Document& changed) {
            if (&changed == document()) {
                ++globalRelabelNotifications;
            }
        });
    const auto cachePropertyChange = [&](const App::Property&) {
        ++propertyChangedEvents;
        cachedPaths.push_back(document()->FileName.getStrValue());
    };
    auto fileNamePropertyConnection =
        document()->FileName.signalChanged.connect(cachePropertyChange);
    auto labelPropertyConnection = document()->Label.signalChanged.connect(cachePropertyChange);
    auto uidPropertyConnection = document()->Uid.signalChanged.connect(cachePropertyChange);
    auto transientDirPropertyConnection =
        document()->TransientDir.signalChanged.connect(cachePropertyChange);
    auto tipNamePropertyConnection =
        document()->TipName.signalChanged.connect(cachePropertyChange);
    auto modifiedDatePropertyConnection =
        document()->LastModifiedDate.signalChanged.connect(cachePropertyChange);
    auto modifiedByPropertyConnection =
        document()->LastModifiedBy.signalChanged.connect(cachePropertyChange);
    auto decorator = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [](App::Internal::DocumentFileReplacementRequest& request) {
            request.testFault =
                App::Internal::DocumentFileWriterTestFault::SerializationSync;
        });
    ASSERT_TRUE(decorator);

    const auto outcome = document()->saveAsWithOutcome(attempted.string().c_str(), true);
    modifiedByPropertyConnection.disconnect();
    modifiedDatePropertyConnection.disconnect();
    tipNamePropertyConnection.disconnect();
    transientDirPropertyConnection.disconnect();
    uidPropertyConnection.disconnect();
    labelPropertyConnection.disconnect();
    fileNamePropertyConnection.disconnect();
    beforeConnection.disconnect();
    changeConnection.disconnect();
    relabelConnection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_EQ(outcome.errorCode, "SERIALIZATION_IO_FAILED");
    EXPECT_EQ(beforeEvents, 0);
    EXPECT_EQ(changedEvents, 0);
    EXPECT_EQ(propertyChangedEvents, 0);
    EXPECT_EQ(globalRelabelNotifications, 0);
    EXPECT_TRUE(cachedPaths.empty());
    EXPECT_EQ(document()->FileName.getStrValue(), originalFileName);
    EXPECT_EQ(document()->Label.getStrValue(), originalLabel);
    EXPECT_EQ(document()->Uid.getValueStr(), originalUid);
    EXPECT_EQ(document()->TransientDir.getStrValue(), originalTransientDir);
    EXPECT_EQ(document()->TipName.getStrValue(), originalTipName);
    EXPECT_EQ(document()->LastModifiedDate.getStrValue(), originalModifiedDate);
    EXPECT_EQ(document()->LastModifiedBy.getStrValue(), originalModifiedBy);
    EXPECT_FALSE(std::filesystem::exists(attempted));
}

TEST_F(DocumentCollaborationBoundaryTest,
       durableSaveAsReplaysAdoptionInOrderAfterCleanSavepointAndBeforeOutcome)
{
    ScopedTemporaryDirectory temporary("fc_successful_save_as_notification_boundary_");
    const auto original = temporary.path / "first" / "original.FCStd";
    const auto attempted = temporary.path / "second" / "adopted.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(original.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    auto* tip = document()->addObject("App::FeatureTest", "CommittedTip");
    ASSERT_NE(tip, nullptr);
    document()->Tip.setValue(tip);
    document()->LastModifiedDate.setValue("2000-01-01T00:00:00Z");
    document()->LastModifiedBy.setValue("Author before Save As");

    auto preferences = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ScopedBoolPreference setAuthorOnSave(preferences, "prefSetAuthorOnSave", true);
    ScopedAsciiPreference savedAuthor(preferences, "prefAuthor", "Committed Save As Author");

    std::vector<std::string> events;
    bool everyReplaySawCleanSavepoint = true;
    bool everyReplaySawInstalledTarget = true;
    const auto noteReplayBoundary = [&] {
        everyReplaySawCleanSavepoint = everyReplaySawCleanSavepoint
            && document()->getFileChangeState() == App::DocumentFileState::Clean
            && !document()->hasPendingFileChanges()
            && !document()->lastCanonicalSaveFailed();
        everyReplaySawInstalledTarget = everyReplaySawInstalledTarget
            && std::filesystem::exists(attempted);
    };
    auto changeConnection = document()->signalChanged.connect(
        [&](const App::Document& changed, const App::Property& property) {
            if (&property == &changed.FileName) {
                events.emplace_back("DocumentFileName");
            }
            else if (&property == &changed.Label) {
                events.emplace_back("DocumentLabel");
            }
            else if (&property == &changed.Uid) {
                events.emplace_back("DocumentUid");
            }
            else if (&property == &changed.TransientDir) {
                events.emplace_back("DocumentTransientDir");
            }
            else if (&property == &changed.TipName) {
                events.emplace_back("DocumentTipName");
            }
            else if (&property == &changed.LastModifiedDate) {
                events.emplace_back("DocumentLastModifiedDate");
            }
            else if (&property == &changed.LastModifiedBy) {
                events.emplace_back("DocumentLastModifiedBy");
            }
            else {
                events.emplace_back("UnexpectedDocumentProperty");
            }
            EXPECT_EQ(changed.FileName.getStrValue(), attempted.string());
            noteReplayBoundary();
        });
    auto relabelConnection = App::GetApplication().signalRelabelDocument.connect(
        [&](const App::Document& changed) {
            if (&changed == document()) {
                events.emplace_back("GlobalRelabel");
                noteReplayBoundary();
            }
        });
    const auto connectProperty = [&](App::Property& property, const char* name) {
        return property.signalChanged.connect([&, name](const App::Property&) {
            events.emplace_back(name);
            EXPECT_EQ(document()->FileName.getStrValue(), attempted.string());
            noteReplayBoundary();
        });
    };
    auto fileNamePropertyConnection =
        connectProperty(document()->FileName, "PropertyFileName");
    auto labelPropertyConnection = connectProperty(document()->Label, "PropertyLabel");
    auto uidPropertyConnection = connectProperty(document()->Uid, "PropertyUid");
    auto transientDirPropertyConnection =
        connectProperty(document()->TransientDir, "PropertyTransientDir");
    auto tipNamePropertyConnection =
        connectProperty(document()->TipName, "PropertyTipName");
    auto modifiedDatePropertyConnection =
        connectProperty(document()->LastModifiedDate, "PropertyLastModifiedDate");
    auto modifiedByPropertyConnection =
        connectProperty(document()->LastModifiedBy, "PropertyLastModifiedBy");
    auto outcomeConnection = document()->signalSaveOutcome().connect(
        [&](const App::Document& changed, const App::DocumentSaveOutcome& outcome) {
            if (&changed == document()) {
                events.emplace_back("SaveOutcome");
                EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
                noteReplayBoundary();
            }
        });

    const auto outcome = document()->saveAsWithOutcome(attempted.string().c_str(), true);
    outcomeConnection.disconnect();
    modifiedByPropertyConnection.disconnect();
    modifiedDatePropertyConnection.disconnect();
    tipNamePropertyConnection.disconnect();
    transientDirPropertyConnection.disconnect();
    uidPropertyConnection.disconnect();
    labelPropertyConnection.disconnect();
    fileNamePropertyConnection.disconnect();
    relabelConnection.disconnect();
    changeConnection.disconnect();

    const std::vector<std::string> expected {
        "DocumentFileName",
        "PropertyFileName",
        "DocumentLabel",
        "GlobalRelabel",
        "PropertyLabel",
        "DocumentUid",
        "DocumentTransientDir",
        "PropertyTransientDir",
        "DocumentTipName",
        "PropertyTipName",
        "DocumentLastModifiedDate",
        "PropertyLastModifiedDate",
        "DocumentLastModifiedBy",
        "PropertyLastModifiedBy",
        "SaveOutcome",
    };
    EXPECT_EQ(events, expected);
    EXPECT_TRUE(everyReplaySawCleanSavepoint);
    EXPECT_TRUE(everyReplaySawInstalledTarget);
    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.durabilityVerified);
    EXPECT_EQ(document()->FileName.getStrValue(), attempted.string());
    EXPECT_EQ(document()->Label.getStrValue(), "adopted");
    EXPECT_EQ(document()->TipName.getStrValue(), tip->getNameInDocument());
    EXPECT_EQ(document()->LastModifiedBy.getStrValue(), "Committed Save As Author");
    EXPECT_TRUE(std::filesystem::exists(attempted));
    EXPECT_FALSE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest,
       canonicalMetadataPropertyCallbackRunsAfterSavepointAndRemainsDirty)
{
    auto* object = document()->addObject("App::FeatureTest", "MetadataObserverMutation");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_canonical_metadata_callback_");
    const auto path = temporary.path / "canonical.FCStd";
    auto preferences = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ScopedBoolPreference setAuthorOnSave(preferences, "prefSetAuthorOnSave", true);
    ScopedAsciiPreference savedAuthor(
        preferences, "prefAuthor", "Canonical metadata callback baseline");
    ASSERT_EQ(document()->saveAsWithOutcome(path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_EQ(document()->LastModifiedBy.getStrValue(),
              "Canonical metadata callback baseline");
    preferences->SetASCII("prefAuthor", "Canonical metadata callback mutation");

    int callbackCount = 0;
    bool callbackSawCleanSavepoint = false;
    bool callbackSawInstalledTarget = false;
    auto metadataConnection = document()->LastModifiedBy.signalChanged.connect(
        [&](const App::Property&) {
            ++callbackCount;
            callbackSawCleanSavepoint = document()->getFileChangeState()
                    == App::DocumentFileState::Clean
                && !document()->hasPendingFileChanges()
                && !document()->lastCanonicalSaveFailed();
            callbackSawInstalledTarget = std::filesystem::exists(path);
            object->Label.setValue("Mutation from canonical metadata property observer");
        });
    const auto before = captureFor(object->getNameInDocument());

    const auto outcome = document()->forceSave();
    metadataConnection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.durabilityVerified);
    EXPECT_EQ(callbackCount, 1);
    EXPECT_TRUE(callbackSawCleanSavepoint);
    EXPECT_TRUE(callbackSawInstalledTarget);
    EXPECT_STREQ(object->Label.getValue(),
                 "Mutation from canonical metadata property observer");
    EXPECT_EQ(document()->LastModifiedBy.getStrValue(),
              "Canonical metadata callback mutation");
    EXPECT_TRUE(document()->hasPendingFileChanges());
    EXPECT_TRUE(
        document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    EXPECT_TRUE(outcome.pendingChanges.testFlag(App::DocumentFileChange::Model));
    expectExactDeltasAndConflicts(
        before, {0, 1, 0, 0, 0}, "canonical metadata property callback");
}

TEST_F(DocumentCollaborationBoundaryTest,
       failedCanonicalWriteEmitsNoProvisionalMetadataPropertyEvents)
{
    ScopedTemporaryDirectory temporary("fc_failed_canonical_metadata_notification_");
    const auto path = temporary.path / "canonical.FCStd";
    auto preferences = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ScopedBoolPreference setAuthorOnSave(preferences, "prefSetAuthorOnSave", true);
    ScopedAsciiPreference savedAuthor(
        preferences, "prefAuthor", "Failed canonical metadata baseline");
    ASSERT_EQ(document()->saveAsWithOutcome(path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    const auto originalBytes = readTextFile(path);
    const std::string originalTipName = document()->TipName.getStrValue();
    const std::string originalModifiedDate = document()->LastModifiedDate.getStrValue();
    const std::string originalModifiedBy = document()->LastModifiedBy.getStrValue();
    preferences->SetASCII("prefAuthor", "Provisional failed canonical author");

    int beforeEvents = 0;
    int changedEvents = 0;
    int propertyEvents = 0;
    int relabelEvents = 0;
    auto beforeConnection = document()->signalBeforeChange.connect(
        [&](const App::Document&, const App::Property&) { ++beforeEvents; });
    auto changedConnection = document()->signalChanged.connect(
        [&](const App::Document&, const App::Property&) { ++changedEvents; });
    const auto recordProperty = [&](const App::Property&) { ++propertyEvents; };
    auto tipNameConnection = document()->TipName.signalChanged.connect(recordProperty);
    auto modifiedDateConnection =
        document()->LastModifiedDate.signalChanged.connect(recordProperty);
    auto modifiedByConnection =
        document()->LastModifiedBy.signalChanged.connect(recordProperty);
    auto relabelConnection = App::GetApplication().signalRelabelDocument.connect(
        [&](const App::Document& changed) {
            if (&changed == document()) {
                ++relabelEvents;
            }
        });
    auto decorator = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [](App::Internal::DocumentFileReplacementRequest& request) {
            request.testFault =
                App::Internal::DocumentFileWriterTestFault::SerializationSync;
        });
    ASSERT_TRUE(decorator);

    const auto outcome = document()->forceSave();
    relabelConnection.disconnect();
    modifiedByConnection.disconnect();
    modifiedDateConnection.disconnect();
    tipNameConnection.disconnect();
    changedConnection.disconnect();
    beforeConnection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_EQ(outcome.errorCode, "SERIALIZATION_IO_FAILED");
    EXPECT_EQ(beforeEvents, 0);
    EXPECT_EQ(changedEvents, 0);
    EXPECT_EQ(propertyEvents, 0);
    EXPECT_EQ(relabelEvents, 0);
    EXPECT_EQ(document()->TipName.getStrValue(), originalTipName);
    EXPECT_EQ(document()->LastModifiedDate.getStrValue(), originalModifiedDate);
    EXPECT_EQ(document()->LastModifiedBy.getStrValue(), originalModifiedBy);
    EXPECT_EQ(readTextFile(path), originalBytes);
    EXPECT_FALSE(document()->hasPendingFileChanges());
    EXPECT_TRUE(document()->lastCanonicalSaveFailed());
}

TEST_F(DocumentCollaborationBoundaryTest,
       resilientSaveOutcomeRefreshesIdentityAfterThrowingLegacyAdoptionObservers)
{
    ScopedTemporaryDirectory temporary("fc_resilient_identity_adoption_");
    const auto original = temporary.path / "original.FCStd";
    const auto attempted = temporary.path / "resilient-adoption.FCStd";
    ASSERT_EQ(document()->saveAsWithOutcome(original.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    std::vector<std::string> events;
    int throwingDocumentCalls = 0;
    int laterDocumentCalls = 0;
    int throwingRelabelCalls = 0;
    int laterRelabelCalls = 0;
    int authoritativeRefreshCalls = 0;
    std::string refreshedFileName;
    std::string refreshedLabel;
    auto throwingDocumentConnection = document()->signalChanged.connect(
        [&](const App::Document&, const App::Property&) {
            ++throwingDocumentCalls;
            events.emplace_back("ThrowingDocumentObserver");
            throw Base::RuntimeError("legacy document adoption observer failure");
        });
    auto laterDocumentConnection = document()->signalChanged.connect(
        [&](const App::Document&, const App::Property&) { ++laterDocumentCalls; });
    auto throwingRelabelConnection = App::GetApplication().signalRelabelDocument.connect(
        [&](const App::Document& changed) {
            if (&changed == document()) {
                ++throwingRelabelCalls;
                events.emplace_back("ThrowingGlobalRelabelObserver");
                throw Base::RuntimeError("legacy global relabel observer failure");
            }
        });
    auto laterRelabelConnection = App::GetApplication().signalRelabelDocument.connect(
        [&](const App::Document& changed) {
            if (&changed == document()) {
                ++laterRelabelCalls;
            }
        });
    auto throwingOutcomeConnection = document()->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome&) {
            events.emplace_back("ThrowingOutcomeObserver");
            throw Base::RuntimeError("outcome observer failure before core refresh");
        });
    auto authoritativeOutcomeConnection = document()->signalSaveOutcome().connect(
        [&](const App::Document& changed, const App::DocumentSaveOutcome& outcome) {
            ++authoritativeRefreshCalls;
            refreshedFileName = changed.FileName.getStrValue();
            refreshedLabel = changed.Label.getStrValue();
            EXPECT_EQ(outcome.intent, App::DocumentSaveIntent::SaveAs);
            EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
            EXPECT_TRUE(outcome.durabilityVerified);
            EXPECT_FALSE(changed.hasPendingFileChanges());
            EXPECT_TRUE(std::filesystem::exists(attempted));
            events.emplace_back("AuthoritativeOutcomeRefresh");
        });

    const auto outcome = document()->saveAsWithOutcome(attempted.string().c_str(), true);
    authoritativeOutcomeConnection.disconnect();
    throwingOutcomeConnection.disconnect();
    laterRelabelConnection.disconnect();
    throwingRelabelConnection.disconnect();
    laterDocumentConnection.disconnect();
    throwingDocumentConnection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_GT(throwingDocumentCalls, 0);
    EXPECT_EQ(laterDocumentCalls, 0);
    EXPECT_EQ(throwingRelabelCalls, 1);
    EXPECT_EQ(laterRelabelCalls, 0);
    EXPECT_EQ(authoritativeRefreshCalls, 1);
    EXPECT_EQ(refreshedFileName, attempted.string());
    EXPECT_EQ(refreshedLabel, "resilient-adoption");
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back(), "AuthoritativeOutcomeRefresh");
}

TEST_F(DocumentCollaborationBoundaryTest, failedSaveAsRestoresIdentityAndPendingState)
{
    auto* object = document()->addObject("App::FeatureTest", "FailedSaveAs");
    ASSERT_NE(object, nullptr);
    const std::string originalLabel = document()->Label.getStrValue();
    ScopedTemporaryDirectory temporary("fc_failed_save_as_");
    const auto blockedParent = temporary.path / "not-a-directory";
    {
        std::ofstream blocker(blockedParent, std::ios::binary);
        blocker << "blocks directory creation";
    }
    const auto invalidDestination = blockedParent / "destination.FCStd";

    int outcomes = 0;
    auto connection = document()->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome&) { ++outcomes; });
    const auto outcome = document()->saveAsWithOutcome(
        invalidDestination.string().c_str(), true);
    connection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_FALSE(outcome.fileWritten);
    EXPECT_TRUE(document()->FileName.getStrValue().empty());
    EXPECT_EQ(document()->Label.getStrValue(), originalLabel);
    EXPECT_TRUE(document()->hasPendingFileChanges());
    EXPECT_EQ(document()->getFileChangeState(), App::DocumentFileState::NotSaved);
    EXPECT_TRUE(document()->lastCanonicalSaveFailed());
    EXPECT_EQ(outcomes, 1);
}

TEST_F(DocumentCollaborationBoundaryTest, structuredMissingPathReportsOneFailedOutcome)
{
    int outcomes = 0;
    auto connection = document()->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome&) { ++outcomes; });
    const auto outcome = document()->saveWithOutcome();
    connection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_EQ(outcome.errorCode, "MISSING_CANONICAL_PATH");
    EXPECT_EQ(outcomes, 1);
}

TEST_F(DocumentCollaborationBoundaryTest, transactionSavepointsFollowUndoRedoAndAbort)
{
    auto* object = document()->addObject("App::FeatureTest", "UndoSavepoint");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_undo_savepoint_");
    const auto path = (temporary.path / "undo.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_FALSE(document()->hasPendingFileChanges());

    document()->openTransaction("dirty after savepoint");
    object->Label.setValue("After savepoint");
    document()->commitTransaction();
    ASSERT_TRUE(document()->hasPendingFileChanges());
    EXPECT_TRUE(document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));

    ASSERT_TRUE(document()->undo());
    EXPECT_FALSE(document()->hasPendingFileChanges());
    EXPECT_EQ(document()->getFileChangeState(), App::DocumentFileState::Clean);

    ASSERT_TRUE(document()->redo());
    EXPECT_TRUE(document()->hasPendingFileChanges());

    ASSERT_TRUE(document()->undo());
    document()->openTransaction("aborted change");
    object->Label.setValue("Abort me");
    ASSERT_TRUE(document()->hasPendingFileChanges());
    document()->abortTransaction();
    EXPECT_FALSE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, undoPreservesLaterNonTransactionalCategoryChanges)
{
    auto* object = document()->addObject("App::FeatureTest", "InterleavedSavepoint");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_interleaved_savepoint_");
    const auto path = (temporary.path / "interleaved.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    document()->openTransaction("model transaction");
    object->Label.setValue("Transactional model change");
    document()->commitTransaction();
    document()->markFileChange(App::DocumentFileChange::Appearance);
    ASSERT_TRUE(
        document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));

    ASSERT_TRUE(document()->undo());
    EXPECT_FALSE(document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    EXPECT_TRUE(
        document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    EXPECT_TRUE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, undoPreservesStickyChangeInTheSameCategory)
{
    auto* object = document()->addObject("App::FeatureTest", "ComposedSavepoint");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_composed_savepoint_");
    const auto path = (temporary.path / "composed.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    document()->openTransaction("transactional model change");
    object->Label.setValue("transactional");
    document()->commitTransaction();

    // Compose the transaction with a later non-transactional change, return
    // through redo, and establish a new canonical savepoint. Undoing the old
    // transaction must still move away from that savepoint.
    ASSERT_TRUE(document()->undo());
    document()->markFileChange(
        App::DocumentFileChange::Model, App::DocumentFileChangeOwnership::Sticky);
    ASSERT_TRUE(document()->redo());
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_FALSE(document()->hasPendingFileChanges());

    ASSERT_TRUE(document()->undo());
    EXPECT_TRUE(document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    EXPECT_TRUE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, abortPreservesStickyChangeDuringActiveTransaction)
{
    auto* object = document()->addObject("App::FeatureTest", "ActiveSticky");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_active_sticky_");
    const auto path = (temporary.path / "active-sticky.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    document()->openTransaction("transaction with external sticky change");
    object->Label.setValue("transactional");
    document()->markFileChange(
        App::DocumentFileChange::Model, App::DocumentFileChangeOwnership::Sticky);
    document()->abortTransaction();

    EXPECT_TRUE(document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    EXPECT_TRUE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, persistentMutationSignalRepeatsWithinOneDirtyCategory)
{
    auto* object = document()->addObject("App::FeatureTest", "RepeatedDirtySignal");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_repeated_dirty_signal_");
    const auto path = (temporary.path / "signals.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    int notifications = 0;
    auto connection = document()->signalFileChangeStateChanged().connect(
        [&](const App::Document&) { ++notifications; });
    object->Label.setValue("First dirty value");
    object->Label.setValue("Second dirty value");
    connection.disconnect();

    EXPECT_GE(notifications, 2)
        << "autosave consumers need a signal for later Model-to-Model mutations";
    EXPECT_TRUE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, compatibilityClearCannotEraseTrackedChanges)
{
    auto* object = document()->addObject("App::FeatureTest", "CompatibilityDirty" );
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_compatibility_dirty_");
    const auto path = (temporary.path / "compatibility.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    document()->setCompatibilityFileModified(true);
    ASSERT_TRUE(document()->hasPendingFileChanges());
    document()->setCompatibilityFileModified(false);
    EXPECT_FALSE(document()->hasPendingFileChanges());

    object->Label.setValue("Tracked model change");
    ASSERT_TRUE(document()->hasPendingFileChanges());
    document()->setCompatibilityFileModified(false);
    EXPECT_TRUE(document()->hasPendingFileChanges())
        << "legacy modified=false may clear only its compatibility marker";
}

TEST_F(DocumentCollaborationBoundaryTest, netCleanCompatibilityTransactionStaysCleanAcrossHistory)
{
    auto* object = document()->addObject("App::FeatureTest", "CompatibilityHistory");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_compatibility_net_clean_");
    const auto path = (temporary.path / "compatibility-net-clean.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    document()->openTransaction("net clean compatibility marker");
    object->Label.setValue("Compatibility history changed");
    document()->setCompatibilityFileModified(true);
    document()->setCompatibilityFileModified(false);
    document()->commitTransaction();
    ASSERT_TRUE(document()->hasPendingFileChanges());
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(document()->undo());
    EXPECT_TRUE(document()->hasPendingFileChanges());
    ASSERT_TRUE(document()->redo());
    EXPECT_FALSE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, propertyStatusTrackingHonorsPersistenceFlags)
{
    auto* object = document()->addObject("App::FeatureTest", "StatusTracking");
    ASSERT_NE(object, nullptr);
    auto* property = object->addDynamicProperty("App::PropertyString", "TrackedStatus");
    ASSERT_NE(property, nullptr);
    ScopedTemporaryDirectory temporary("fc_property_status_tracking_");
    const auto path = (temporary.path / "status.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    property->setStatus(App::Property::NoModify, true);
    EXPECT_FALSE(document()->hasPendingFileChanges());

    property->setStatus(App::Property::NoModify, false);
    EXPECT_FALSE(document()->hasPendingFileChanges());

    property->setStatus(App::Property::Hidden, true);
    EXPECT_TRUE(document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
}

TEST_F(DocumentCollaborationBoundaryTest, transientStatusTransitionsTriggerCanonicalWrites)
{
    auto* object = document()->addObject("App::FeatureTest", "TransientStatusTracking");
    ASSERT_NE(object, nullptr);
    auto* persistent = object->addDynamicProperty(
        "App::PropertyString", "BecomesTransient");
    auto* dynamicTransient = object->addDynamicProperty(
        "App::PropertyString", "TransientSchema", nullptr, nullptr, App::Prop_Transient);
    ASSERT_NE(persistent, nullptr);
    ASSERT_NE(dynamicTransient, nullptr);
    static_cast<App::PropertyString*>(persistent)->setValue("persistent value");
    static_cast<App::PropertyString*>(dynamicTransient)->setValue("transient value");

    ScopedTemporaryDirectory temporary("fc_transient_status_");
    const auto path = (temporary.path / "transient-status.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    persistent->setStatus(App::Property::Transient, true);
    ASSERT_TRUE(document()->hasPendingFileChanges());
    EXPECT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    dynamicTransient->setStatus(App::Property::Hidden, true);
    ASSERT_TRUE(document()->hasPendingFileChanges())
        << "dynamic transient schema/status remains serialized";
    EXPECT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);
}

TEST_F(DocumentCollaborationBoundaryTest,
       staticTransientPropertyStatusTransitionTriggersCanonicalWrite)
{
    auto* object = document()->addObject("App::FeatureTest", "StaticTransientStatusTracking");
    ASSERT_NE(object, nullptr);
    auto* property = object->getPropertyByName("TypeTransient");
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(property->testStatus(App::Property::PropTransient));

    ScopedTemporaryDirectory temporary("fc_static_transient_status_");
    const auto path = (temporary.path / "static-transient-status.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    property->setStatus(App::Property::Hidden, true);
    ASSERT_TRUE(document()->hasPendingFileChanges())
        << "serialized Hidden status on a static Prop_Transient property must be tracked";
    EXPECT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);
}

TEST_F(DocumentCollaborationBoundaryTest, documentDynamicSchemaChangesArePersistent)
{
    ScopedTemporaryDirectory temporary("fc_document_schema_");
    const auto path = (temporary.path / "document-schema.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    auto* property = document()->addDynamicProperty("App::PropertyString", "DocumentSchema");
    ASSERT_NE(property, nullptr);
    EXPECT_TRUE(document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(document()->removeDynamicProperty("DocumentSchema"));
    EXPECT_TRUE(document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
}

TEST_F(DocumentCollaborationBoundaryTest,
       transientDynamicSchemaTracksStructureButNotValueAndNoPersistStaysClean)
{
    auto* object = document()->addObject("App::FeatureTest", "TransientDynamicSchema");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_transient_dynamic_schema_");
    const auto path = (temporary.path / "transient-dynamic-schema.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    auto* property = dynamic_cast<App::PropertyString*>(object->addDynamicProperty(
        "App::PropertyString", "TransientSchema", "Original group", "Original documentation",
        App::Prop_Transient));
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    property->setValue("transient value only");
    EXPECT_FALSE(document()->hasPendingFileChanges());

    ASSERT_TRUE(object->renameDynamicProperty(property, "RenamedTransientSchema"));
    ASSERT_TRUE(document()->hasPendingFileChanges());
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(object->changeDynamicProperty(
        property, "Renamed group", "Renamed documentation"));
    ASSERT_TRUE(document()->hasPendingFileChanges());
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(object->changeDynamicProperty(
        property, "Renamed group", "Renamed documentation"));
    EXPECT_FALSE(document()->hasPendingFileChanges());
    ASSERT_TRUE(object->changeDynamicProperty(property, nullptr, nullptr));
    EXPECT_FALSE(document()->hasPendingFileChanges());

    document()->openTransaction("dynamic metadata is not undo payload");
    ASSERT_TRUE(object->changeDynamicProperty(
        property, "Aborted group", "Aborted documentation"));
    document()->abortTransaction();
    EXPECT_STREQ(object->getPropertyGroup(property), "Aborted group");
    EXPECT_STREQ(object->getPropertyDocumentation(property), "Aborted documentation");
    EXPECT_TRUE(document()->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    EXPECT_TRUE(document()->hasPendingFileChanges());

    ASSERT_TRUE(object->removeDynamicProperty("RenamedTransientSchema"));
    ASSERT_TRUE(document()->hasPendingFileChanges());
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    auto* noPersist = object->addDynamicProperty(
        "App::PropertyString", "NoPersistSchema", "Group", "Documentation", App::Prop_NoPersist);
    ASSERT_NE(noPersist, nullptr);
    EXPECT_FALSE(document()->hasPendingFileChanges());
    ASSERT_TRUE(object->renameDynamicProperty(noPersist, "RenamedNoPersistSchema"));
    ASSERT_TRUE(object->changeDynamicProperty(
        noPersist, "Changed group", "Changed documentation"));
    ASSERT_TRUE(object->removeDynamicProperty("RenamedNoPersistSchema"));
    EXPECT_FALSE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, documentTransientDynamicSchemaIsFileRelevant)
{
    ScopedTemporaryDirectory temporary("fc_document_transient_schema_");
    const auto path = (temporary.path / "document-transient-schema.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    auto* property = document()->addDynamicProperty(
        "App::PropertyString", "DocumentTransientSchema", "Group", "Documentation",
        App::Prop_Transient);
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(document()->hasPendingFileChanges());
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(document()->renameDynamicProperty(property, "RenamedDocumentTransientSchema"));
    ASSERT_TRUE(document()->hasPendingFileChanges());
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(document()->changeDynamicProperty(
        property, "Changed group", "Changed documentation"));
    ASSERT_TRUE(document()->hasPendingFileChanges());
    ASSERT_EQ(document()->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(document()->removeDynamicProperty("RenamedDocumentTransientSchema"));
    EXPECT_TRUE(document()->hasPendingFileChanges());
}

TEST_F(DocumentCollaborationBoundaryTest, mutationAfterSerializationRemainsPending)
{
    auto* object = document()->addObject("App::FeatureTest", "ReentrantSave");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_reentrant_save_");
    const auto path = (temporary.path / "reentrant.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    auto connection = document()->signalFinishSave.connect(
        [&](const App::Document&, const std::string&) {
            object->Label.setValue("Changed after serialization");
        });
    const auto outcome = document()->forceSave();
    connection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_TRUE(document()->hasPendingFileChanges());
    EXPECT_TRUE(outcome.pendingChanges.testFlag(App::DocumentFileChange::Model));
}

TEST_F(DocumentCollaborationBoundaryTest, throwingFinishObserverDoesNotReclassifyDurableSave)
{
    auto* object = document()->addObject("App::FeatureTest", "ThrowingFinish");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_throwing_finish_");
    const auto path = (temporary.path / "throwing-finish.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    auto connection = document()->signalFinishSave.connect(
        [&](const App::Document&, const std::string&) {
            object->Label.setValue("Changed by finish observer");
            throw Base::RuntimeError("finish observer failure");
        });
    const auto outcome = document()->forceSave();
    connection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_TRUE(document()->hasPendingFileChanges());
    EXPECT_TRUE(outcome.pendingChanges.testFlag(App::DocumentFileChange::Model));
}

TEST_F(DocumentCollaborationBoundaryTest, throwingOutcomeObserverDoesNotEscapeStructuredSave)
{
    auto* object = document()->addObject("App::FeatureTest", "ThrowingOutcome");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_throwing_outcome_");
    const auto path = (temporary.path / "throwing-outcome.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(path.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    object->Label.setValue("Dirty before observer failure");
    auto outcomeConnection = document()->signalSaveOutcome().connect(
        [](const App::Document&, const App::DocumentSaveOutcome&) {
            throw Base::RuntimeError("outcome observer failure");
        });
    auto outcomeCloneFailureArmed = std::make_shared<std::atomic<bool>>(false);
    auto outcomeCloneFailure = document()->signalSaveOutcome().underlying().connect(
        ThrowOnArmedOutcomeCopy(outcomeCloneFailureArmed));
    auto stateConnection = document()->signalFileChangeStateChanged().connect(
        [](const App::Document&) {
            throw Base::RuntimeError("file-state observer failure");
        });
    int laterStateObservers = 0;
    int laterOutcomeObservers = 0;
    auto laterStateConnection = document()->signalFileChangeStateChanged().connect(
        [&](const App::Document&) { ++laterStateObservers; });
    auto laterOutcomeConnection = document()->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome&) {
            ++laterOutcomeObservers;
        });
    outcomeCloneFailureArmed->store(true, std::memory_order_release);
    const auto outcome = document()->saveWithOutcome();
    laterOutcomeConnection.disconnect();
    laterStateConnection.disconnect();
    stateConnection.disconnect();
    outcomeCloneFailure.disconnect();
    outcomeConnection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_FALSE(document()->hasPendingFileChanges());
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(laterStateObservers, 0)
        << "a throwing extension must not starve later App/Gui/autosave observers";
    EXPECT_EQ(laterOutcomeObservers, 1);
}

TEST_F(DocumentCollaborationBoundaryTest,
       nonStandardSaveAsObserverFailureRestoresCompleteDocumentIdentity)
{
    ScopedTemporaryDirectory temporary("fc_nonstd_save_as_");
    const auto originalPath = (temporary.path / "original.FCStd").string();
    const auto attemptedPath = (temporary.path / "attempted.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(originalPath.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    const std::string originalFileName = document()->FileName.getStrValue();
    const std::string originalLabel = document()->Label.getStrValue();
    const std::string originalUid = document()->Uid.getValueStr();
    const std::string originalTransientDir = document()->TransientDir.getStrValue();
    const std::string originalTipName = document()->TipName.getStrValue();
    const std::string originalModifiedDate = document()->LastModifiedDate.getStrValue();
    const std::string originalModifiedBy = document()->LastModifiedBy.getStrValue();
    int outcomes = 0;
    auto outcomeConnection = document()->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome&) { ++outcomes; });
    auto startConnection = document()->signalStartSave.connect(
        [](const App::Document&, const std::string&) { throw 17; });

    const auto outcome = document()->saveAsWithOutcome(attemptedPath.c_str(), true);
    startConnection.disconnect();
    outcomeConnection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_FALSE(outcome.fileWritten);
    EXPECT_EQ(outcome.errorCode, "FILE_WRITE_FAILED");
    EXPECT_EQ(outcomes, 1);
    EXPECT_EQ(document()->FileName.getStrValue(), originalFileName);
    EXPECT_EQ(document()->Label.getStrValue(), originalLabel);
    EXPECT_EQ(document()->Uid.getValueStr(), originalUid);
    EXPECT_EQ(document()->TransientDir.getStrValue(), originalTransientDir);
    EXPECT_EQ(document()->TipName.getStrValue(), originalTipName);
    EXPECT_EQ(document()->LastModifiedDate.getStrValue(), originalModifiedDate);
    EXPECT_EQ(document()->LastModifiedBy.getStrValue(), originalModifiedBy);
    EXPECT_EQ(outcome.canonicalPath, originalFileName);
    EXPECT_EQ(outcome.targetPath, attemptedPath);
    EXPECT_FALSE(std::filesystem::exists(attemptedPath));
}

TEST_F(DocumentCollaborationBoundaryTest,
       saveAsRollbackSuppressesThrowingIdentityObserverAndRestoresTransientDirectory)
{
    ScopedTemporaryDirectory temporary("fc_save_as_rollback_observer_");
    const auto originalPath = (temporary.path / "original.FCStd").string();
    const auto attemptedPath = (temporary.path / "attempted.FCStd").string();
    ASSERT_EQ(document()->saveAsWithOutcome(originalPath.c_str()).disposition,
              App::DocumentSaveDisposition::Written);

    const std::string originalLabel = document()->Label.getStrValue();
    const std::string originalUid = document()->Uid.getValueStr();
    const std::string originalTransientDir = document()->TransientDir.getStrValue();
    const auto sentinel = std::filesystem::path(originalTransientDir) / "rollback-sentinel";
    const std::string originalTipName = document()->TipName.getStrValue();
    const std::string originalModifiedDate = document()->LastModifiedDate.getStrValue();
    const std::string originalModifiedBy = document()->LastModifiedBy.getStrValue();
    {
        std::ofstream marker(sentinel, std::ios::binary);
        marker << "must return to original transient directory";
    }

    int observedRestorationNotifications = 0;
    auto changeConnection = document()->signalChanged.connect(
        [&](const App::Document& changed, const App::Property&) {
            if (changed.FileName.getStrValue() == originalPath) {
                ++observedRestorationNotifications;
                throw Base::RuntimeError("identity restoration observer must be suppressed");
            }
        });
    auto startConnection = document()->signalStartSave.connect(
        [](const App::Document&, const std::string&) { throw Base::RuntimeError("write failure"); });

    const auto outcome = document()->saveAsWithOutcome(attemptedPath.c_str(), true);
    startConnection.disconnect();
    changeConnection.disconnect();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_EQ(outcome.errorCode, "FILE_WRITE_FAILED");
    EXPECT_EQ(observedRestorationNotifications, 0);
    EXPECT_EQ(document()->FileName.getStrValue(), originalPath);
    EXPECT_EQ(document()->Label.getStrValue(), originalLabel);
    EXPECT_EQ(document()->Uid.getValueStr(), originalUid);
    EXPECT_EQ(document()->TransientDir.getStrValue(), originalTransientDir);
    EXPECT_EQ(document()->TipName.getStrValue(), originalTipName);
    EXPECT_EQ(document()->LastModifiedDate.getStrValue(), originalModifiedDate);
    EXPECT_EQ(document()->LastModifiedBy.getStrValue(), originalModifiedBy);
    EXPECT_TRUE(std::filesystem::exists(sentinel));
    EXPECT_FALSE(std::filesystem::exists(attemptedPath));
    EXPECT_FALSE(document()->hasPendingFileChanges());
    EXPECT_EQ(document()->getFileChangeState(), App::DocumentFileState::Clean);
    EXPECT_TRUE(document()->lastCanonicalSaveFailed());
}

TEST_F(DocumentCollaborationBoundaryTest, nativeSaveAsPolicyDoesNotClobberByDefault)
{
    ScopedTemporaryDirectory temporary("fc_native_save_as_policy_");
    const auto occupiedPath = (temporary.path / "occupied.FCStd").string();
    const auto availablePath = (temporary.path / "available.FCStd").string();
    {
        std::ofstream occupied(occupiedPath, std::ios::binary);
        occupied << "preserve existing destination";
    }

    EXPECT_EQ(document()->saveAsWithPolicy(occupiedPath.c_str(), false),
              App::DocumentSaveAsStatus::DestinationExists);
    EXPECT_TRUE(document()->FileName.getStrValue().empty());
    EXPECT_EQ(document()->saveAsWithPolicy(availablePath.c_str(), false),
              App::DocumentSaveAsStatus::Saved);
    EXPECT_EQ(document()->FileName.getStrValue(), availablePath);
    EXPECT_EQ(document()->saveAsWithPolicy(availablePath.c_str(), false),
              App::DocumentSaveAsStatus::Saved)
        << "saving the document's current path is not an overwrite conflict";
}

TEST_F(DocumentCollaborationBoundaryTest, saveObserverMutationIsNotSuppressed)
{
    auto* object = document()->addObject("App::FeatureTest", "SaveObserverIngress");
    ASSERT_NE(object, nullptr);
    ScopedTemporaryDirectory temporary("fc_collaboration_save_observer_");
    const auto path = (temporary.path / "observer.FCStd").string();
    ASSERT_TRUE(document()->saveAs(path.c_str()));

    int observerCalls = 0;
    auto connection = document()->signalStartSave.connect(
        [&](const App::Document&, const std::string&) {
            ++observerCalls;
            object->Label.setValue("ChangedBySaveObserver");
        }
    );
    const auto before = captureFor(object->getNameInDocument());
    ASSERT_EQ(document()->forceSave().disposition, App::DocumentSaveDisposition::Written);
    connection.disconnect();

    EXPECT_EQ(observerCalls, 1);
    EXPECT_STREQ(object->Label.getValue(), "ChangedBySaveObserver");
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 0}, "save start observer");
}

TEST_F(DocumentCollaborationBoundaryTest, documentTipPublishesEveryMutation)
{
    auto* object = document()->addObject("App::FeatureTest", "DocumentTipIngress");
    ASSERT_NE(object, nullptr);

    auto before = captureFor(object->getNameInDocument());
    document()->Tip.setValue(object);
    expectExactDeltasAndConflicts(before, {0, 0, 0, 0, 1}, "Document::Tip");

    before = captureFor(object->getNameInDocument());
    EXPECT_NO_THROW(document()->Tip.setValue(nullptr));
    EXPECT_EQ(document()->Tip.getValue(), nullptr);
    expectExactDeltasAndConflicts(before, {0, 0, 0, 0, 1}, "Document::Tip clear");
}

TEST_F(DocumentCollaborationBoundaryTest, clearDocumentPublishesRemovals)
{
    auto* object = document()->addObject("App::FeatureTest", "ClearDocumentIngress");
    ASSERT_NE(object, nullptr);
    const std::string objectName = object->getNameInDocument();
    const std::string firstIncarnation = document()->collaborationObjectIdentity(*object);
    const auto keys = dependencyKeysFor(objectName);

    auto before = revisions().capture(keys);
    EXPECT_NO_THROW(document()->clearDocument());
    EXPECT_EQ(document()->getObject(objectName.c_str()), nullptr);
    expectExactDeltasAndConflicts(
        before,
        {1, 0, 1, 1, 1},
        "Document::clearDocument"
    );

    auto* replacement = document()->addObject("App::FeatureTest", objectName.c_str());
    ASSERT_NE(replacement, nullptr);
    EXPECT_NE(document()->collaborationObjectIdentity(*replacement), firstIncarnation);
}

TEST_F(DocumentCollaborationBoundaryTest, addEntryPointsAttachAndPublishWithoutExternalCapability)
{
    auto before = captureFor("CapabilityFreeTypedObject");
    auto* typed = document()->addObject("App::FeatureTest", "CapabilityFreeTypedObject");
    ASSERT_NE(typed, nullptr);
    expectExactDeltasAndConflicts(before, {1, 0, 0, 1, 0}, "typed add entry point");

    before = captureFor("CapabilityFreeBatchObject");
    const auto batch =
        document()->addObjects("App::FeatureTest", {"CapabilityFreeBatchObject"});
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(document()->getObject("CapabilityFreeBatchObject"), batch.front());
    expectExactDeltasAndConflicts(before, {1, 0, 0, 1, 0}, "batch add entry point");

    auto external = std::make_unique<App::FeatureTest>();
    auto* externalRaw = external.get();
    before = captureFor("CapabilityFreeExternalObject");
    EXPECT_NO_THROW(document()->addObject(externalRaw, "CapabilityFreeExternalObject"));
    external.release();
    EXPECT_EQ(externalRaw->getDocument(), document());
    EXPECT_EQ(document()->getObject("CapabilityFreeExternalObject"), externalRaw);
    expectExactDeltasAndConflicts(before, {1, 0, 0, 1, 0}, "external add entry point");
}

TEST_F(DocumentCollaborationBoundaryTest, throwingAddObserverStillPublishesSurvivingObject)
{
    const std::string objectName = "ThrowingAddObserverIngress";
    auto connection = document()->signalChangedObject.connect(
        [&](const App::DocumentObject& changedObject, const App::Property& changedProperty) {
            if (std::string_view(changedObject.getNameInDocument()) == objectName
                && std::string_view(changedProperty.getName()) == "Label") {
                throw Base::RuntimeError("intentional add observer failure");
            }
        }
    );
    const auto before = captureFor(objectName);
    EXPECT_THROW(
        document()->addObject("App::FeatureTest", objectName.c_str()),
        Base::RuntimeError
    );
    connection.disconnect();

    auto* surviving = document()->getObject(objectName.c_str());
    ASSERT_NE(surviving, nullptr);
    EXPECT_FALSE(document()->collaborationObjectIdentity(*surviving).empty());
    expectExactDeltasAndConflicts(
        before,
        {1, 0, 0, 1, 0},
        "throwing add observer surviving state"
    );
}

TEST_F(DocumentCollaborationBoundaryTest, throwingDeleteObserverPublishesSurvivingPartialState)
{
    const std::string objectName = "ThrowingDeleteObserverIngress";
    auto* object = document()->addObject("App::FeatureTest", objectName.c_str());
    ASSERT_NE(object, nullptr);
    auto connection = document()->signalDeletedObject.connect(
        [&](const App::DocumentObject& deleting) {
            if (&deleting == object) {
                throw Base::RuntimeError("intentional delete observer failure");
            }
        }
    );
    const auto before = captureFor(objectName);
    EXPECT_THROW(document()->removeObject(objectName.c_str()), Base::RuntimeError);
    connection.disconnect();

    EXPECT_EQ(document()->getObject(objectName.c_str()), object);
    expectExactDeltasAndConflicts(
        before,
        {1, 0, 0, 1, 0},
        "throwing delete observer surviving state"
    );
}

TEST_F(DocumentCollaborationBoundaryTest, throwingTipObserverPublishesLateRemovalBoundaryOnce)
{
    const std::string objectName = "ThrowingTipObserverIngress";
    auto* object = document()->addObject("App::FeatureTest", objectName.c_str());
    ASSERT_NE(object, nullptr);
    document()->Tip.setValue(object);

    auto connection = document()->signalChanged.connect(
        [&](const App::Document&, const App::Property& changedProperty) {
            if (&changedProperty == &document()->Tip) {
                throw Base::RuntimeError("intentional late Tip observer failure");
            }
        }
    );
    const auto before = captureFor(objectName);
    EXPECT_THROW(document()->removeObject(objectName.c_str()), Base::RuntimeError);
    connection.disconnect();

    EXPECT_EQ(document()->getObject(objectName.c_str()), object);
    EXPECT_EQ(document()->Tip.getValue(), nullptr);
    expectExactDeltasAndConflicts(
        before,
        {1, 0, 0, 1, 0},
        "late throwing Tip observer removal state"
    );
}

TEST_F(DocumentCollaborationBoundaryTest, applicationCloseCrossesTheLifecycleRegistryBoundary)
{
    App::DocumentInitFlags flags;
    flags.createView = false;
    const std::string name = App::GetApplication().getUniqueDocumentName("collaborationClose");
    auto* closing = App::GetApplication().newDocument(name.c_str(), "collaborationClose", flags);
    ASSERT_NE(closing, nullptr);
    const auto liveIdentity = closing->collaborationIdentity();
    ASSERT_EQ(liveIdentity.state, App::DocumentLifecycleState::Live);

    bool observedClosingIdentity = false;
    auto connection = App::GetApplication().signalDeleteDocument.connect(
        [&](const App::Document& observed) {
            if (&observed != closing) {
                return;
            }
            observedClosingIdentity = true;
            const auto registryIdentity = observed.collaborationIdentity();
            const auto revisionIdentity = observed.collaborationRevisions().documentIdentity();
            ASSERT_TRUE(revisionIdentity.has_value());
            EXPECT_EQ(revisionIdentity->documentInstanceId, registryIdentity.instanceId);
            EXPECT_EQ(revisionIdentity->lifecycleEpoch, registryIdentity.lifecycleEpoch);
            EXPECT_EQ(registryIdentity.state, App::DocumentLifecycleState::Closing);
        }
    );

    ASSERT_TRUE(App::GetApplication().closeDocument(name.c_str()));
    connection.disconnect();
    EXPECT_TRUE(observedClosingIdentity);

    const auto tombstone = App::GetApplication().collaborationRegistry().identity(
        liveIdentity.instanceId
    );
    ASSERT_TRUE(tombstone.has_value());
    EXPECT_EQ(tombstone->state, App::DocumentLifecycleState::Closed);
    EXPECT_NE(tombstone->lifecycleEpoch, liveIdentity.lifecycleEpoch);
    EXPECT_EQ(
        App::GetApplication().collaborationRegistry().validate(
            tombstone->instanceId,
            tombstone->lifecycleEpoch
        ),
        App::DocumentIdentityValidation::NotLive
    );
}

TEST_F(DocumentCollaborationBoundaryTest,
       resilientStabilizationPreservesHistoricalBaseCopySemantics)
{
    App::ResilientSignal<void(const App::Document&)> signal;
    auto& historicalBase =
        static_cast<fastsignals::signal<void(const App::Document&)>&>(signal);
    auto copyFailureArmed = std::make_shared<std::atomic<bool>>(false);
    auto connection = historicalBase.connect(ThrowOnArmedCopy(copyFailureArmed));

    EXPECT_NO_THROW(signal(*document()));
    copyFailureArmed->store(true, std::memory_order_release);
    EXPECT_THROW(historicalBase(*document()), std::bad_alloc);
    connection.disconnect();
}

TEST_F(DocumentCollaborationBoundaryTest,
       throwingEarlyAndLateLifecycleObserversCannotInterruptCloseCleanup)
{
    App::DocumentInitFlags flags;
    flags.createView = false;
    const std::string name =
        App::GetApplication().getUniqueDocumentName("resilientCollaborationClose");
    auto* closing = App::GetApplication().newDocument(
        name.c_str(), "resilientCollaborationClose", flags);
    ASSERT_NE(closing, nullptr);
    const auto liveIdentity = closing->collaborationIdentity();
    bool destructionObserved = false;
    auto* destructionProbe = new CloseDestructionProbeFeature;
    destructionProbe->destroyed = &destructionObserved;
    closing->addObject(destructionProbe, "CloseDestructionProbe");

    std::vector<std::string> events;
    bool preDeleteSawClosing = false;
    bool preDeleteSawMapEntry = false;
    bool postDeleteSawMapRemoval = false;
    // Connect through the historical public base type to model an observer
    // compiled against the pre-resilient Application ABI.
    auto& legacyPreDeleteSignal =
        static_cast<fastsignals::signal<void(const App::Document&)>&>(
            App::GetApplication().signalDeleteDocument);
    auto throwingPreDelete = legacyPreDeleteSignal.connect(
        [&](const App::Document& observed) {
            if (&observed == closing) {
                events.emplace_back("throwing-pre-delete");
                throw Base::RuntimeError("intentional pre-delete lifecycle observer failure");
            }
        });
    auto cloneFailureArmed = std::make_shared<std::atomic<bool>>(false);
    auto throwingClonePreDelete = legacyPreDeleteSignal.connect(
        ThrowOnArmedCopy(cloneFailureArmed));
    auto reentrantCopyArmed = std::make_shared<std::atomic<bool>>(false);
    auto reentrantCopyObserved = std::make_shared<std::atomic<bool>>(false);
    auto reentrantCopyPreDelete = legacyPreDeleteSignal.connect(
        ReentrantSignalCopy(
            &legacyPreDeleteSignal, reentrantCopyArmed, reentrantCopyObserved));
    auto laterPreDelete = App::GetApplication().signalDeleteDocument.connect(
        [&](const App::Document& observed) {
            if (&observed == closing) {
                events.emplace_back("later-pre-delete");
                preDeleteSawClosing =
                    observed.collaborationIdentity().state
                    == App::DocumentLifecycleState::Closing;
                preDeleteSawMapEntry =
                    App::GetApplication().getDocument(name.c_str()) == closing;
            }
        });

    cloneFailureArmed->store(true, std::memory_order_release);
    reentrantCopyArmed->store(true, std::memory_order_release);
    auto throwingPostDelete = App::GetApplication().signalDeletedDocument.connect([&] {
        events.emplace_back("throwing-post-delete");
        throw std::logic_error("intentional post-delete lifecycle observer failure");
    });
    auto laterPostDelete = App::GetApplication().signalDeletedDocument.connect([&] {
        events.emplace_back("later-post-delete");
        postDeleteSawMapRemoval =
            App::GetApplication().getDocument(name.c_str()) == nullptr;
    });

    bool closed = false;
    EXPECT_NO_THROW(closed = App::GetApplication().closeDocument(name.c_str()));
    laterPostDelete.disconnect();
    throwingPostDelete.disconnect();
    laterPreDelete.disconnect();
    reentrantCopyPreDelete.disconnect();
    throwingClonePreDelete.disconnect();
    throwingPreDelete.disconnect();

    EXPECT_TRUE(closed);
    EXPECT_TRUE(preDeleteSawClosing);
    EXPECT_TRUE(preDeleteSawMapEntry);
    EXPECT_TRUE(postDeleteSawMapRemoval);
    EXPECT_TRUE(destructionObserved);
    EXPECT_TRUE(reentrantCopyObserved->load(std::memory_order_acquire));
    const std::vector<std::string> expectedEvents {
        "throwing-pre-delete",
        "later-pre-delete",
        "throwing-post-delete",
        "later-post-delete",
    };
    EXPECT_EQ(events, expectedEvents);
    EXPECT_EQ(App::GetApplication().getDocument(name.c_str()), nullptr);

    const auto tombstone =
        App::GetApplication().collaborationRegistry().identity(liveIdentity.instanceId);
    ASSERT_TRUE(tombstone.has_value());
    EXPECT_EQ(tombstone->state, App::DocumentLifecycleState::Closed);
    EXPECT_NE(tombstone->lifecycleEpoch, liveIdentity.lifecycleEpoch);

    // Reusing the exact name proves the successful close left no stale
    // recompute tombstone or service-lifetime admission gate behind.
    auto* reused = App::GetApplication().newDocument(
        name.c_str(), "reusedAfterThrowingClose", flags);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->getName(), name);
    EXPECT_TRUE(App::GetApplication().closeDocument(name.c_str()));
}

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
TEST_F(DocumentCollaborationBoundaryTest,
       tombstoneReservationFailurePrecedesIrrevocableClosingBoundary)
{
    App::DocumentInitFlags flags;
    flags.createView = false;
    const std::string name =
        App::GetApplication().getUniqueDocumentName("tombstoneReservationFailure");
    auto* closing = App::GetApplication().newDocument(
        name.c_str(), "tombstoneReservationFailure", flags);
    ASSERT_NE(closing, nullptr);
    const auto liveIdentity = closing->collaborationIdentity();
    ASSERT_EQ(liveIdentity.state, App::DocumentLifecycleState::Live);
    ASSERT_FALSE(closing->collaborationRevisionPublicationSuppressed());

    TombstonePreparationHookReset hookReset;
    App::Internal::CollaborationRegistryTestAccess::setTombstonePreparationHook(
        &throwTombstonePreparationAllocationFailure);
    EXPECT_THROW(App::GetApplication().closeDocument(name.c_str()), std::bad_alloc);
    App::Internal::CollaborationRegistryTestAccess::setTombstonePreparationHook(nullptr);

    ASSERT_EQ(App::GetApplication().getDocument(name.c_str()), closing);
    EXPECT_EQ(closing->collaborationIdentity(), liveIdentity);
    EXPECT_FALSE(closing->collaborationRevisionPublicationSuppressed());
    EXPECT_EQ(
        App::GetApplication().collaborationRegistry().validate(
            liveIdentity.instanceId, liveIdentity.lifecycleEpoch),
        App::DocumentIdentityValidation::Valid);

    ASSERT_TRUE(App::GetApplication().closeDocument(name.c_str()));
    EXPECT_EQ(App::GetApplication().getDocument(name.c_str()), nullptr);
    const auto tombstone =
        App::GetApplication().collaborationRegistry().identity(liveIdentity.instanceId);
    ASSERT_TRUE(tombstone.has_value());
    EXPECT_EQ(tombstone->state, App::DocumentLifecycleState::Closed);
    EXPECT_NE(tombstone->lifecycleEpoch, liveIdentity.lifecycleEpoch);
}
#endif
