// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <App/Application.h>
#include <App/CollaborationRegistry.h>
#include <App/Document.h>
#include <App/DocumentMutationAuthority.h>
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
#include <Base/Reader.h>
#include <src/App/InitApplication.h>

namespace
{

using App::DocumentRevision;
using App::DocumentRevisionKey;

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
        "property->setStatus(",
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

    const auto enforced = sanitizedBody.find("enforceDocumentMutation");
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
        if (_document) {
            App::DocumentMutationAuthority::instance().clearOwner(*_document);
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

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 17, "boundary-test");
    const auto beforeRejectedWrite = captureFor(object->getNameInDocument());
    EXPECT_THROW(object->Label.setValue("Rejected"), Base::MutationDeniedException);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), beforeRejectedWrite)
        << "the pre-change authority guard must not publish any semantic revision";
    EXPECT_STREQ(object->Label.getValue(), "Accepted");
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

TEST_F(DocumentCollaborationBoundaryTest, errorOnlyFeatureRecomputePublishesAndRejectsAuthority)
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

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 41, "recompute-error-test");
    before = captureFor(feature->getNameInDocument());
    EXPECT_THROW(
        document()->recomputeFeature(feature, false),
        Base::MutationDeniedException
    );
    EXPECT_TRUE(feature->isValid());
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*feature)), before);
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

TEST_F(DocumentCollaborationBoundaryTest, freezeStatePublishesAndRejectsAuthorityBeforeMutation)
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

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 38, "freeze-test");
    const auto beforeRejected = captureFor(object->getNameInDocument());
    EXPECT_THROW(object->freeze(), Base::MutationDeniedException);
    EXPECT_FALSE(object->isFreezed());
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), beforeRejected);
}

TEST_F(DocumentCollaborationBoundaryTest, pythonRecomputeStatusIngressPublishesAndRejectsAuthority)
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

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 40, "python-status-test");
    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(pythonObject->setNoTouch(Py::Boolean(false)), Base::MutationDeniedException);
    EXPECT_TRUE(object->testStatus(App::ObjectStatus::NoTouch));
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
}

TEST_F(DocumentCollaborationBoundaryTest, dynamicExtensionPublishesAndRejectsAuthority)
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

    auto* rejected = document()->addObject("App::FeatureTest", "RejectedExtensionIngress");
    ASSERT_NE(rejected, nullptr);
    Py::Object rejectedPython(rejected->getPyObject(), true);
    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 39, "extension-test");
    before = captureFor(rejected->getNameInDocument());
    EXPECT_THROW(rejectedPython.callMemberFunction("addExtension", args), Py::Exception);
    PyErr_Clear();
    EXPECT_FALSE(rejected->hasExtension(extensionType, false));
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*rejected)), before);
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

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 19, "python-from-string-test");
    const auto valueBeforeRejection = property->toString();
    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(property->fromString("[5]"), Base::MutationDeniedException);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    EXPECT_EQ(property->toString(), valueBeforeRejection);
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

TEST_F(DocumentCollaborationBoundaryTest, hostileDirectStatusAndListStorageWritersAreRejected)
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

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 29, "hostile-direct-test");

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(
        object->setPropertyStatus(App::Property::Hidden, true),
        Base::MutationDeniedException
    );
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(list->setSize(2), Base::MutationDeniedException);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    EXPECT_EQ(list->getSize(), 1);

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(list->set1Value(0, replacement), Base::MutationDeniedException);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    ASSERT_EQ((*list)[0], target);

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(
        list->setValues(std::vector<App::DocumentObject*> {replacement}),
        Base::MutationDeniedException
    );
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    ASSERT_EQ((*list)[0], target);
    const auto targetInList = target->getInList();
    const auto replacementInList = replacement->getInList();
    EXPECT_TRUE(std::ranges::find(targetInList, object) != targetInList.end());
    EXPECT_TRUE(std::ranges::find(replacementInList, object) == replacementInList.end());

    authority.clearOwner(*document());
    before = captureFor(object->getNameInDocument());
    list->set1Value(0, replacement);
    expectExactDeltasAndConflicts(
        before,
        {0, 0, 1, 1, 0},
        "AtomicPropertyChange reject then recover"
    );
    ASSERT_EQ((*list)[0], replacement);
}

TEST_F(DocumentCollaborationBoundaryTest, directLinkResetPublishesExactlyOnceAndRejectsAuthority)
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
    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 30, "link-reset-test");

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(link->resetLink(), Base::MutationDeniedException);
    EXPECT_EQ(link->getValue(), target);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
}

TEST_F(DocumentCollaborationBoundaryTest, directXLinkSubValuesPublishAndRejectAuthority)
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

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 32, "xlink-sub-test");

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(
        link->setSubValues(std::vector<std::string> {"Face2"}),
        Base::MutationDeniedException
    );
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    EXPECT_EQ(link->getSubValues(), (std::vector<std::string> {"Face1"}));
}

TEST_F(DocumentCollaborationBoundaryTest, existingXLinkSubListAddPublishesAndRejectsAuthority)
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

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 33, "xlink-list-test");

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(
        links->addValue(target, std::vector<std::string> {"Face3"}, true),
        Base::MutationDeniedException
    );
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    EXPECT_EQ(
        links->getSubValues(target),
        (std::vector<std::string> {"Face1", "Face2"})
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

TEST_F(DocumentCollaborationBoundaryTest, listResizeAndNestedAppendPublishExactlyOnceAndRejectAuthority)
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

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 31, "list-boundary-test");

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(list->setSize(3), Base::MutationDeniedException);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    EXPECT_EQ(list->getSize(), 2);

    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(list->set1Value(-1, 99), Base::MutationDeniedException);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    EXPECT_EQ(list->getSize(), 2);
    EXPECT_EQ((*list)[1], 42);
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

TEST_F(DocumentCollaborationBoundaryTest, touchRejectsAuthorityAndPublishesBeforeThrowingObserver)
{
    auto* object = document()->addObject("App::FeatureTest", "TouchIngress");
    ASSERT_NE(object, nullptr);
    auto* property = object->addDynamicProperty("App::PropertyInteger", "TouchValue");
    ASSERT_NE(property, nullptr);

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 36, "touch-test");
    auto before = captureFor(object->getNameInDocument());
    EXPECT_THROW(property->touch(), Base::MutationDeniedException);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
    authority.clearOwner(*document());

    auto connection = document()->signalChangedObject.connect(
        [&](const App::DocumentObject& changedObject, const App::Property& changedProperty) {
            if (&changedObject == object && &changedProperty == property) {
                throw Base::RuntimeError("intentional touch observer failure");
            }
        }
    );
    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(property->touch(), Base::RuntimeError);
    connection.disconnect();
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 0}, "throwing touch observer");
}

TEST_F(DocumentCollaborationBoundaryTest, directPropertyPurgeRejectsAuthorityAndPublishes)
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
    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 38, "property-purge-test");
    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(property->purgeTouched(), Base::MutationDeniedException);
    EXPECT_TRUE(property->isTouched());
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
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
    ASSERT_TRUE(document()->save());
    connection.disconnect();

    EXPECT_EQ(observerCalls, 1);
    EXPECT_STREQ(object->Label.getValue(), "ChangedBySaveObserver");
    expectExactDeltasAndConflicts(before, {0, 1, 0, 0, 0}, "save start observer");
}

TEST_F(DocumentCollaborationBoundaryTest, documentTipPublishesDocumentStructureAndRejectsAuthority)
{
    auto* object = document()->addObject("App::FeatureTest", "DocumentTipIngress");
    ASSERT_NE(object, nullptr);

    auto before = captureFor(object->getNameInDocument());
    document()->Tip.setValue(object);
    expectExactDeltasAndConflicts(before, {0, 0, 0, 0, 1}, "Document::Tip");

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 34, "document-tip-test");
    before = captureFor(object->getNameInDocument());
    EXPECT_THROW(document()->Tip.setValue(nullptr), Base::MutationDeniedException);
    EXPECT_EQ(document()->Tip.getValue(), object);
    EXPECT_EQ(revisions().capture(dependencyKeysFor(*object)), before);
}

TEST_F(DocumentCollaborationBoundaryTest, clearDocumentPublishesRemovalsAndRejectsAuthority)
{
    auto* object = document()->addObject("App::FeatureTest", "ClearDocumentIngress");
    ASSERT_NE(object, nullptr);
    const std::string objectName = object->getNameInDocument();
    const std::string firstIncarnation = document()->collaborationObjectIdentity(*object);
    const auto keys = dependencyKeysFor(objectName);

    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 35, "clear-document-test");
    auto before = revisions().capture(keys);
    EXPECT_THROW(document()->clearDocument(), Base::MutationDeniedException);
    EXPECT_NE(document()->getObject(objectName.c_str()), nullptr);
    EXPECT_EQ(revisions().capture(keys), before);

    authority.clearOwner(*document());
    before = revisions().capture(keys);
    document()->clearDocument();
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

TEST_F(DocumentCollaborationBoundaryTest, rejectedAddEntryPointsDoNotAttachOrPublish)
{
    auto& authority = App::DocumentMutationAuthority::instance();
    authority.setOwner(*document(), App::MutationOwner::McpOwned, 37, "add-object-test");

    auto before = captureFor("RejectedTypedObject");
    EXPECT_THROW(
        document()->addObject("App::FeatureTest", "RejectedTypedObject"),
        Base::MutationDeniedException
    );
    EXPECT_EQ(document()->getObject("RejectedTypedObject"), nullptr);
    EXPECT_EQ(revisions().capture(dependencyKeysFor("RejectedTypedObject")), before);

    before = captureFor("RejectedBatchObject");
    EXPECT_THROW(
        document()->addObjects("App::FeatureTest", {"RejectedBatchObject"}),
        Base::MutationDeniedException
    );
    EXPECT_EQ(document()->getObject("RejectedBatchObject"), nullptr);
    EXPECT_EQ(revisions().capture(dependencyKeysFor("RejectedBatchObject")), before);

    auto external = std::make_unique<App::FeatureTest>();
    before = captureFor("RejectedExternalObject");
    EXPECT_THROW(
        document()->addObject(external.get(), "RejectedExternalObject"),
        Base::MutationDeniedException
    );
    EXPECT_EQ(external->getDocument(), nullptr);
    EXPECT_EQ(document()->getObject("RejectedExternalObject"), nullptr);
    EXPECT_EQ(revisions().capture(dependencyKeysFor("RejectedExternalObject")), before);
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
