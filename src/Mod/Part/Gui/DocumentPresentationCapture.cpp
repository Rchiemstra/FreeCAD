// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentPresentationCapture.h"

#include <Gui/PresentationCaptureRegistry.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentWouldBlock.h>
#include <Base/Parameter.h>
#include <Base/Tools.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/Tools.h>

#include "SoBrepEdgeSet.h"
#include "SoBrepFaceSet.h"
#include "SoBrepPointSet.h"
#include "ViewProviderExt.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoNormal.h>

#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace PartGui
{
namespace
{

void appendCoinVec3Field(const SoMFVec3f& field, std::vector<float>& target)
{
    const int count = field.getNum();
    target.reserve(target.size() + static_cast<std::size_t>(count) * 3U);
    for (int index = 0; index < count; ++index) {
        const SbVec3f& value = field[index];
        target.push_back(value[0]);
        target.push_back(value[1]);
        target.push_back(value[2]);
    }
}

void appendCoinInt32Field(const SoMFInt32& field, std::vector<std::uint32_t>& target)
{
    const int count = field.getNum();
    target.reserve(target.size() + static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        // coordIndex contains Coin face delimiters; the packet uses packed triples.
        if (field[index] >= 0) {
            target.push_back(static_cast<std::uint32_t>(field[index]));
        }
    }
}

std::pair<double, double> defaultPartTessellationDeviation()
{
    ParameterGrp::handle hGrp =
        App::GetApplication().GetUserParameter().GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup(
            "View");
    hGrp = hGrp->GetGroup("Geometry");
    const double deviation = hGrp->GetFloat("Deviation", 0.2);
    const double angularDeflection = hGrp->GetFloat("MeshAngularDeflection", 28.65);
    return {deviation, angularDeflection};
}

// The mutex protects only cache metadata. Concurrent misses may mesh twice;
// private OCC copies keep their mutable triangulations independent.
struct CaptureCache
{
    using Key = std::pair<App::DocumentInstanceId, std::string>;
    struct Entry;
    using Entries = std::list<std::shared_ptr<Entry>>;
    struct Entry
    {
        Key key;
        TopoDS_Shape shape;
        double deviation;
        double angularDeflection;
        bool normalsFromUV;
        std::shared_ptr<const Gui::PresentationRenderBuffer> buffer;
        std::size_t bytes {0};
        std::uint64_t generation {0};
        Entries::iterator position;
    };

    CaptureCache()
        : deleteConnection(App::GetApplication().signalDeleteDocument.connect(
              [this](const App::Document& document) { invalidate(document); }))
        , objectDeleteConnection(App::GetApplication().signalDeletedObject.connect(
              [this](const App::DocumentObject& object) { invalidate(object); }))
        , shapeChangeConnection(App::GetApplication().signalChangedObject.connect(
              [this](const App::DocumentObject& object, const App::Property& property) {
                  if (dynamic_cast<const Part::PropertyPartShape*>(&property)) {
                      invalidate(object);
                  }
              }))
    {}

    void invalidate(const App::Document& document)
    {
        const auto instance = document.collaborationIdentity().instanceId;
        std::lock_guard lock(mutex);
        for (auto it = index.begin(); it != index.end();) {
            const auto entry = (it++)->second;
            if (entry->key.first == instance) {
                erase(entry);
            }
        }
    }

    void invalidate(const App::DocumentObject& object)
    {
        const auto* document = object.getDocument();
        if (!document) {
            return;
        }
        if (!document->containsObject(&object)) {
            // Deferred deletion replay runs after the stable object identity
            // has been removed. Conservatively invalidate the document then.
            invalidate(*document);
            return;
        }
        const Key key {document->collaborationIdentity().instanceId,
                       document->collaborationObjectIdentity(object)};
        std::lock_guard lock(mutex);
        if (const auto found = index.find(key); found != index.end()) {
            // Also cancels publication by captures holding this generation.
            const auto entry = found->second;
            erase(entry);
        }
    }

    void erase(const std::shared_ptr<Entry>& entry)
    {
        ++entry->generation;
        if (entry->buffer) {
            bytes -= entry->bytes;
            entries.erase(entry->position);
        }
        index.erase(entry->key);
    }

    std::mutex mutex;
    Entries entries;  // Published entries, most recently used first.
    std::map<Key, std::shared_ptr<Entry>> index;  // Includes in-flight misses.
    std::size_t bytes {0};
    std::uint64_t tessellations {0};
#if defined(FREECAD_TEST_PART_CAPTURE_CACHE)
    PresentationCaptureMeshHook meshHook;
#endif
    fastsignals::scoped_connection deleteConnection;
    fastsignals::scoped_connection objectDeleteConnection;
    fastsignals::scoped_connection shapeChangeConnection;
};

CaptureCache& captureCache()
{
    // Lazy initialization ensures the App application exists before connecting.
    static CaptureCache cache;
    return cache;
}

template<typename Node>
auto referencedCoinNode()
{
    auto* node = new Node;
    node->ref();
    return std::unique_ptr<Node, void (*)(Node*)>(node, [](Node* value) { value->unref(); });
}

bool capturePartFeaturePresentationRenderBuffer(const App::DocumentObject& object,
                                                const std::string& stableObjectIdentity,
                                                Gui::PresentationRenderBuffer& buffer)
{
    const auto* feature = dynamic_cast<const Part::Feature*>(&object);
    if (!feature || !object.getDocument() || App::DocumentWouldBlock::isGuiThread()) {
        return false;
    }
    const auto [deviation, angularDeflection] = defaultPartTessellationDeviation();
    return capturePartPresentationRenderBuffer(
        *object.getDocument(), stableObjectIdentity, feature->Shape.getValue(),
        deviation, angularDeflection, false, buffer);
}

const Gui::DocumentPresentationCaptureRegistrar partPresentationCaptureRegistrar {
    "Part::Feature",
    capturePartFeaturePresentationRenderBuffer,
};

}  // namespace

bool capturePartPresentationRenderBuffer(const App::DocumentObject& object,
                                        const std::string& stableObjectIdentity,
                                        const TopoDS_Shape& renderedShape,
                                        double deviation,
                                        double angularDeflection,
                                        bool normalsFromUV,
                                        Gui::PresentationRenderBuffer& buffer)
{
    if (!object.getDocument() || App::DocumentWouldBlock::isGuiThread()) {
        buffer = Gui::PresentationRenderBuffer {};
        return false;
    }
    TopoDS_Shape shape = renderedShape;
    if (const auto* feature = dynamic_cast<const Part::Feature*>(&object)) {
        const auto& stored = feature->Shape.getValue();
        if (!stored.IsEqual(shape) && stored.IsPartner(shape)
            && stored.Orientation() == shape.Orientation()) {
            // getRenderedShape reapplies Placement through a fresh TopLoc datum
            // even when the matrix is unchanged. Use the persistent location
            // only when topology, orientation and every transform value agree
            // exactly. Cache validity then still uses IsEqual on the stored
            // shape, including its original Location (never just IsPartner).
            const auto& storedTransform = stored.Location().Transformation();
            const auto& renderedTransform = shape.Location().Transformation();
            bool sameTransform = true;
            for (int row = 1; row <= 3; ++row) {
                for (int column = 1; column <= 4; ++column) {
                    sameTransform &= storedTransform.Value(row, column)
                        == renderedTransform.Value(row, column);
                }
            }
            if (sameTransform) {
                shape.Location(stored.Location());
            }
        }
    }
    return capturePartPresentationRenderBuffer(
        *object.getDocument(), stableObjectIdentity, shape,
        deviation, angularDeflection, normalsFromUV, buffer);
}

bool capturePartPresentationRenderBuffer(const App::Document& document,
                                        const std::string& stableObjectIdentity,
                                        const TopoDS_Shape& shape,
                                        double deviation,
                                        double angularDeflection,
                                        bool normalsFromUV,
                                        Gui::PresentationRenderBuffer& buffer)
{
    buffer = Gui::PresentationRenderBuffer {};
    buffer.stableObjectIdentity = stableObjectIdentity;
    if (App::DocumentWouldBlock::isGuiThread()) {
        return false;
    }

    auto& cache = captureCache();
    const auto documentIdentity = document.collaborationIdentity();
    const CaptureCache::Key key {documentIdentity.instanceId, stableObjectIdentity};
    std::shared_ptr<CaptureCache::Entry> entry;
    std::shared_ptr<const Gui::PresentationRenderBuffer> hit;
    std::uint64_t generation;
#if defined(FREECAD_TEST_PART_CAPTURE_CACHE)
    PresentationCaptureMeshHook meshHook;
#endif
    {
        std::lock_guard lock(cache.mutex);
        const auto identity = App::GetApplication().collaborationRegistry().identity(key.first);
        if (!identity || identity->state != App::DocumentLifecycleState::Live) {
            return false;
        }
        const auto found = cache.index.find(key);
        if (found != cache.index.end()) {
            entry = found->second;
            // IsEqual includes Location and Orientation. Shape property changes
            // separately invalidate even when TShape identity stays unchanged.
            if (entry->shape.IsEqual(shape) && entry->deviation == deviation
                && entry->angularDeflection == angularDeflection
                && entry->normalsFromUV == normalsFromUV) {
                hit = entry->buffer;
                if (hit) {
                    cache.entries.splice(cache.entries.begin(), cache.entries, entry->position);
                }
            }
            else {
                cache.erase(entry);
                entry.reset();
            }
        }
        if (!entry) {
            entry = std::make_shared<CaptureCache::Entry>();
            entry->key = key;
            entry->shape = shape;
            entry->deviation = deviation;
            entry->angularDeflection = angularDeflection;
            entry->normalsFromUV = normalsFromUV;
            cache.index.emplace(key, entry);
        }
        generation = entry->generation;
#if defined(FREECAD_TEST_PART_CAPTURE_CACHE)
        meshHook = cache.meshHook;
#endif
    }
    if (hit) {
        buffer = *hit;
        return true;
    }
    Base::ScopeGuard discardUnpublished([&] {
        std::lock_guard lock(cache.mutex);
        if (entry->generation == generation && !entry->buffer) {
            cache.erase(entry);
        }
    });
    if (Part::Tools::isShapeEmpty(shape)) {
        return !stableObjectIdentity.empty();
    }

    // setupCoinGeometry cleans and remeshes OCC triangulations. Copy outside
    // the cache lock so shared TShapes and different documents can mesh safely.
    const auto meshShape = BRepBuilderAPI_Copy(shape).Shape();
#if defined(FREECAD_TEST_PART_CAPTURE_CACHE)
    if (meshHook) {
        meshHook(stableObjectIdentity);
    }
#endif
    auto coords = referencedCoinNode<SoCoordinate3>();
    auto faces = referencedCoinNode<SoBrepFaceSet>();
    auto normals = referencedCoinNode<SoNormal>();
    auto lines = referencedCoinNode<SoBrepEdgeSet>();
    auto nodes = referencedCoinNode<SoBrepPointSet>();
    {
        std::lock_guard lock(cache.mutex);
        ++cache.tessellations;
    }
    ViewProviderPartExt::setupCoinGeometry(
        meshShape, coords.get(), faces.get(), normals.get(), lines.get(), nodes.get(),
        deviation, angularDeflection, normalsFromUV);

    appendCoinVec3Field(coords->point, buffer.vertices);
    appendCoinVec3Field(normals->vector, buffer.normals);
    appendCoinInt32Field(faces->coordIndex, buffer.indices);
    appendCoinInt32Field(faces->partIndex, buffer.topology);

    // setupCoinGeometry clears output on failure. Never cache that result, so a
    // later capture can retry rather than retaining a failed tessellation.
    if (!buffer.vertices.empty() && !stableObjectIdentity.empty()) {
        auto packed = std::make_shared<const Gui::PresentationRenderBuffer>(buffer);
        const std::size_t bytes = packed->vertices.capacity() * sizeof(float)
            + packed->normals.capacity() * sizeof(float)
            + packed->indices.capacity() * sizeof(std::uint32_t)
            + packed->topology.capacity() * sizeof(std::uint32_t)
            + packed->stableObjectIdentity.capacity();
        std::lock_guard lock(cache.mutex);
        // Do not dereference the document here: it may have closed mid-mesh.
        const auto identity = App::GetApplication().collaborationRegistry().identity(key.first);
        if (identity && identity->state == App::DocumentLifecycleState::Live
            && entry->generation == generation && !entry->buffer
            && bytes <= PresentationCaptureCacheMaxBytes) {
            while (!cache.entries.empty()
                   && (cache.entries.size() >= PresentationCaptureCacheMaxEntries
                       || cache.bytes + bytes > PresentationCaptureCacheMaxBytes)) {
                const auto oldest = cache.entries.back();
                cache.erase(oldest);
            }
            cache.entries.push_front(entry);
            entry->position = cache.entries.begin();
            entry->bytes = bytes;
            entry->buffer = std::move(packed);
            cache.bytes += bytes;
        }
    }
    return !buffer.vertices.empty() || !stableObjectIdentity.empty();
}

#if defined(FREECAD_TEST_PART_CAPTURE_CACHE)
void setPresentationCaptureMeshHook(PresentationCaptureMeshHook hook)
{
    auto& cache = captureCache();
    std::lock_guard lock(cache.mutex);
    cache.meshHook = std::move(hook);
}
#endif

PresentationCaptureCacheStatistics presentationCaptureCacheStatistics()
{
    auto& cache = captureCache();
    std::lock_guard lock(cache.mutex);
    return {cache.entries.size(), cache.bytes, cache.tessellations};
}

}  // namespace PartGui
