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

#pragma once

#ifdef _MSC_VER
#pragma warning(disable : 4834)
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <list>
#include <mutex>
#include <thread>
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <utility>

#include <boost/bimap.hpp>
#include <boost/graph/adjacency_list.hpp>

#include <CXX/Objects.hxx>

#include <App/DocumentObject.h>
#include <App/DocumentObserver.h>
#include <App/DocumentRevisionIndex.h>
#include <App/MainThreadSignal.h>
#include <App/StringHasher.h>
#include <App/ExportInfo.h>
#include <Base/UniqueNameManager.h>

// using VertexProperty = boost::property<boost::vertex_root_t, DocumentObject* >;
using DependencyList = boost::adjacency_list<
    boost::vecS,         // class OutEdgeListS  : a Sequence or an AssociativeContainer
    boost::vecS,         // class VertexListS   : a Sequence or a RandomAccessContainer
    boost::directedS,    // class DirectedS     : This is a directed graph
    boost::no_property,  // class VertexProperty:
    boost::no_property,  // class EdgeProperty:
    boost::no_property,  // class GraphProperty:
    boost::listS         // class EdgeListS:
    >;
using Traits = boost::graph_traits<DependencyList>;
using Vertex = Traits::vertex_descriptor;
using Edge = Traits::edge_descriptor;
using Node = std::vector<size_t>;
using Path = std::vector<size_t>;

namespace App
{
using HasherMap = boost::bimap<StringHasherRef, int>;
class Document;
class Transaction;
class DocumentCollaborationService;
enum class DocumentSaveIntent;
struct DocumentSaveOutcome;

enum class CollaborationDeferredNotificationKind
{
    DocumentBeforeChange,
    DocumentChanged,
    FileChangeStateChanged,
    ObjectBeforeChange,
    ObjectEarlyChanged,
    ObjectChanged,
    PropertyChanged,
    TouchedObject,
    RelabelObject,
    BeforeRecompute,
    RecomputedObject,
    Recomputed,
    OpenTransaction,
    CommitTransaction,
    AbortTransaction,
    BecameStable,
    NewObject,
    DeletedObject,
    TransactionAppendObject,
    TransactionRemoveObject,
    ActivatedObject,
    AppendDynamicProperty,
    RemoveDynamicProperty,
    RenameDynamicProperty,
    ChangePropertyEditor,
    BeforeAddingDynamicExtension,
    AddedDynamicExtension,
    ImportObjects,
    FinishRestoreObject,
    FinishImportObjects
};

struct CollaborationDeferredNotification
{
    CollaborationDeferredNotification(
        CollaborationDeferredNotificationKind kind,
        DocumentObject* object = nullptr,
        Property* property = nullptr,
        std::string text = {},
        std::vector<DocumentObject*> objects = {})
        : kind(kind)
        , object(object)
        , property(property)
        , propertyContainer(property ? property->getContainer() : nullptr)
        , text(std::move(text))
        , objects(std::move(objects))
    {}

    CollaborationDeferredNotificationKind kind;
    DocumentObject* object {nullptr};
    Property* property {nullptr};
    const PropertyContainer* propertyContainer {nullptr};
    Transaction* transaction {nullptr};
    std::string text;
    std::vector<DocumentObject*> objects;
    std::shared_ptr<Internal::CollaborationImportReplay> importReplay;
    std::shared_ptr<std::string> retainedText;
    std::shared_ptr<Property> retainedProperty;
};

// Pimpl class
struct DocumentP
{
    struct FileChangeTokenState
    {
        struct Channel
        {
            std::uint64_t transactional {0};
            std::uint64_t sticky {0};
        };
        Channel model;
        Channel appearance;
        Channel compatibility;
    };

    struct TransactionalFileChangeTokenState
    {
        std::uint64_t model {0};
        std::uint64_t appearance {0};
        std::uint64_t compatibility {0};
    };

    struct TransactionFileChangeState
    {
        TransactionalFileChangeTokenState before;
        TransactionalFileChangeTokenState after;
    };

    // Array to preserve the creation order of created objects
    std::vector<DocumentObject*> objectArray;
    std::unordered_set<App::DocumentObject*> touchedObjs;
    std::unordered_map<std::string, DocumentObject*> objectMap;
    Base::UniqueNameManager objectNameManager;
    Base::UniqueNameManager objectLabelManager;
    std::unordered_map<long, DocumentObject*> objectIdMap;
    std::unordered_map<std::string, bool> partialLoadObjects;
    std::vector<DocumentObjectT> pendingRemove;
    std::atomic_bool pendingRemovalProcessing {false};
    long lastObjectId {};
    DocumentObject* activeObject {nullptr};
    Transaction* activeUndoTransaction {nullptr};
    FileChangeTokenState fileChangeTokens;
    FileChangeTokenState canonicalSaveTokens;
    // The canonical savepoint belongs to both a content revision and a path.
    // FileName is intentionally transient and can be changed by legacy callers;
    // a different path must therefore never reuse the old clean savepoint.
    std::string canonicalSavePath;
    std::optional<TransactionFileChangeState> activeTransactionFileChanges;
    std::unordered_map<int, TransactionFileChangeState> transactionFileChanges;
    std::uint64_t nextFileChangeToken {1};
    ResilientMainThreadSignal<void(const Document&)> signalFileChangeStateChanged;
    ResilientMainThreadSignal<void(const Document&, const DocumentSaveOutcome&)>
        signalSaveOutcome;
    std::vector<DocumentSaveIntent> activeSaveIntents;
    bool suppressFileChangeTracking {true};
    // Save As temporarily stages document identity before serialization.  A
    // failed write must restore that identity without allowing public document
    // property observers to interrupt the physical Uid/TransientDir repair.
    bool suppressSaveBookkeepingNotifications {false};
    // Suppression is restricted to the exact identity/metadata properties
    // currently owned by save staging.  Save observers may still mutate and
    // publish arbitrary document properties while this set is populated.
    std::unordered_set<const Property*> saveBookkeepingPropertySuppression;
    bool lastCanonicalSaveFailed {false};
    // pointer to the python class
    Py::Object DocumentPythonObject;
    int iTransactionMode {0};
    bool rollback {false};
    bool undoing {false};  ///< document in the middle of undo or redo
    bool committing {false};
    bool definingTransaction {false};
    bool opentransaction {false};
    bool suppressCollaborationRevisionPublication {true};
    bool collaborationCommitNotificationBarrier {false};
    bool collaborationTransactionControlGranted {false};
    bool collaborationCompatibilityStructuralMutationGranted {false};
    bool collaborationCompatibilityRecomputeMutationGranted {false};
    bool collaborationCompatibilityTrustedStructuralMutationGranted {false};
    bool collaborationDeferredRecomputeBlocked {false};
    const DocumentObject* collaborationCompatibilityRecomputeSourceObject {nullptr};
    bool collaborationImportDeferralActive {false};
    bool collaborationRollbackStabilizing {false};
    bool collaborationReplayingNotifications {false};
    bool collaborationCommitPoisoned {false};
    bool collaborationAtomicPresentationAuditActive {false};
    std::atomic_bool collaborationAtomicPresentationAuditViolated {false};
    bool collaborationAtomicPresentationAuditReadOnly {false};
    bool collaborationAtomicPresentationAuditPreparedOwner {false};
    std::vector<CollaborationAtomicPresentationWrite>
        collaborationAtomicPresentationAllowedWrites;
    std::atomic<unsigned int> collaborationLifecycleMutationBlockDepth {0};
    std::atomic<unsigned int> collaborationStableNotificationDepth {0};
    std::atomic<unsigned int> collaborationRecomputeTeardownDepth {0};
    std::recursive_mutex collaborationCommitMutex;
    std::thread::id collaborationOwnerThread {std::this_thread::get_id()};
    std::array<char, 1024> collaborationCommitPoisonDiagnostic {};
    std::bitset<32> StatusBits;
    unsigned int UndoMemSize {0};
    unsigned int UndoMaxStackSize {20};
    unsigned int TransactionLock {0};
    // Id and name that the next transaction will take
    // as soon as there is a change to the document
    int bookedTransaction {0};

    std::string programVersion;
    mutable HasherMap hashers;
    std::multimap<const App::DocumentObject*, std::unique_ptr<App::DocumentObjectExecReturn>>
        _RecomputeLog;
    std::multimap<const App::DocumentObject*, std::unique_ptr<App::DocumentObjectExecReturn>>
        collaborationBoundaryRecomputeLog;
    ExportInfo exportInfo;
    DocumentRevisionIndex collaborationRevisions;
    std::unique_ptr<DocumentCollaborationService> collaborationService;
    std::unordered_map<const DocumentObject*, std::uint64_t> collaborationObjectIdentities;
    std::unordered_map<const DocumentObject*, std::uint64_t>
        collaborationBoundaryObjectIdentities;
    struct CollaborationBoundaryObjectSchedulerState
    {
        DocumentObject* object {nullptr};
        unsigned long status {0};
        bool touched {false};
        unsigned long expressionStatus {0};
        std::unordered_set<std::string> touchedProperties;
    };
    std::vector<CollaborationBoundaryObjectSchedulerState>
        collaborationBoundaryObjectSchedulerStates;
    std::unordered_set<DocumentObject*> collaborationBoundaryTouchedObjects;
    struct CollaborationBoundaryPropertyStatusState
    {
        long objectId {0};
        std::int64_t propertyId {0};
        std::string propertyName;
        unsigned long before {0};
        unsigned long after {0};
    };
    std::vector<CollaborationBoundaryPropertyStatusState>
        collaborationBoundaryPropertyStatusStates;
    struct CollaborationTransactionPropertyStatusState
    {
        long objectId {0};
        std::int64_t propertyId {0};
        std::string propertyName;
        unsigned long before {0};
        unsigned long after {0};
    };
    std::unordered_map<int, std::vector<CollaborationTransactionPropertyStatusState>>
        collaborationTransactionPropertyStatusStates;
    std::vector<DocumentObject*> collaborationBoundaryObjectOrder;
    std::vector<DocumentObjectT> collaborationBoundaryPendingRemove;
    DocumentObject* collaborationBoundaryActiveObject {nullptr};
    std::unordered_set<const DocumentObject*> collaborationInitializationSuppression;
    std::unordered_set<const DocumentObject*> collaborationNewObjectStructuralSetup;
    std::unordered_set<const DocumentObject*> collaborationImportNewObjects;
    std::unordered_set<const Property*> collaborationPropertyPublicationSuppression;
    std::vector<CollaborationDeferredNotification> collaborationDeferredNotifications;
    std::vector<DocumentRevisionPublicationRequest> collaborationObservedStructuralEffects;
    std::shared_ptr<Internal::CollaborationImportReplay> collaborationActiveImportReplay;
    const DocumentObject* collaborationSpreadsheetRecomputeSchemaObject {nullptr};
    std::list<Transaction*> collaborationPreparedUndoSlot;

    StringHasherRef Hasher {new StringHasher};

    DocumentP();

    void addRecomputeLog(const char* why, App::DocumentObject* obj)
    {
        addRecomputeLog(new DocumentObjectExecReturn(why, obj));
    }

    void addRecomputeLog(const std::string& why, App::DocumentObject* obj)
    {
        addRecomputeLog(new DocumentObjectExecReturn(why, obj));
    }

    void addRecomputeLog(DocumentObjectExecReturn* returnCode)
    {
        if (!returnCode->Which) {
            delete returnCode;
            return;
        }
        _RecomputeLog.emplace(returnCode->Which,
                              std::unique_ptr<DocumentObjectExecReturn>(returnCode));
        returnCode->Which->setStatus(ObjectStatus::Error, true);
    }

    void clearRecomputeLog(const App::DocumentObject* obj = nullptr)
    {
        if (!obj) {
            _RecomputeLog.clear();
        }
        else {
            _RecomputeLog.erase(obj);
        }
    }

    void clearDocument()
    {
        objectLabelManager.clear();
        objectArray.clear();
        for (auto& v : objectMap) {
            v.second->setStatus(ObjectStatus::Destroy, true);
            delete (v.second);
            v.second = nullptr;
        }
        objectMap.clear();
        objectNameManager.clear();
        objectIdMap.clear();
    }

    const char* findRecomputeLog(const App::DocumentObject* obj)
    {
        auto range = _RecomputeLog.equal_range(obj);
        if (range.first == range.second) {
            return nullptr;
        }
        return (--range.second)->second->Why.c_str();
    }

    static void findAllPathsAt(const std::vector<Node>& all_nodes,
                               size_t id,
                               std::vector<Path>& all_paths,
                               Path tmp);
    std::vector<App::DocumentObject*>
    topologicalSort(const std::vector<App::DocumentObject*>& objects) const;
    static std::vector<App::DocumentObject*>
    partialTopologicalSort(const std::vector<App::DocumentObject*>& objects);
    static void checkStringHasher(const Base::XMLReader& reader);
};

}  // namespace App
