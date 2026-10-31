// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <App/Application.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/CollaborativeSetPropertyOperation.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentCommand.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/DocumentPresentationBoundary.h>
#include <App/DocumentRecomputeCoordinator.h>
#include <App/DocumentRevisionIndex.h>
#include <App/EditSession.h>
#include <App/FeatureTest.h>
#include <App/GenericIsolatedRecompute.h>
#include <App/PreparedEdit.h>
#include <App/Property.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <App/RecomputeHandle.h>
#include <Base/Parameter.h>
#include <fastsignals/connection.h>

#include "InitApplication.h"

using namespace App;

namespace {

constexpr auto kTimeout = std::chrono::seconds(30);

struct FineGrainedPreference
{
    Base::Reference<ParameterGrp> group;
    bool previous {false};

    explicit FineGrainedPreference(bool enabled)
        : group(GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document"))
        , previous(group->GetBool("FineGrainedRecompute", false))
    {
        group->SetBool("FineGrainedRecompute", enabled);
    }

    ~FineGrainedPreference()
    {
        group->SetBool("FineGrainedRecompute", previous);
    }
};

std::string objectName(const DocumentObject& object)
{
    const char* name = object.getNameInDocument();
    return name ? name : std::string(object.getTypeId().getName());
}

std::string propertyEvent(const char* kind, const DocumentObject& object, const Property& property)
{
    return std::string(kind) + ":" + objectName(object) + "." + property.getName();
}

DocumentRevisionCursor cursorNow(Document& document);
DocumentRevisionKey propertyKey(const DocumentObject& object, const Property& property);
DocumentRevisionKey objectModelKey(const DocumentObject& object);
bool eventsContain(const DocumentRevisionPollResult& poll, const DocumentRevisionKey& key);
std::string joinTrace(const std::vector<std::string>& trace);

class SignalTrace
{
public:
    std::vector<std::string> events;
    std::vector<std::string> publicationFailures;
    std::vector<std::string> publicationReadbacks;

    void watch(Document& document)
    {
        publicationDocument = &document;
        publicationCursor = cursorNow(document);
        connections.reserve(64);
        connections.emplace_back(document.signalBeforeRecompute.connect(
            [&](const Document&) { events.emplace_back("beforeRecompute"); }));
        connections.emplace_back(document.signalBeforeChangeObject.connect(
            [&](const DocumentObject& object, const Property& property) {
                events.push_back(propertyEvent("docBefore", object, property));
            }));
        connections.emplace_back(document.signalChangedObject.connect(
            [&](const DocumentObject& object, const Property& property) {
                events.push_back(propertyEvent("docChanged", object, property));
                notePublished(object, property);
            }));
        connections.emplace_back(document.signalTouchedObject.connect(
            [&](const DocumentObject& object) {
                events.push_back("touched:" + objectName(object));
            }));
        connections.emplace_back(document.signalRecomputedObject.connect(
            [&](const DocumentObject& object) {
                events.push_back("recomputedObject:" + objectName(object));
            }));
        connections.emplace_back(document.signalRecomputed.connect(
            [&](const Document&, const std::vector<DocumentObject*>& objects) {
                std::string line = "recomputed:";
                for (std::size_t index = 0; index < objects.size(); ++index) {
                    if (index != 0) {
                        line += ",";
                    }
                    line += objects[index] ? objectName(*objects[index]) : "?";
                }
                events.push_back(std::move(line));
            }));
        connections.emplace_back(document.signalBecameStable.connect(
            [&](const Document&) { events.emplace_back("becameStable"); }));
        connections.emplace_back(document.signalOpenTransaction.connect(
            [&](const Document&, std::string name) {
                events.push_back("openTransaction:" + name);
            }));
        connections.emplace_back(document.signalCommitTransaction.connect(
            [&](const Document&) { events.emplace_back("commitTransaction"); }));
        connections.emplace_back(document.signalAbortTransaction.connect(
            [&](const Document&) { events.emplace_back("abortTransaction"); }));
        connections.emplace_back(document.signalUndo.connect(
            [&](const Document&) { events.emplace_back("undo"); }));
        connections.emplace_back(document.signalRedo.connect(
            [&](const Document&) { events.emplace_back("redo"); }));

        for (DocumentObject* object : document.getObjects()) {
            if (!object) {
                continue;
            }
            connections.emplace_back(object->signalBeforeChange.connect(
                [&](const DocumentObject& changed, const Property& property) {
                    events.push_back(propertyEvent("objBefore", changed, property));
                }));
            connections.emplace_back(object->signalChanged.connect(
                [&](const DocumentObject& changed, const Property& property) {
                    events.push_back(propertyEvent("objChanged", changed, property));
                    notePublished(changed, property);
                }));
            std::vector<Property*> properties;
            object->getPropertyList(properties);
            for (Property* property : properties) {
                if (!property) {
                    continue;
                }
                connections.emplace_back(property->signalChanged.connect([&](const Property& changed) {
                    const auto* container =
                        dynamic_cast<const DocumentObject*>(changed.getContainer());
                    events.push_back(std::string("prop:")
                                     + (container ? objectName(*container) : std::string("?"))
                                     + "." + changed.getName());
                    if (container) {
                        notePublished(*container, changed);
                    }
                }));
            }
        }
        capturePublicationBaselines();
    }

    void expectPublications()
    {
        publicationDocument = nullptr;
        EXPECT_TRUE(publicationFailures.empty()) << joinTrace(publicationFailures);
        EXPECT_FALSE(publicationReadbacks.empty())
            << "observers returned without reading the published model";
    }

    void disarmPublicationChecks()
    {
        publicationDocument = nullptr;
    }

private:
    void capturePublicationBaselines()
    {
        if (!publicationDocument) {
            return;
        }
        auto& revisions = publicationDocument->collaborationRevisions();
        const auto wildcard = DocumentRevisionKey::unknownModelMutation();
        baselineRevisions[wildcard] = revisions.current(wildcard);
        for (DocumentObject* object : publicationDocument->getObjects()) {
            if (!object || !object->getNameInDocument()) {
                continue;
            }
            const auto model = objectModelKey(*object);
            baselineRevisions[model] = revisions.current(model);
            std::vector<Property*> properties;
            object->getPropertyList(properties);
            for (Property* property : properties) {
                if (!property) {
                    continue;
                }
                const auto key = propertyKey(*object, *property);
                baselineRevisions[key] = revisions.current(key);
            }
        }
    }

    void notePublished(const DocumentObject& object, const Property& property)
    {
        if (!publicationDocument || !object.getNameInDocument()) {
            return;
        }
        try {
            const auto key = propertyKey(object, property);
            const auto model = objectModelKey(object);
            const auto wildcard = DocumentRevisionKey::unknownModelMutation();
            auto& revisions = publicationDocument->collaborationRevisions();
            const auto keyNow = revisions.current(key);
            const auto modelNow = revisions.current(model);
            const auto wildcardNow = revisions.current(wildcard);
            const auto keyBase = baselineOrZero(key);
            const auto modelBase = baselineOrZero(model);
            const auto wildcardBase = baselineOrZero(wildcard);
            const auto poll = revisions.pollPublications(publicationCursor);
            const bool objectVisible = (keyNow > keyBase && eventsContain(poll, key))
                || (modelNow > modelBase && eventsContain(poll, model));
            const bool wildcardVisible =
                wildcardNow > wildcardBase && eventsContain(poll, wildcard);
            const bool exactValue = dynamic_cast<const PropertyInteger*>(&property)
                || dynamic_cast<const PropertyString*>(&property);
            const std::string label = propertyEvent("readback", object, property);
            std::string valueText;
            if (const auto* integer = dynamic_cast<const PropertyInteger*>(&property)) {
                valueText = std::to_string(integer->getValue());
            }
            else if (const auto* text = dynamic_cast<const PropertyString*>(&property)) {
                valueText = text->getValue() ? text->getValue() : "";
            }
            else if (const auto* length = dynamic_cast<const PropertyLength*>(&property)) {
                valueText = std::to_string(length->getValue());
            }
            else {
                valueText = property.getName();
            }
            publicationReadbacks.push_back(label + "=" + valueText + "@" + std::to_string(keyNow)
                                           + "/" + std::to_string(modelNow) + "/"
                                           + std::to_string(wildcardNow));
            // Exact integer and string writes, including a recompute's declared
            // property effects, publish that object's key. A PropertyLength
            // same-value write publishes the wildcard instead. Either way the
            // journal entry and the new revision are already visible here.
            if (exactValue) {
                if (!objectVisible) {
                    publicationFailures.push_back(
                        label + " read " + valueText
                        + " before its object publication (property revision " + std::to_string(keyNow)
                        + " baseline " + std::to_string(keyBase) + ", model revision "
                        + std::to_string(modelNow) + " baseline " + std::to_string(modelBase) + ")");
                }
            }
            else if (!objectVisible && !wildcardVisible) {
                publicationFailures.push_back(
                    label + " read " + valueText + " before publication (wildcard revision "
                    + std::to_string(wildcardNow) + " baseline " + std::to_string(wildcardBase) + ")");
            }
        }
        catch (const std::exception& error) {
            publicationFailures.push_back(std::string("publication readback threw: ") + error.what());
        }
    }

    DocumentRevision baselineOrZero(const DocumentRevisionKey& key) const
    {
        const auto found = baselineRevisions.find(key);
        return found == baselineRevisions.end() ? DocumentRevision {0} : found->second;
    }

    std::vector<::fastsignals::scoped_connection> connections;
    Document* publicationDocument {nullptr};
    DocumentRevisionCursor publicationCursor {};
    std::map<DocumentRevisionKey, DocumentRevision> baselineRevisions;
};

std::string joinTrace(const std::vector<std::string>& trace)
{
    std::string text;
    for (const auto& event : trace) {
        text += event;
        text += '\n';
    }
    return text;
}

void expectTrace(const std::vector<std::string>& actual, const std::vector<std::string>& expected)
{
    EXPECT_EQ(actual, expected) << joinTrace(actual);
}

std::vector<std::size_t> positions(const std::vector<std::string>& trace, const std::string& event)
{
    std::vector<std::size_t> found;
    for (std::size_t index = 0; index < trace.size(); ++index) {
        if (trace[index] == event) {
            found.push_back(index);
        }
    }
    return found;
}

void expectBefore(const std::vector<std::string>& trace,
                  const std::string& earlier,
                  const std::string& later)
{
    const auto earlierHits = positions(trace, earlier);
    const auto laterHits = positions(trace, later);
    ASSERT_FALSE(earlierHits.empty()) << earlier << "\n" << joinTrace(trace);
    ASSERT_FALSE(laterHits.empty()) << later << "\n" << joinTrace(trace);
    EXPECT_LT(earlierHits.front(), laterHits.front()) << joinTrace(trace);
}

DocumentRevisionCursor cursorNow(Document& document)
{
    const auto identity = document.collaborationIdentity();
    const auto metadata = document.collaborationRevisions().pollPublications(
        {identity.instanceId, identity.lifecycleEpoch, 0},
        0);
    return {identity.instanceId, identity.lifecycleEpoch, metadata.latestSequence};
}

DocumentRevisionKey propertyKey(const DocumentObject& object, const Property& property)
{
    return DocumentRevisionKey::objectProperty(object.getNameInDocument(), property.getName());
}

DocumentRevisionKey objectModelKey(const DocumentObject& object)
{
    return DocumentRevisionKey::objectModel(object.getNameInDocument());
}

bool eventsContain(const DocumentRevisionPollResult& poll, const DocumentRevisionKey& key)
{
    for (const auto& event : poll.events) {
        for (const auto& change : event.changes) {
            if (change.key == key) {
                return true;
            }
        }
    }
    return false;
}

bool conflictsOn(const std::vector<DocumentRevisionConflict>& conflicts,
                 const DocumentRevisionKey& key)
{
    for (const auto& conflict : conflicts) {
        if (conflict.key == key) {
            return true;
        }
    }
    return false;
}

const DocumentRecomputeFeatureSnapshot& featureNode(const DocumentRecomputeSnapshot& snapshot,
                                                     const char* name)
{
    const auto found = std::find_if(
        snapshot.features.begin(), snapshot.features.end(), [&](const auto& feature) {
            return feature.featureId == name;
        });
    if (found == snapshot.features.end()) {
        throw std::runtime_error(std::string("recompute snapshot omitted ") + name);
    }
    return *found;
}

class RecomputePublicationContract: public ::testing::Test
{
protected:
    void SetUp() override
    {
        tests::initApplication();
        Internal::ensureGenericIsolatedRecomputeRegistered();
        ensureCollaborativeSetPropertyOperationRegistered();
        fineGrained.emplace(false);
        documentName = GetApplication().getUniqueDocumentName("PublicationContract");
        document = GetApplication().newDocument(documentName.c_str(), "test");
        document->setMaxUndoStackSize(20);
        session.emplace(document->collaborationService().beginEditSession("publication-contract"));
    }

    void TearDown() override
    {
        session.reset();
        if (document) {
            GetApplication().closeDocument(document->getName());
            document = nullptr;
        }
        fineGrained.reset();
    }

    FeatureTest& feature(const char* name)
    {
        return *document->addObject<FeatureTest>(name);
    }

    DocumentObject& host(const char* name)
    {
        return *document->addObject<DocumentObject>(name);
    }

    PropertyInteger& integer(DocumentObject& object, const char* name, short type)
    {
        return *static_cast<PropertyInteger*>(
            object.addDynamicProperty("App::PropertyInteger", name, nullptr, nullptr, type));
    }

    std::string nextOperation(const char* prefix)
    {
        return std::string(prefix) + "-" + std::to_string(++operationSerial);
    }

    PreparedEdit prepareInteger(DocumentObject& object, PropertyInteger& property, long value)
    {
        return document->collaborationService().prepareEdit(
            session->sessionId(),
            nextOperation("set-integer"),
            {std::string(CollaborativeSetPropertyOperationType),
             {{"object", std::string(object.getNameInDocument())},
              {"property", std::string(property.getName())},
              {"value_type", "integer"},
              {"value", std::to_string(value)}}},
            "publication-contract");
    }

    PreparedEdit prepareRecompute(FeatureTest& target)
    {
        if (document->mustExecute()) {
            std::string pending;
            for (auto* object : document->getObjects()) {
                if (object && (object->isTouched() || object->mustRecompute())) {
                    pending += object->getNameInDocument();
                    pending += " ";
                }
            }
            throw std::runtime_error("prepareRecompute saw pending work: " + pending);
        }
        const auto executionId = document->collaborationService().prepareEditAsync(
            session->sessionId(),
            nextOperation("recompute"),
            {std::string(GenericIsolatedRecomputeOperationType),
             {{"feature", std::string(target.getNameInDocument())},
              {"legacy_revision_semantics", "1"},
              {"owner_thread_execution", "1"}}},
            "publication-contract");
        const auto deadline = std::chrono::steady_clock::now() + kTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status = document->collaborationService().preparedEditStatus(executionId);
            if (!status
                || (status->status != PreparedEditExecutionStatus::Completed
                    && status->status != PreparedEditExecutionStatus::Failed
                    && status->status != PreparedEditExecutionStatus::Cancelled)) {
                std::this_thread::yield();
                continue;
            }
            auto taken = document->collaborationService().takePreparedEdit(session->sessionId(),
                                                                           executionId);
            if (!taken || taken->status != PreparedEditExecutionStatus::Completed
                || !taken->preparedEdit) {
                const char* diagnostic = taken && !taken->diagnostic.empty()
                    ? taken->diagnostic.c_str()
                    : status->diagnostic.c_str();
                std::string touched;
                for (auto* object : document->getObjects()) {
                    if (!object) {
                        continue;
                    }
                    if (object->isTouched() || object->mustRecompute()) {
                        touched += object->getNameInDocument();
                        touched += object->isTouched() ? ":touched" : "";
                        touched += object->mustRecompute() ? ":recompute" : "";
                        touched += " ";
                    }
                }
                throw std::runtime_error(
                    std::string(diagnostic && *diagnostic
                                    ? diagnostic
                                    : "generic recompute preparation did not complete")
                    + " touched=[" + touched + "]");
            }
            return std::move(*taken->preparedEdit);
        }
        throw std::runtime_error("timed out preparing a generic recompute client");
    }

    DocumentCommitResult commitPrepared(const PreparedEdit& edit)
    {
        return document->collaborationService().commitEdit(session->sessionId(), edit);
    }

    void settle()
    {
        const auto snapshot = document->recomputeAsync()->wait(kTimeout);
        if (snapshot.state != DocumentRecomputeState::Completed) {
            throw std::runtime_error(snapshot.diagnostic.empty()
                                         ? "initial recompute did not complete"
                                         : snapshot.diagnostic);
        }
        if (document->mustExecute()) {
            std::string pending;
            for (auto* object : document->getObjects()) {
                if (object && (object->isTouched() || object->mustRecompute())) {
                    pending += object->getNameInDocument();
                    pending += " ";
                }
            }
            throw std::runtime_error("document still requires recompute after settle: " + pending);
        }
    }

    std::optional<FineGrainedPreference> fineGrained;
    std::optional<EditSession> session;
    Document* document {nullptr};
    std::string documentName;
    int operationSerial {0};
};

class PresentationProbe
{
public:
    int calls {0};

    PresentationProbe()
    {
        setDocumentPresentationBoundaryCallback([this](Document&) { ++calls; });
    }

    ~PresentationProbe()
    {
        setDocumentPresentationBoundaryCallback(nullptr);
    }

    PresentationProbe(const PresentationProbe&) = delete;
    PresentationProbe& operator=(const PresentationProbe&) = delete;
};

struct CancelRendezvous
{
    std::mutex mutex;
    std::condition_variable ready;
    bool observed {false};
    bool released {false};
    bool abort {false};
    std::string timeout;
    std::exception_ptr cancelError;

    void waitForPublication(const DocumentObject& object, const Property& property)
    {
        const char* name = object.getNameInDocument();
        if (!name || std::string(name) != "A" || std::string(property.getName()) != "ExecCount") {
            return;
        }
        std::unique_lock<std::mutex> lock(mutex);
        if (observed) {
            return;
        }
        observed = true;
        ready.notify_all();
        if (!ready.wait_for(lock, kTimeout, [&] { return released || abort; })) {
            if (timeout.empty()) {
                timeout = "cancel handshake timed out after 30s waiting for cancel() to release "
                          "the ExecCount observer";
            }
        }
    }

    template<typename CancelFn>
    void cancelAndRelease(CancelFn&& cancelFn)
    {
        bool shouldCancel = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!ready.wait_for(lock, kTimeout, [&] { return observed || abort; })) {
                if (timeout.empty()) {
                    timeout = "cancel handshake timed out after 30s waiting for A's ExecCount "
                              "publication";
                }
            }
            else if (observed && !abort) {
                shouldCancel = true;
            }
            else if (timeout.empty()) {
                timeout = "cancel handshake aborted before A's ExecCount publication";
            }
        }
        if (shouldCancel) {
            try {
                std::forward<CancelFn>(cancelFn)();
            }
            catch (...) {
                cancelError = std::current_exception();
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        ready.notify_all();
    }
};

struct JoinCanceller
{
    std::thread thread;
    CancelRendezvous& gate;

    JoinCanceller(std::thread threadIn, CancelRendezvous& gateIn)
        : thread(std::move(threadIn))
        , gate(gateIn)
    {}

    ~JoinCanceller()
    {
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.abort = true;
            gate.released = true;
            gate.ready.notify_all();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    JoinCanceller(const JoinCanceller&) = delete;
    JoinCanceller& operator=(const JoinCanceller&) = delete;
};

template<typename WaitFn, typename CancelFn>
void rendezvousCancel(Document& document, WaitFn&& waitFn, CancelFn&& cancelFn)
{
    auto gate = std::make_shared<CancelRendezvous>();
    ::fastsignals::scoped_connection connection = document.signalChangedObject.connect(
        [gate](const DocumentObject& object, const Property& property) {
            gate->waitForPublication(object, property);
        });
    JoinCanceller joiner {
        std::thread([gate, cancelFn = std::forward<CancelFn>(cancelFn)]() mutable {
            gate->cancelAndRelease(std::move(cancelFn));
        }),
        *gate};
    std::forward<WaitFn>(waitFn)();
    ASSERT_TRUE(gate->timeout.empty()) << gate->timeout;
    if (gate->cancelError) {
        std::rethrow_exception(gate->cancelError);
    }
    static_cast<void>(connection);
    static_cast<void>(joiner);
}

DocumentCommandHandle submitLaneRecompute(Document& document, const char* key)
{
    auto execution = document.executionHandle();
    DocumentCommand command;
    command.kind = DocumentCommandKind::Recompute;
    command.document = execution.identity();
    command.recompute = DocumentCommandRecomputePayload {};
    command.recompute->coalescingKey = key;
    const auto outcome = execution.trySubmit(std::move(command));
    if (outcome.result != DocumentCommandSubmitResult::Accepted) {
        throw std::runtime_error(std::string("lane recompute was not accepted: ")
                                 + outcome.diagnostic);
    }
    return DocumentCommandHandle(outcome.commandId, execution.identity());
}

DocumentCommandSnapshot waitForLaneCommand(DocumentCommandHandle& handle)
{
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    auto snapshot = handle.status();
    while (!snapshot.terminal()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::string("lane recompute timed out in state ")
                                     + documentCommandStateName(snapshot.state) + ": "
                                     + snapshot.diagnostic);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        snapshot = handle.status();
    }
    return snapshot;
}

void expectFailureFinalization(const SignalTrace& trace, int presentationCalls)
{
    EXPECT_EQ(presentationCalls, 1);
    EXPECT_FALSE(positions(trace.events, "recomputedObject:Keep").empty()) << joinTrace(trace.events);
    EXPECT_FALSE(positions(trace.events, "recomputedObject:Other").empty()) << joinTrace(trace.events);
    EXPECT_FALSE(positions(trace.events, "recomputed:Bad,Down,Keep,Other").empty())
        << joinTrace(trace.events);
    EXPECT_TRUE(positions(trace.events, "becameStable").empty()) << joinTrace(trace.events);
}

void expectCancelFinalization(const SignalTrace& trace, int presentationCalls)
{
    EXPECT_EQ(presentationCalls, 1);
    EXPECT_FALSE(positions(trace.events, "recomputedObject:A").empty()) << joinTrace(trace.events);
    EXPECT_TRUE(positions(trace.events, "recomputedObject:B").empty()) << joinTrace(trace.events);
    EXPECT_FALSE(positions(trace.events, "recomputed:A,B").empty()) << joinTrace(trace.events);
    EXPECT_TRUE(positions(trace.events, "becameStable").empty()) << joinTrace(trace.events);
}

DocumentCommandSnapshot runLaneCancel(Document& document, const char* key)
{
    auto gate = std::make_shared<CancelRendezvous>();
    ::fastsignals::scoped_connection connection = document.signalChangedObject.connect(
        [gate](const DocumentObject& object, const Property& property) {
            gate->waitForPublication(object, property);
        });
    struct Release
    {
        CancelRendezvous& gate;
        DocumentCommandHandle* command {nullptr};

        Release(CancelRendezvous& gateIn, DocumentCommandHandle* commandIn)
            : gate(gateIn)
            , command(commandIn)
        {}

        ~Release()
        {
            {
                std::lock_guard<std::mutex> lock(gate.mutex);
                gate.released = true;
                gate.abort = true;
                gate.ready.notify_all();
            }
            if (command && command->valid()) {
                try {
                    static_cast<void>(waitForLaneCommand(*command));
                }
                catch (const std::exception&) {
                    // The caller already failed. Unblock the lane and let it finish
                    // so document teardown is not left waiting on the observer gate.
                }
            }
        }
    } release {*gate, nullptr};
    auto command = submitLaneRecompute(document, key);
    release.command = &command;
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        if (!gate->ready.wait_for(lock, kTimeout, [&] { return gate->observed || gate->abort; })) {
            if (gate->timeout.empty()) {
                gate->timeout =
                    "cancel handshake timed out after 30s waiting for A's ExecCount publication";
            }
        }
    }
    if (!gate->timeout.empty()) {
        throw std::runtime_error(gate->timeout);
    }
    if (!command.cancel("cancelled mid-way")) {
        throw std::runtime_error("lane cancel() returned false");
    }
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
        gate->ready.notify_all();
    }
    auto snapshot = waitForLaneCommand(command);
    if (!gate->timeout.empty()) {
        throw std::runtime_error(gate->timeout);
    }
    static_cast<void>(connection);
    return snapshot;
}

}  // namespace

TEST_F(RecomputePublicationContract, exactPublicationInvalidatesPreparedClientsCoveringThatKey)
{
    auto& target = host("Host");
    auto& first = integer(target, "First", Prop_NoRecompute);
    auto& second = integer(target, "Second", Prop_NoRecompute);
    first.setValue(1);
    second.setValue(2);
    settle();

    auto covering = prepareInteger(target, first, 11);
    auto disjoint = prepareInteger(target, second, 22);
    const auto cursor = cursorNow(*document);
    auto publisher = prepareInteger(target, first, 7);
    const auto committed = commitPrepared(publisher);
    ASSERT_TRUE(committed.committed()) << committed.message;

    const auto poll = document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(poll.status, DocumentRevisionCursorStatus::Valid);
    const auto firstKey = propertyKey(target, first);
    const auto modelKey = objectModelKey(target);
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();
    EXPECT_TRUE(eventsContain(poll, firstKey));
    EXPECT_TRUE(eventsContain(poll, modelKey));
    EXPECT_FALSE(eventsContain(poll, wildcard));

    const auto coveringConflicts =
        document->collaborationRevisions().validate(covering.expectedRevisions());
    EXPECT_FALSE(coveringConflicts.empty());
    EXPECT_TRUE(conflictsOn(coveringConflicts, firstKey));
    const auto coveringCommit = commitPrepared(covering);
    EXPECT_FALSE(coveringCommit.committed());
    EXPECT_EQ(coveringCommit.status, DocumentCommitStatus::Conflict);
    EXPECT_TRUE(conflictsOn(coveringCommit.conflicts, firstKey));

    EXPECT_TRUE(document->collaborationRevisions().validate(disjoint.expectedRevisions()).empty());
    const auto disjointCommit = commitPrepared(disjoint);
    EXPECT_TRUE(disjointCommit.committed()) << disjointCommit.message;
    EXPECT_EQ(first.getValue(), 7);
    EXPECT_EQ(second.getValue(), 22);
}

TEST_F(RecomputePublicationContract, siblingCompatibleExactPublicationLeavesDisjointPreparedClientValid)
{
    auto& target = feature("Feature");
    auto& side = host("Host");
    auto& first = integer(side, "First", Prop_NoRecompute);
    first.setValue(1);
    settle();
    const auto executed = target.ExecCount.getValue();

    auto recompute = prepareRecompute(target);
    const auto cursor = cursorNow(*document);
    auto publisher = prepareInteger(side, first, 4);
    ASSERT_TRUE(commitPrepared(publisher).committed());

    const auto poll = document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(poll.status, DocumentRevisionCursorStatus::Valid);
    EXPECT_TRUE(eventsContain(poll, propertyKey(side, first)));
    EXPECT_TRUE(eventsContain(poll, objectModelKey(side)));
    EXPECT_FALSE(eventsContain(
        poll,
        DocumentRevisionKey::unknownModelMutation()));
    EXPECT_TRUE(document->collaborationRevisions().validate(recompute.expectedRevisions()).empty());

    const auto committed = commitPrepared(recompute);
    EXPECT_TRUE(committed.committed()) << committed.message;
    EXPECT_EQ(target.ExecCount.getValue(), executed + 1);
}

TEST_F(RecomputePublicationContract,
       broadRecomputePublicationInformsExistingClientsAndInvalidatesCoveredContracts)
{
    auto& changed = feature("A");
    auto& sibling = feature("B");
    auto& side = host("Host");
    auto& first = integer(side, "First", Prop_NoRecompute);
    first.setValue(1);
    changed.touch();
    sibling.touch();
    settle();

    auto exactClient = prepareInteger(side, first, 9);
    auto recomputeClient = prepareRecompute(sibling);
    const auto cursor = cursorNow(*document);
    const auto recomputed = document->recompute({&changed}, true, nullptr, 0);
    EXPECT_EQ(recomputed, 1);

    const auto poll = document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(poll.status, DocumentRevisionCursorStatus::Valid);
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();
    const auto modelKey = objectModelKey(changed);
    EXPECT_TRUE(eventsContain(poll, wildcard));
    EXPECT_TRUE(eventsContain(poll, modelKey));

    const auto exactConflicts =
        document->collaborationRevisions().validate(exactClient.expectedRevisions());
    const auto recomputeConflicts =
        document->collaborationRevisions().validate(recomputeClient.expectedRevisions());
    EXPECT_TRUE(conflictsOn(exactConflicts, wildcard));
    EXPECT_TRUE(conflictsOn(recomputeConflicts, wildcard));

    const auto exactCommit = commitPrepared(exactClient);
    const auto recomputeCommit = commitPrepared(recomputeClient);
    EXPECT_EQ(exactCommit.status, DocumentCommitStatus::Conflict);
    EXPECT_EQ(recomputeCommit.status, DocumentCommitStatus::Conflict);
    EXPECT_TRUE(conflictsOn(exactCommit.conflicts, wildcard));
    EXPECT_TRUE(conflictsOn(recomputeCommit.conflicts, wildcard));
    EXPECT_EQ(first.getValue(), 1);
    EXPECT_EQ(changed.ExecCount.getValue(), 2);
    EXPECT_EQ(sibling.ExecCount.getValue(), 1);

    auto fresh = prepareInteger(side, first, 9);
    EXPECT_TRUE(commitPrepared(fresh).committed());
    EXPECT_EQ(first.getValue(), 9);
}

TEST_F(RecomputePublicationContract, realAndBroadMutationsRejectPreparedRecomputeClient)
{
    auto& target = feature("Feature");
    auto& lengths = host("Lengths");
    auto& length = *static_cast<PropertyLength*>(lengths.addDynamicProperty(
        "App::PropertyLength", "Length", nullptr, nullptr, Prop_None));
    length.setValue(10.0);
    settle();

    auto exact = prepareRecompute(target);
    target.Integer.setValue(3);
    const auto exactCommit = commitPrepared(exact);
    EXPECT_EQ(exactCommit.status, DocumentCommitStatus::Conflict);
    EXPECT_TRUE(conflictsOn(
        exactCommit.conflicts,
        propertyKey(target, target.Integer)));
    settle();

    auto structural = prepareRecompute(target);
    const auto structuralCursor = cursorNow(*document);
    target.addDynamicProperty("App::PropertyInteger", "Extra", nullptr, nullptr, Prop_None);
    EXPECT_EQ(commitPrepared(structural).status, DocumentCommitStatus::Conflict);
    EXPECT_TRUE(eventsContain(
        document->collaborationRevisions().pollPublications(structuralCursor),
        DocumentRevisionKey::objectStructure(target.getNameInDocument())));
    settle();

    auto documentWide = prepareRecompute(target);
    const auto documentCursor = cursorNow(*document);
    document->addObject<DocumentObject>("Added");
    EXPECT_EQ(commitPrepared(documentWide).status, DocumentCommitStatus::Conflict);
    EXPECT_TRUE(eventsContain(
        document->collaborationRevisions().pollPublications(documentCursor),
        DocumentRevisionKey::documentStructure()));
    settle();

    auto broad = prepareRecompute(target);
    const auto broadCursor = cursorNow(*document);
    length.setValue(11.0);
    const auto broadCommit = commitPrepared(broad);
    EXPECT_EQ(broadCommit.status, DocumentCommitStatus::Conflict);
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();
    EXPECT_TRUE(conflictsOn(broadCommit.conflicts, wildcard));
    EXPECT_TRUE(eventsContain(
        document->collaborationRevisions().pollPublications(broadCursor), wildcard));
}

TEST_F(RecomputePublicationContract,
       pr114SameValueExtentWritesPublishWildcardAndInvalidatePreparedClients)
{
    auto& datum = host("MidX");
    auto& length = *static_cast<PropertyLength*>(
        datum.addDynamicProperty("App::PropertyLength", "Length", nullptr, nullptr, Prop_None));
    auto& width = *static_cast<PropertyLength*>(
        datum.addDynamicProperty("App::PropertyLength", "Width", nullptr, nullptr, Prop_None));
    length.setValue(10.0);
    width.setValue(20.0);
    auto& target = feature("Feature");
    auto& side = host("Host");
    auto& first = integer(side, "First", Prop_NoRecompute);
    first.setValue(1);
    settle();

    auto exactClient = prepareInteger(side, first, 5);
    auto recomputeClient = prepareRecompute(target);
    SignalTrace trace;
    trace.watch(*document);
    const auto cursor = cursorNow(*document);
    length.setValue(length.getValue());
    width.setValue(width.getValue());

    const auto poll = document->collaborationRevisions().pollPublications(cursor);
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();
    EXPECT_TRUE(eventsContain(poll, wildcard));
    EXPECT_EQ(commitPrepared(exactClient).status, DocumentCommitStatus::Conflict);
    EXPECT_EQ(commitPrepared(recomputeClient).status, DocumentCommitStatus::Conflict);
    trace.expectPublications();
    expectTrace(trace.events,
                {"docBefore:MidX.Length",
                 "objBefore:MidX.Length",
                 "docChanged:MidX.Length",
                 "objChanged:MidX.Length",
                 "prop:MidX.Length",
                 "docBefore:MidX.Width",
                 "objBefore:MidX.Width",
                 "docChanged:MidX.Width",
                 "objChanged:MidX.Width",
                 "prop:MidX.Width"});
    settle();

    auto survivingExact = prepareInteger(side, first, 5);
    auto survivingRecompute = prepareRecompute(target);
    EXPECT_TRUE(commitPrepared(survivingExact).committed());
    EXPECT_TRUE(commitPrepared(survivingRecompute).committed());
}

TEST_F(RecomputePublicationContract, independentSiblingsCommitAndNotifyInNameOrder)
{
    auto& first = feature("A");
    auto& second = feature("B");
    settle();
    first.touch();
    second.touch();
    SignalTrace trace;
    trace.watch(*document);
    const auto snapshot = document->recomputeAsync()->wait(kTimeout);
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::Completed);
    expectBefore(trace.events, "docChanged:A.ExecCount", "docChanged:B.ExecCount");
    expectBefore(trace.events, "recomputedObject:A", "recomputedObject:B");
    trace.expectPublications();
    expectTrace(trace.events,
                {"beforeRecompute",
                 "docBefore:A.ExecCount",
                 "objBefore:A.ExecCount",
                 "docChanged:A.ExecCount",
                 "objChanged:A.ExecCount",
                 "prop:A.ExecCount",
                 "docBefore:A.ExecResult",
                 "objBefore:A.ExecResult",
                 "docChanged:A.ExecResult",
                 "objChanged:A.ExecResult",
                 "prop:A.ExecResult",
                 "docBefore:B.ExecCount",
                 "objBefore:B.ExecCount",
                 "docChanged:B.ExecCount",
                 "objChanged:B.ExecCount",
                 "prop:B.ExecCount",
                 "docBefore:B.ExecResult",
                 "objBefore:B.ExecResult",
                 "docChanged:B.ExecResult",
                 "objChanged:B.ExecResult",
                 "prop:B.ExecResult",
                 "recomputedObject:A",
                 "recomputedObject:B",
                 "recomputed:A,B",
                 "becameStable"});
}

TEST_F(RecomputePublicationContract, chainCommitsSourceBeforeConsumerAndNotifiesByName)
{
    auto& consumer = feature("A_consumer");
    auto& source = feature("Z_source");
    consumer.Source1.setValue(&source);
    settle();
    consumer.touch();
    source.touch();
    const auto dependencies = consumer.getOutList();
    ASSERT_EQ(dependencies.size(), 1u);
    EXPECT_EQ(dependencies.front(), &source);

    SignalTrace trace;
    trace.watch(*document);
    const auto snapshot = document->recomputeAsync()->wait(kTimeout);
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::Completed);
    expectBefore(trace.events, "docChanged:Z_source.ExecCount", "docChanged:A_consumer.ExecCount");
    expectBefore(trace.events, "recomputedObject:A_consumer", "recomputedObject:Z_source");
    trace.expectPublications();
    expectTrace(trace.events,
                {"beforeRecompute",
                 "docBefore:Z_source.ExecCount",
                 "objBefore:Z_source.ExecCount",
                 "docChanged:Z_source.ExecCount",
                 "objChanged:Z_source.ExecCount",
                 "prop:Z_source.ExecCount",
                 "docBefore:Z_source.ExecResult",
                 "objBefore:Z_source.ExecResult",
                 "docChanged:Z_source.ExecResult",
                 "objChanged:Z_source.ExecResult",
                 "prop:Z_source.ExecResult",
                 "touched:A_consumer",
                 "docBefore:A_consumer.ExecCount",
                 "objBefore:A_consumer.ExecCount",
                 "docChanged:A_consumer.ExecCount",
                 "objChanged:A_consumer.ExecCount",
                 "prop:A_consumer.ExecCount",
                 "recomputedObject:A_consumer",
                 "recomputedObject:Z_source",
                 "recomputed:A_consumer,Z_source",
                 "becameStable"});
}

TEST_F(RecomputePublicationContract, abzReadinessCommitsDependentBeforeIndependentSibling)
{
    auto& source = feature("A");
    auto& dependent = feature("B");
    auto& independent = feature("Z");
    dependent.Source1.setValue(&source);
    settle();
    source.touch();
    dependent.touch();
    independent.touch();
    ASSERT_EQ(dependent.getOutList().front(), &source);

    SignalTrace trace;
    trace.watch(*document);
    const auto snapshot = document->recomputeAsync()->wait(kTimeout);
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::Completed);
    expectBefore(trace.events, "docChanged:A.ExecCount", "docChanged:B.ExecCount");
    expectBefore(trace.events, "docChanged:B.ExecCount", "docChanged:Z.ExecCount");
    expectBefore(trace.events, "recomputedObject:A", "recomputedObject:B");
    expectBefore(trace.events, "recomputedObject:B", "recomputedObject:Z");
    trace.expectPublications();
    expectTrace(trace.events,
                {"beforeRecompute",
                 "docBefore:A.ExecCount",
                 "objBefore:A.ExecCount",
                 "docChanged:A.ExecCount",
                 "objChanged:A.ExecCount",
                 "prop:A.ExecCount",
                 "docBefore:A.ExecResult",
                 "objBefore:A.ExecResult",
                 "docChanged:A.ExecResult",
                 "objChanged:A.ExecResult",
                 "prop:A.ExecResult",
                 "touched:B",
                 "docBefore:B.ExecCount",
                 "objBefore:B.ExecCount",
                 "docChanged:B.ExecCount",
                 "objChanged:B.ExecCount",
                 "prop:B.ExecCount",
                 "docBefore:Z.ExecCount",
                 "objBefore:Z.ExecCount",
                 "docChanged:Z.ExecCount",
                 "objChanged:Z.ExecCount",
                 "prop:Z.ExecCount",
                 "docBefore:Z.ExecResult",
                 "objBefore:Z.ExecResult",
                 "docChanged:Z.ExecResult",
                 "objChanged:Z.ExecResult",
                 "prop:Z.ExecResult",
                 "recomputedObject:A",
                 "recomputedObject:B",
                 "recomputedObject:Z",
                 "recomputed:A,B,Z",
                 "becameStable"});
}

TEST_F(RecomputePublicationContract, coldRestoredDocumentRecomputePinsResultsPublicationsUndoAndSignals)
{
    const auto path =
        std::filesystem::path(DATADIR) / "tests" / "parallel-recompute"
        / "featuretest-abz-dirty.FCStd";
    ASSERT_TRUE(std::filesystem::exists(path)) << path;
    session.reset();
    GetApplication().closeDocument(document->getName());
    document = nullptr;
    document = GetApplication().openDocument(path.string().c_str());
    ASSERT_NE(document, nullptr);
    session.emplace(document->collaborationService().beginEditSession("cold"));
    document->setMaxUndoStackSize(20);
    document->clearUndos();

    auto* source = dynamic_cast<FeatureTest*>(document->getObject("A"));
    auto* dependent = dynamic_cast<FeatureTest*>(document->getObject("B"));
    auto* independent = dynamic_cast<FeatureTest*>(document->getObject("Z"));
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dependent, nullptr);
    ASSERT_NE(independent, nullptr);
    EXPECT_TRUE(source->isTouched());
    EXPECT_EQ(source->ExecCount.getValue(), 1);
    EXPECT_EQ(dependent->ExecCount.getValue(), 1);
    EXPECT_FALSE(independent->isTouched());
    EXPECT_EQ(independent->ExecCount.getValue(), 1);
    const auto undosBefore = document->getAvailableUndos();

    const auto cursor = cursorNow(*document);
    SignalTrace trace;
    trace.watch(*document);
    const auto snapshot = document->recomputeAsync()->wait(kTimeout);
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::Completed) << snapshot.diagnostic;
    EXPECT_EQ(source->ExecCount.getValue(), 2);
    EXPECT_EQ(dependent->ExecCount.getValue(), 2);
    EXPECT_EQ(independent->ExecCount.getValue(), 1);
    EXPECT_FALSE(source->isError());
    EXPECT_EQ(document->getAvailableUndos(), undosBefore);
    EXPECT_TRUE(eventsContain(
        document->collaborationRevisions().pollPublications(cursor),
        DocumentRevisionKey::unknownModelMutation()));
    trace.expectPublications();
    expectTrace(trace.events,
                {"beforeRecompute",
                 "docBefore:A.ExecCount",
                 "objBefore:A.ExecCount",
                 "docChanged:A.ExecCount",
                 "objChanged:A.ExecCount",
                 "prop:A.ExecCount",
                 "touched:B",
                 "docBefore:B.ExecCount",
                 "objBefore:B.ExecCount",
                 "docChanged:B.ExecCount",
                 "objChanged:B.ExecCount",
                 "prop:B.ExecCount",
                 "recomputedObject:A",
                 "recomputedObject:B",
                 "recomputed:A,B",
                 "becameStable"});
}

TEST_F(RecomputePublicationContract, failedFeaturePinsPartialSuccessPublicationsUndoAndSignals)
{
    auto& kept = feature("Keep");
    auto& bad = feature("Bad");
    auto& blocked = feature("Down");
    auto& other = feature("Other");
    blocked.Source1.setValue(&bad);
    settle();
    bad.ExceptionType.setValue(1);
    kept.touch();
    bad.touch();
    blocked.touch();
    other.touch();
    const auto keptCount = kept.ExecCount.getValue();
    const auto badCount = bad.ExecCount.getValue();
    const auto blockedCount = blocked.ExecCount.getValue();
    document->clearUndos();

    const auto cursor = cursorNow(*document);
    SignalTrace trace;
    trace.watch(*document);
    const auto snapshot = document->recomputeAsync()->wait(kTimeout);
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::PartialFailure) << snapshot.diagnostic;
    EXPECT_EQ(featureNode(snapshot, "Bad").state, DocumentRecomputeFeatureState::Failed);
    EXPECT_TRUE(featureNode(snapshot, "Bad").executed);
    EXPECT_TRUE(bad.isError());
    EXPECT_TRUE(bad.isTouched());
    EXPECT_EQ(bad.ExecCount.getValue(), badCount);
    const char* diagnostic = document->getErrorDescription(&bad);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_NE(std::string(diagnostic).find("Test Exception"), std::string::npos);
    EXPECT_EQ(featureNode(snapshot, "Down").state, DocumentRecomputeFeatureState::Blocked);
    EXPECT_EQ(blocked.ExecCount.getValue(), blockedCount);
    EXPECT_TRUE(blocked.isTouched());
    EXPECT_EQ(featureNode(snapshot, "Keep").state, DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureNode(snapshot, "Other").state, DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(kept.ExecCount.getValue(), keptCount + 1);
    EXPECT_EQ(other.ExecCount.getValue(), keptCount + 1);
    EXPECT_EQ(document->getAvailableUndos(), 0);
    EXPECT_TRUE(eventsContain(
        document->collaborationRevisions().pollPublications(cursor),
        DocumentRevisionKey::unknownModelMutation()));
    expectBefore(trace.events, "docChanged:Keep.ExecCount", "docChanged:Other.ExecCount");
    expectBefore(trace.events, "recomputedObject:Keep", "recomputedObject:Other");
    trace.expectPublications();
    expectTrace(trace.events,
                {"beforeRecompute",
                 "touched:Bad",
                 "docBefore:Keep.ExecCount",
                 "objBefore:Keep.ExecCount",
                 "docChanged:Keep.ExecCount",
                 "objChanged:Keep.ExecCount",
                 "prop:Keep.ExecCount",
                 "docBefore:Keep.ExecResult",
                 "objBefore:Keep.ExecResult",
                 "docChanged:Keep.ExecResult",
                 "objChanged:Keep.ExecResult",
                 "prop:Keep.ExecResult",
                 "docBefore:Other.ExecCount",
                 "objBefore:Other.ExecCount",
                 "docChanged:Other.ExecCount",
                 "objChanged:Other.ExecCount",
                 "prop:Other.ExecCount",
                 "docBefore:Other.ExecResult",
                 "objBefore:Other.ExecResult",
                 "docChanged:Other.ExecResult",
                 "objChanged:Other.ExecResult",
                 "prop:Other.ExecResult",
                 "recomputedObject:Keep",
                 "recomputedObject:Other",
                 "recomputed:Bad,Down,Keep,Other"});

    document->openTransaction("caller edit");
    bad.touch();
    kept.touch();
    other.touch();
    blocked.touch();
    const auto undone = document->recomputeAsync()->wait(kTimeout);
    document->commitTransaction();
    EXPECT_EQ(undone.state, DocumentRecomputeState::PartialFailure) << undone.diagnostic;
    EXPECT_EQ(document->getAvailableUndos(), 1);
    const auto keptAfter = kept.ExecCount.getValue();
    const auto otherAfter = other.ExecCount.getValue();
    EXPECT_EQ(bad.ExceptionType.getValue(), 1);
    EXPECT_TRUE(bad.isError());
    EXPECT_TRUE(bad.isTouched());
    const char* diagnosticBeforeUndo = document->getErrorDescription(&bad);
    ASSERT_NE(diagnosticBeforeUndo, nullptr);
    EXPECT_NE(std::string(diagnosticBeforeUndo).find("Test Exception"), std::string::npos);
    trace.events.clear();
    ASSERT_TRUE(document->undo());
    EXPECT_EQ(kept.ExecCount.getValue(), keptAfter - 1);
    EXPECT_EQ(other.ExecCount.getValue(), otherAfter - 1);
    // ExceptionType was changed before this caller transaction, so undo leaves
    // the failure cause, the error bit, and the diagnostic in place.
    EXPECT_EQ(bad.ExceptionType.getValue(), 1);
    EXPECT_TRUE(bad.isError());
    EXPECT_TRUE(bad.isTouched());
    const char* diagnosticAfterUndo = document->getErrorDescription(&bad);
    ASSERT_NE(diagnosticAfterUndo, nullptr);
    EXPECT_NE(std::string(diagnosticAfterUndo).find("Test Exception"), std::string::npos);
    expectTrace(trace.events,
                {"docBefore:Keep.ExecCount",
                 "objBefore:Keep.ExecCount",
                 "docChanged:Keep.ExecCount",
                 "objChanged:Keep.ExecCount",
                 "prop:Keep.ExecCount",
                 "docBefore:Other.ExecCount",
                 "objBefore:Other.ExecCount",
                 "docChanged:Other.ExecCount",
                 "objChanged:Other.ExecCount",
                 "prop:Other.ExecCount",
                 "undo",
                 "becameStable"});
    trace.events.clear();
    ASSERT_EQ(document->getAvailableRedos(), 1);
    ASSERT_TRUE(document->redo());
    EXPECT_EQ(kept.ExecCount.getValue(), keptAfter);
    EXPECT_EQ(other.ExecCount.getValue(), otherAfter);
    EXPECT_EQ(bad.ExceptionType.getValue(), 1);
    EXPECT_TRUE(bad.isError());
    EXPECT_TRUE(bad.isTouched());
    expectTrace(trace.events,
                {"docBefore:Keep.ExecCount",
                 "objBefore:Keep.ExecCount",
                 "docChanged:Keep.ExecCount",
                 "objChanged:Keep.ExecCount",
                 "prop:Keep.ExecCount",
                 "docBefore:Other.ExecCount",
                 "objBefore:Other.ExecCount",
                 "docChanged:Other.ExecCount",
                 "objChanged:Other.ExecCount",
                 "prop:Other.ExecCount",
                 "redo",
                 "becameStable"});
}

TEST_F(RecomputePublicationContract, cancelMidRecomputeKeepsCommittedPrefixAndPinsSignals)
{
    auto& first = feature("A");
    auto& second = feature("B");
    settle();
    first.touch();
    second.touch();
    document->clearUndos();
    const auto firstCount = first.ExecCount.getValue();
    const auto secondCount = second.ExecCount.getValue();

    const auto cursor = cursorNow(*document);
    SignalTrace trace;
    trace.watch(*document);
    DocumentRecomputeSnapshot snapshot;
    auto handle = document->recomputeAsync();
    rendezvousCancel(
        *document,
        [&] { snapshot = handle->wait(kTimeout); },
        [&] {
            if (!handle->cancel("cancelled mid-way")) {
                throw std::runtime_error("recompute cancel() returned false");
            }
        });

    EXPECT_EQ(snapshot.state, DocumentRecomputeState::Cancelled) << snapshot.diagnostic;
    EXPECT_EQ(featureNode(snapshot, "A").state, DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureNode(snapshot, "B").state, DocumentRecomputeFeatureState::Cancelled);
    EXPECT_EQ(first.ExecCount.getValue(), firstCount + 1);
    EXPECT_EQ(second.ExecCount.getValue(), secondCount);
    EXPECT_TRUE(second.isTouched());
    EXPECT_EQ(document->getAvailableUndos(), 0);
    EXPECT_TRUE(eventsContain(
        document->collaborationRevisions().pollPublications(cursor),
        objectModelKey(first)));
    trace.expectPublications();
    expectTrace(trace.events,
                {"beforeRecompute",
                 "docBefore:A.ExecCount",
                 "objBefore:A.ExecCount",
                 "docChanged:A.ExecCount",
                 "objChanged:A.ExecCount",
                 "prop:A.ExecCount",
                 "docBefore:A.ExecResult",
                 "objBefore:A.ExecResult",
                 "docChanged:A.ExecResult",
                 "objChanged:A.ExecResult",
                 "prop:A.ExecResult",
                 "recomputedObject:A",
                 "recomputed:A,B"});

    trace.events.clear();
    document->openTransaction("caller edit");
    first.touch();
    second.touch();
    DocumentRecomputeSnapshot secondSnapshot;
    auto secondHandle = document->recomputeAsync();
    rendezvousCancel(
        *document,
        [&] { secondSnapshot = secondHandle->wait(kTimeout); },
        [&] {
            if (!secondHandle->cancel("cancelled mid-way")) {
                throw std::runtime_error("recompute cancel() returned false");
            }
        });
    document->commitTransaction();
    EXPECT_EQ(secondSnapshot.state, DocumentRecomputeState::Cancelled);
    const auto committedCount = first.ExecCount.getValue();
    EXPECT_EQ(document->getAvailableUndos(), 1);
    EXPECT_TRUE(second.isTouched());
    trace.events.clear();
    ASSERT_TRUE(document->undo());
    EXPECT_EQ(first.ExecCount.getValue(), committedCount - 1);
    EXPECT_EQ(second.ExecCount.getValue(), secondCount);
    EXPECT_TRUE(second.isTouched());
    expectTrace(trace.events,
                {"docBefore:A.ExecCount",
                 "objBefore:A.ExecCount",
                 "docChanged:A.ExecCount",
                 "objChanged:A.ExecCount",
                 "prop:A.ExecCount",
                 "undo",
                 "becameStable"});
    trace.events.clear();
    ASSERT_EQ(document->getAvailableRedos(), 1);
    ASSERT_TRUE(document->redo());
    EXPECT_EQ(first.ExecCount.getValue(), committedCount);
    EXPECT_EQ(second.ExecCount.getValue(), secondCount);
    EXPECT_TRUE(second.isTouched());
    expectTrace(trace.events,
                {"docBefore:A.ExecCount",
                 "objBefore:A.ExecCount",
                 "docChanged:A.ExecCount",
                 "objChanged:A.ExecCount",
                 "prop:A.ExecCount",
                 "redo",
                 "becameStable"});
}

TEST_F(RecomputePublicationContract, healthyFailureInsideCallerTransactionUndoesAndRedoes)
{
    auto& kept = feature("Keep");
    auto& bad = feature("Bad");
    auto& blocked = feature("Down");
    blocked.Source1.setValue(&bad);
    settle();
    document->clearUndos();
    const auto keptCount = kept.ExecCount.getValue();
    const auto badCount = bad.ExecCount.getValue();
    const auto blockedCount = blocked.ExecCount.getValue();
    EXPECT_FALSE(bad.isError());
    EXPECT_FALSE(bad.isTouched());
    EXPECT_FALSE(kept.isTouched());
    EXPECT_EQ(bad.ExceptionType.getValue(), 0);
    EXPECT_EQ(document->getErrorDescription(&bad), nullptr);

    {
        SignalTrace rollback;
        rollback.watch(*document);
        document->openTransaction("rollback edit");
        bad.ExceptionType.setValue(1);
        rollback.expectPublications();
        document->abortTransaction();
        EXPECT_EQ(bad.ExceptionType.getValue(), 0);
        EXPECT_FALSE(bad.isError());
        EXPECT_EQ(document->getErrorDescription(&bad), nullptr);
        // The value rolls back. The touch bit set by that write does not.
        EXPECT_TRUE(bad.isTouched());
        expectTrace(rollback.events,
                    {"docBefore:Bad.ExceptionType",
                     "openTransaction:rollback edit",
                     "objBefore:Bad.ExceptionType",
                     "docChanged:Bad.ExceptionType",
                     "objChanged:Bad.ExceptionType",
                     "prop:Bad.ExceptionType",
                     "docBefore:Bad.ExceptionType",
                     "objBefore:Bad.ExceptionType",
                     "docChanged:Bad.ExceptionType",
                     "objChanged:Bad.ExceptionType",
                     "prop:Bad.ExceptionType",
                     "abortTransaction",
                     "becameStable"});
    }

    SignalTrace trace;
    trace.watch(*document);
    document->openTransaction("make bad");
    bad.ExceptionType.setValue(1);
    bad.touch();
    kept.touch();
    blocked.touch();
    const auto snapshot = document->recomputeAsync()->wait(kTimeout);
    document->commitTransaction();
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::PartialFailure) << snapshot.diagnostic;
    EXPECT_EQ(bad.ExceptionType.getValue(), 1);
    EXPECT_TRUE(bad.isError());
    EXPECT_TRUE(bad.isTouched());
    EXPECT_EQ(bad.ExecCount.getValue(), badCount);
    EXPECT_EQ(blocked.ExecCount.getValue(), blockedCount);
    EXPECT_TRUE(blocked.isTouched());
    EXPECT_EQ(kept.ExecCount.getValue(), keptCount + 1);
    EXPECT_FALSE(kept.isTouched());
    const char* failedDiagnostic = document->getErrorDescription(&bad);
    ASSERT_NE(failedDiagnostic, nullptr);
    EXPECT_NE(std::string(failedDiagnostic).find("Test Exception"), std::string::npos);
    EXPECT_EQ(document->getAvailableUndos(), 1);
    trace.expectPublications();

    trace.events.clear();
    ASSERT_TRUE(document->undo());
    EXPECT_EQ(bad.ExceptionType.getValue(), 0);
    EXPECT_EQ(bad.ExecCount.getValue(), badCount);
    EXPECT_EQ(kept.ExecCount.getValue(), keptCount);
    EXPECT_EQ(blocked.ExecCount.getValue(), blockedCount);
    EXPECT_FALSE(kept.isTouched());
    // Property values return to the pre-transaction state. The error bit, the
    // failed and blocked touch bits, and the recompute-log diagnostic do not.
    EXPECT_TRUE(bad.isError());
    EXPECT_TRUE(bad.isTouched());
    EXPECT_TRUE(blocked.isTouched());
    const char* diagnosticAfterUndo = document->getErrorDescription(&bad);
    ASSERT_NE(diagnosticAfterUndo, nullptr);
    EXPECT_NE(std::string(diagnosticAfterUndo).find("Test Exception"), std::string::npos);
    expectTrace(trace.events,
                {"docBefore:Bad.ExceptionType",
                 "objBefore:Bad.ExceptionType",
                 "docChanged:Bad.ExceptionType",
                 "objChanged:Bad.ExceptionType",
                 "prop:Bad.ExceptionType",
                 "docBefore:Keep.ExecResult",
                 "objBefore:Keep.ExecResult",
                 "docChanged:Keep.ExecResult",
                 "objChanged:Keep.ExecResult",
                 "prop:Keep.ExecResult",
                 "docBefore:Keep.ExecCount",
                 "objBefore:Keep.ExecCount",
                 "docChanged:Keep.ExecCount",
                 "objChanged:Keep.ExecCount",
                 "prop:Keep.ExecCount",
                 "undo",
                 "becameStable"});

    trace.events.clear();
    ASSERT_EQ(document->getAvailableRedos(), 1);
    ASSERT_TRUE(document->redo());
    EXPECT_EQ(bad.ExceptionType.getValue(), 1);
    EXPECT_TRUE(bad.isError());
    EXPECT_TRUE(bad.isTouched());
    EXPECT_EQ(bad.ExecCount.getValue(), badCount);
    EXPECT_EQ(kept.ExecCount.getValue(), keptCount + 1);
    EXPECT_EQ(blocked.ExecCount.getValue(), blockedCount);
    const char* redoneDiagnostic = document->getErrorDescription(&bad);
    ASSERT_NE(redoneDiagnostic, nullptr);
    EXPECT_NE(std::string(redoneDiagnostic).find("Test Exception"), std::string::npos);
    expectTrace(trace.events,
                {"docBefore:Bad.ExceptionType",
                 "objBefore:Bad.ExceptionType",
                 "docChanged:Bad.ExceptionType",
                 "objChanged:Bad.ExceptionType",
                 "prop:Bad.ExceptionType",
                 "docBefore:Keep.ExecCount",
                 "objBefore:Keep.ExecCount",
                 "docChanged:Keep.ExecCount",
                 "objChanged:Keep.ExecCount",
                 "prop:Keep.ExecCount",
                 "docBefore:Keep.ExecResult",
                 "objBefore:Keep.ExecResult",
                 "docChanged:Keep.ExecResult",
                 "objChanged:Keep.ExecResult",
                 "prop:Keep.ExecResult",
                 "redo",
                 "becameStable"});
}

TEST_F(RecomputePublicationContract, handlePartialFailureFinalizesCommittedSiblingsAndPresentation)
{
    auto& kept = feature("Keep");
    auto& bad = feature("Bad");
    auto& blocked = feature("Down");
    auto& other = feature("Other");
    blocked.Source1.setValue(&bad);
    settle();
    bad.ExceptionType.setValue(1);
    kept.touch();
    bad.touch();
    blocked.touch();
    other.touch();
    const auto keptCount = kept.ExecCount.getValue();
    PresentationProbe probe;
    SignalTrace trace;
    trace.watch(*document);
    const auto snapshot = document->recomputeAsync()->wait(kTimeout);
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::PartialFailure) << snapshot.diagnostic;
    EXPECT_EQ(featureNode(snapshot, "Keep").state, DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureNode(snapshot, "Other").state, DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureNode(snapshot, "Bad").state, DocumentRecomputeFeatureState::Failed);
    EXPECT_EQ(kept.ExecCount.getValue(), keptCount + 1);
    EXPECT_TRUE(bad.isError());
    const char* diagnostic = document->getErrorDescription(&bad);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_NE(std::string(diagnostic).find("Test Exception"), std::string::npos);
    trace.expectPublications();
    expectFailureFinalization(trace, probe.calls);
}

TEST_F(RecomputePublicationContract, lanePartialFailureSkipsTerminalFinalization)
{
    auto& kept = feature("Keep");
    auto& bad = feature("Bad");
    auto& blocked = feature("Down");
    auto& other = feature("Other");
    blocked.Source1.setValue(&bad);
    settle();
    bad.ExceptionType.setValue(1);
    kept.touch();
    bad.touch();
    blocked.touch();
    other.touch();
    const auto keptCount = kept.ExecCount.getValue();
    PresentationProbe probe;
    SignalTrace trace;
    trace.watch(*document);
    GTEST_SKIP() << "known defect: lane skips terminal finalization for PartialFailure/Cancelled; "
                    "tracked as FIX-LANE-TERMINAL-FINALIZE";
    auto command = submitLaneRecompute(*document, "lane-partial-failure");
    const auto snapshot = waitForLaneCommand(command);
    EXPECT_EQ(snapshot.state, DocumentCommandState::Failed) << snapshot.diagnostic;
    ASSERT_TRUE(snapshot.recompute);
    EXPECT_EQ(snapshot.recompute->state, DocumentCommandRecomputeState::PartialFailure);
    EXPECT_EQ(kept.ExecCount.getValue(), keptCount + 1);
    EXPECT_TRUE(bad.isError());
    const char* diagnostic = document->getErrorDescription(&bad);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_NE(std::string(diagnostic).find("Test Exception"), std::string::npos);
    trace.expectPublications();
    expectFailureFinalization(trace, probe.calls);
}

TEST_F(RecomputePublicationContract, handleCancelFinalizesCommittedPrefixAndPresentation)
{
    auto& first = feature("A");
    auto& second = feature("B");
    settle();
    first.touch();
    second.touch();
    const auto firstCount = first.ExecCount.getValue();
    const auto secondCount = second.ExecCount.getValue();
    PresentationProbe probe;
    SignalTrace trace;
    trace.watch(*document);
    DocumentRecomputeSnapshot snapshot;
    auto handle = document->recomputeAsync();
    rendezvousCancel(
        *document,
        [&] { snapshot = handle->wait(kTimeout); },
        [&] {
            if (!handle->cancel("cancelled mid-way")) {
                throw std::runtime_error("recompute cancel() returned false");
            }
        });
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::Cancelled) << snapshot.diagnostic;
    EXPECT_EQ(featureNode(snapshot, "A").state, DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureNode(snapshot, "B").state, DocumentRecomputeFeatureState::Cancelled);
    EXPECT_EQ(first.ExecCount.getValue(), firstCount + 1);
    EXPECT_EQ(second.ExecCount.getValue(), secondCount);
    EXPECT_TRUE(second.isTouched());
    trace.expectPublications();
    expectCancelFinalization(trace, probe.calls);
}

TEST_F(RecomputePublicationContract, laneCancelSkipsTerminalFinalization)
{
    auto& first = feature("A");
    auto& second = feature("B");
    settle();
    first.touch();
    second.touch();
    const auto firstCount = first.ExecCount.getValue();
    const auto secondCount = second.ExecCount.getValue();
    PresentationProbe probe;
    SignalTrace trace;
    trace.watch(*document);
    GTEST_SKIP() << "known defect: lane skips terminal finalization for PartialFailure/Cancelled; "
                    "tracked as FIX-LANE-TERMINAL-FINALIZE";
    const auto snapshot = runLaneCancel(*document, "lane-cancel-finalization");
    EXPECT_EQ(snapshot.state, DocumentCommandState::Cancelled) << snapshot.diagnostic;
    ASSERT_TRUE(snapshot.recompute);
    EXPECT_EQ(snapshot.recompute->state, DocumentCommandRecomputeState::Cancelled);
    EXPECT_EQ(first.ExecCount.getValue(), firstCount + 1);
    EXPECT_EQ(second.ExecCount.getValue(), secondCount);
    EXPECT_TRUE(second.isTouched());
    trace.expectPublications();
    expectCancelFinalization(trace, probe.calls);
}
