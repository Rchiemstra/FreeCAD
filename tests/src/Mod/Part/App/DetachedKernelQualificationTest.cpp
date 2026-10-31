// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DetachedKernelQualification.h"

#include <gtest/gtest.h>

#include <src/App/InitApplication.h>

#include <App/Application.h>
#include <App/Datums.h>
#include <App/Document.h>
#include <App/GeometryWorkerMain.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectGroup.h>
#include <App/ElementMap.h>
#include <App/Expression.h>
#include <App/GenericIsolatedRecompute.h>
#include <App/GeoFeature.h>
#include <App/GeoFeatureGroupExtension.h>
#include <App/Link.h>
#include <App/ObjectIdentifier.h>
#include <App/Part.h>
#include <Mod/Material/App/MaterialManager.h>
#include <App/RecomputeHandle.h>
#include <App/RecomputePhaseTrace.h>
#include <Base/Interpreter.h>
#include <Mod/Part/App/FCBRepAlgoAPI_Common.h>
#include <Mod/Part/App/FCBRepAlgoAPI_Cut.h>
#include <Mod/Part/App/FCBRepAlgoAPI_Fuse.h>
#include <Mod/Part/App/FeaturePartBoolean.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <Mod/Part/App/FeaturePartCommon.h>
#include <Mod/Part/App/FeaturePartCut.h>
#include <Mod/Part/App/FeaturePartFuse.h>
#include <Mod/Part/App/FuzzyHelper.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShapeOpCode.h>
#include <Mod/Part/App/PrismExtension.h>
#include <Mod/Part/App/PropertyTopoShape.h>

#include "PartTestHelpers.h"

#include <Bnd_Box.hxx>
#include <BOPAlgo_Options.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <OSD_Parallel.hxx>
#include <OSD_ThreadPool.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// The isolated-process cost probe reads Linux /proc and spawns with posix_spawn.
// Qualification and harness tests stay outside this guard. Set the macro to 0
// to compile this file without the probe (Windows, macOS, or a scratch check).
#ifndef PRC_M0_HAS_PROCESS_COST_PROBE
# if defined(__linux__)
#  define PRC_M0_HAS_PROCESS_COST_PROBE 1
# else
#  define PRC_M0_HAS_PROCESS_COST_PROBE 0
# endif
#endif

// POSIX signal snapshots need sigaction, sigset_t and sigismember. The default
// follows Linux. Set the macro to 0 to compile this file without those names
// (Windows, macOS, or a scratch check). The signal comparison then skips.
#ifndef PRC_M0_HAS_POSIX_SIGNALS
# if defined(__linux__)
#  define PRC_M0_HAS_POSIX_SIGNALS 1
# else
#  define PRC_M0_HAS_POSIX_SIGNALS 0
# endif
#endif

#if PRC_M0_HAS_POSIX_SIGNALS
# include <csignal>
# include <signal.h>
#endif

#if PRC_M0_HAS_PROCESS_COST_PROBE
# include <fcntl.h>
# include <spawn.h>
# include <sys/wait.h>
extern char** environ;
#endif

using PartQualification::AdmissionRefusal;
using PartQualification::EffectClass;
using PartQualification::QualificationField;
using PartQualification::QualificationProbe;
using PartQualification::qualifyForDetachedExecution;

namespace
{

constexpr int kWarmup = 1;
constexpr int kRepetitions = 3;
constexpr int kWideBoxes = 32;

struct FrozenBoxInputs
{
    double length {0};
    double width {0};
    double height {0};
    double confusion {0};
    Base::Placement placement;
};

struct BoxKernelResult
{
    bool succeeded {false};
    std::string diagnostic;
    TopoDS_Shape shape;
};

struct VertexPoint
{
    double x {0};
    double y {0};
    double z {0};
};

struct Postcondition
{
    bool error {false};
    std::string status;
    std::string diagnostic;
    bool hasPlacement {false};
    Base::Placement placement;
    bool hasDimensions {false};
    double length {0};
    double width {0};
    double height {0};
    std::string materialUuid;
    std::vector<std::string> dynamicNames;
    bool hasShape {false};
    double volume {0};
    int solids {0};
    int faces {0};
    int edges {0};
    int vertices {0};
    std::vector<VertexPoint> points;
    std::vector<std::string> elementMap;
    std::string elementMapVersion;
    bool isLink {false};
    std::string linkedName;
    double scale {1};
    Base::Vector3d scaleVector {1, 1, 1};
    int historySize {-1};
    bool hasRefine {false};
    bool refine {false};
};

struct PhaseSample
{
    std::int64_t captureNs {0};
    std::int64_t executeNs {0};
    std::int64_t transferNs {0};
    std::int64_t commitNs {0};
    std::int64_t wallNs {0};
};

std::filesystem::path costPath()
{
    if (const char* env = std::getenv("PRC_M0_COST_LOG")) {
        if (env[0] != '\0') {
            return env;
        }
    }
    return std::filesystem::temp_directory_path() / "prc-m0-2a-costs.txt";
}

void appendCost(const std::string& line)
{
    const auto path = costPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::app);
    ASSERT_TRUE(static_cast<bool>(out)) << path;
    out << line << '\n';
}

std::int64_t elapsedNs(std::chrono::steady_clock::time_point started)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
}

std::int64_t medianOf(std::vector<std::int64_t> values)
{
    EXPECT_FALSE(values.empty());
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::string milliseconds(std::int64_t nanoseconds)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << (static_cast<double>(nanoseconds) / 1e6);
    return out.str();
}

std::int64_t phaseSum(const App::RecomputePhaseTraceRecord& record, App::RecomputePhase phase)
{
    const auto index = static_cast<std::size_t>(phase);
    std::int64_t total = 0;
    for (const auto& object : record.objects) {
        for (const auto& span : object.attempts[index]) {
            total += span.endNs - span.startNs;
        }
    }
    return total;
}

void expectNoTransfer(const App::RecomputePhaseTraceRecord& record)
{
    const auto index = static_cast<std::size_t>(App::RecomputePhase::Transfer);
    for (const auto& object : record.objects) {
        EXPECT_TRUE(object.attempts[index].empty()) << object.name;
    }
}

Base::Placement rotatedPlacement()
{
    return {Base::Vector3d(4, 5, 6), Base::Rotation(Base::Vector3d(0, 1, 0), 0.4)};
}

BoxKernelResult executeBoxKernel(const FrozenBoxInputs& inputs)
{
    // Mirrors Part::Box::execute dimension order and MakeBox call. No App object.
    if (inputs.length < inputs.confusion) {
        return {false, "Length of box too small", {}};
    }
    if (inputs.width < inputs.confusion) {
        return {false, "Width of box too small", {}};
    }
    if (inputs.height < inputs.confusion) {
        return {false, "Height of box too small", {}};
    }
    try {
        BRepPrimAPI_MakeBox box(inputs.length, inputs.width, inputs.height);
        return {true, {}, box.Shape()};
    }
    catch (const Standard_Failure& error) {
        const char* text = error.what();
        return {false, text ? text : "OCC failure", {}};
    }
}

FrozenBoxInputs freezeBox(const Part::Box& box)
{
    return {box.Length.getValue(),
            box.Width.getValue(),
            box.Height.getValue(),
            Precision::Confusion(),
            box.Placement.getValue()};
}

class RecomputeStatus
{
public:
    explicit RecomputeStatus(App::DocumentObject& object)
        : _object(object)
    {
        _object.setStatus(App::Recompute, true);
    }

    ~RecomputeStatus()
    {
        _object.setStatus(App::Recompute, false);
    }

private:
    App::DocumentObject& _object;
};

Data::ElementMapPtr seededElementMap()
{
    auto map = std::make_shared<Data::ElementMap>();
    const struct
    {
        const char* kind;
        int count;
    } rows[] = {{"Face", 6}, {"Edge", 12}, {"Vertex", 8}};
    for (const auto& row : rows) {
        for (int index = 1; index <= row.count; ++index) {
            const std::string text = std::string("Qual") + row.kind + std::to_string(index);
            map->setElementName(
                Data::IndexedName(row.kind, index),
                Data::MappedName(text),
                /*masterTag=*/1);
        }
    }
    return map;
}

void installSeededMap(Part::Feature& feature)
{
    Part::TopoShape shape = feature.Shape.getShape();
    shape.Tag = feature.getID();
    shape.resetElementMap(seededElementMap());
    RecomputeStatus status(feature);
    feature.Shape.setValue(shape);
}

void publishKernel(Part::Box& box, const TopoDS_Shape& raw)
{
    Part::TopoShape shape;
    shape.setShape(raw, /*resetElementMap=*/true);
    shape.Tag = box.getID();
    shape.resetElementMap(seededElementMap());
    RecomputeStatus status(box);
    box.Shape.setValue(shape);
}

void applyRawShape(Part::Box& box, const TopoDS_Shape& raw)
{
    RecomputeStatus status(box);
    box.Shape.setValue(raw, /*resetElementMap=*/false);
}

int countShapes(const TopoDS_Shape& shape, TopAbs_ShapeEnum type)
{
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> shapes;
    TopExp::MapShapes(shape, type, shapes);
    return shapes.Extent();
}

std::vector<VertexPoint> verticesOf(const TopoDS_Shape& shape)
{
    std::vector<VertexPoint> points;
    if (shape.IsNull()) {
        return points;
    }
    for (TopExp_Explorer explorer(shape, TopAbs_VERTEX); explorer.More(); explorer.Next()) {
        const auto point = BRep_Tool::Pnt(TopoDS::Vertex(explorer.Current()));
        points.push_back({point.X(), point.Y(), point.Z()});
    }
    return points;
}

void expectVerticesNear(const std::vector<VertexPoint>& left, const std::vector<VertexPoint>& right)
{
    auto first = left;
    auto second = right;
    ASSERT_EQ(first.size(), second.size());
    const auto less = [](const VertexPoint& lhs, const VertexPoint& rhs) {
        return std::tie(lhs.x, lhs.y, lhs.z) < std::tie(rhs.x, rhs.y, rhs.z);
    };
    std::sort(first.begin(), first.end(), less);
    std::sort(second.begin(), second.end(), less);
    for (std::size_t index = 0; index < first.size(); ++index) {
        EXPECT_NEAR(first[index].x, second[index].x, 1e-7);
        EXPECT_NEAR(first[index].y, second[index].y, 1e-7);
        EXPECT_NEAR(first[index].z, second[index].z, 1e-7);
    }
}

void expectVerticesNear(const TopoDS_Shape& left, const TopoDS_Shape& right)
{
    expectVerticesNear(verticesOf(left), verticesOf(right));
}

void expectBoxTopology(const TopoDS_Shape& shape)
{
    ASSERT_FALSE(shape.IsNull());
    BRepCheck_Analyzer analyzer(shape);
    EXPECT_TRUE(analyzer.IsValid());
    EXPECT_EQ(countShapes(shape, TopAbs_SOLID), 1);
    EXPECT_EQ(countShapes(shape, TopAbs_FACE), 6);
    EXPECT_EQ(countShapes(shape, TopAbs_EDGE), 12);
    EXPECT_EQ(countShapes(shape, TopAbs_VERTEX), 8);
    EXPECT_NEAR(PartTestHelpers::getVolume(shape), 6000.0, 1e-4);
}

std::vector<std::string> elementMapText(const Part::TopoShape& shape)
{
    std::vector<std::string> lines;
    for (const auto& element : shape.getElementMap()) {
        lines.push_back(element.index.toString() + "=" + element.name.toString());
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

// StringHasher ids (`:H<hex>:<hex>`) are allocated per document. Two documents
// that build the same shape share the indexed name and the opcode text, not
// those ids.
std::string canonicalElementLine(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == ':' && index + 1 < text.size() && text[index + 1] == 'H') {
            std::size_t cursor = index + 2;
            while (cursor < text.size()
                   && std::isxdigit(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
            if (cursor > index + 2) {
                if (cursor < text.size() && text[cursor] == ':') {
                    std::size_t value = cursor + 1;
                    while (value < text.size()
                           && std::isxdigit(static_cast<unsigned char>(text[value]))) {
                        ++value;
                    }
                    if (value > cursor + 1) {
                        cursor = value;
                    }
                }
                index = cursor;
                continue;
            }
        }
        out.push_back(text[index]);
        ++index;
    }
    return out;
}

std::vector<std::string> canonicalElementMap(const std::vector<std::string>& lines)
{
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (const auto& line : lines) {
        out.push_back(canonicalElementLine(line));
    }
    std::sort(out.begin(), out.end());
    return out;
}

Postcondition capturePostcondition(const App::DocumentObject& object)
{
    Postcondition value;
    value.error = object.isError();
    const char* status = object.getStatusString();
    value.status = status ? status : "";
    if (object.getDocument()) {
        const char* diagnostic = object.getDocument()->getErrorDescription(&object);
        value.diagnostic = diagnostic ? diagnostic : "";
    }
    value.dynamicNames = object.getDynamicPropertyNames();
    std::sort(value.dynamicNames.begin(), value.dynamicNames.end());
    if (const auto* geo = dynamic_cast<const App::GeoFeature*>(&object)) {
        value.hasPlacement = true;
        value.placement = geo->Placement.getValue();
        value.elementMapVersion = geo->_ElementMapVersion.getStrValue();
    }
    if (const auto* box = dynamic_cast<const Part::Box*>(&object)) {
        value.hasDimensions = true;
        value.length = box->Length.getValue();
        value.width = box->Width.getValue();
        value.height = box->Height.getValue();
    }
    if (const auto* feature = dynamic_cast<const Part::Feature*>(&object)) {
        const auto& topo = feature->Shape.getShape();
        const auto& shape = topo.getShape();
        value.hasShape = !shape.IsNull();
        if (value.hasShape) {
            value.volume = PartTestHelpers::getVolume(shape);
            value.solids = countShapes(shape, TopAbs_SOLID);
            value.faces = countShapes(shape, TopAbs_FACE);
            value.edges = countShapes(shape, TopAbs_EDGE);
            value.vertices = countShapes(shape, TopAbs_VERTEX);
            value.points = verticesOf(shape);
        }
        value.elementMap = elementMapText(topo);
        value.materialUuid = feature->ShapeMaterial.getValue().getUUID().toStdString();
    }
    if (const auto* boolean = dynamic_cast<const Part::Boolean*>(&object)) {
        value.historySize = boolean->History.getSize();
        value.hasRefine = true;
        value.refine = boolean->Refine.getValue();
    }
    if (const auto* link = dynamic_cast<const App::Link*>(&object)) {
        value.isLink = true;
        value.scale = link->Scale.getValue();
        value.scaleVector = link->getScaleVector();
        value.hasPlacement = true;
        value.placement = link->Placement.getValue();
        App::DocumentObject* linked = link->getLinkedObject(/*recurse=*/false);
        if (!linked || linked == link) {
            linked = link->LinkedObject.getValue();
        }
        if (linked && linked->getNameInDocument()) {
            value.linkedName = linked->getNameInDocument();
        }
    }
    return value;
}

// Link objects do not own a Shape. The serial output is the resolved topo shape
// plus the link's placement, status and diagnostic, so the same comparator
// covers naming as well as those fields.
Postcondition captureLinkOutput(const App::Link& link)
{
    auto value = capturePostcondition(link);
    const auto resolved = Part::Feature::getTopoShape(&link, Part::ShapeOption::NoFlag);
    const auto& shape = resolved.getShape();
    value.hasShape = !shape.IsNull();
    if (value.hasShape) {
        value.volume = PartTestHelpers::getVolume(shape);
        value.solids = countShapes(shape, TopAbs_SOLID);
        value.faces = countShapes(shape, TopAbs_FACE);
        value.edges = countShapes(shape, TopAbs_EDGE);
        value.vertices = countShapes(shape, TopAbs_VERTEX);
        value.points = verticesOf(shape);
    }
    value.elementMap = elementMapText(resolved);
    return value;
}

void expectEqualPostcondition(const Postcondition& left,
                              const Postcondition& right,
                              bool compareDynamicNames = true,
                              bool compareStatus = true,
                              bool compareElementMap = true)
{
    EXPECT_EQ(left.error, right.error);
    if (compareStatus) {
        EXPECT_EQ(left.status, right.status);
    }
    EXPECT_EQ(left.diagnostic, right.diagnostic);
    EXPECT_EQ(left.hasPlacement, right.hasPlacement);
    if (left.hasPlacement && right.hasPlacement) {
        EXPECT_TRUE(left.placement.isSame(right.placement, 1e-9));
    }
    EXPECT_EQ(left.hasDimensions, right.hasDimensions);
    if (left.hasDimensions && right.hasDimensions) {
        EXPECT_NEAR(left.length, right.length, 1e-9);
        EXPECT_NEAR(left.width, right.width, 1e-9);
        EXPECT_NEAR(left.height, right.height, 1e-9);
    }
    EXPECT_EQ(left.materialUuid, right.materialUuid);
    if (compareDynamicNames) {
        EXPECT_EQ(left.dynamicNames, right.dynamicNames);
    }
    EXPECT_EQ(left.hasShape, right.hasShape);
    EXPECT_NEAR(left.volume, right.volume, 1e-4);
    EXPECT_EQ(left.solids, right.solids);
    EXPECT_EQ(left.faces, right.faces);
    EXPECT_EQ(left.edges, right.edges);
    EXPECT_EQ(left.vertices, right.vertices);
    expectVerticesNear(left.points, right.points);
    if (compareElementMap) {
        EXPECT_EQ(canonicalElementMap(left.elementMap), canonicalElementMap(right.elementMap));
    }
    EXPECT_EQ(left.elementMapVersion, right.elementMapVersion);
    EXPECT_EQ(left.isLink, right.isLink);
    EXPECT_EQ(left.linkedName, right.linkedName);
    EXPECT_NEAR(left.scale, right.scale, 1e-12);
    EXPECT_NEAR(left.scaleVector.x, right.scaleVector.x, 1e-12);
    EXPECT_NEAR(left.scaleVector.y, right.scaleVector.y, 1e-12);
    EXPECT_NEAR(left.scaleVector.z, right.scaleVector.z, 1e-12);
    EXPECT_EQ(left.historySize, right.historySize);
    EXPECT_EQ(left.hasRefine, right.hasRefine);
    if (left.hasRefine && right.hasRefine) {
        EXPECT_EQ(left.refine, right.refine);
    }
}

void expectVenueEqual(const Part::Box& left,
                      const Part::Box& right,
                      bool compareElementMap = true)
{
    expectEqualPostcondition(capturePostcondition(left),
                             capturePostcondition(right),
                             /*compareDynamicNames=*/true,
                             /*compareStatus=*/true,
                             compareElementMap);
    expectBoxTopology(left.Shape.getValue());
    expectBoxTopology(right.Shape.getValue());
    expectVerticesNear(left.Shape.getValue().Located(TopLoc_Location()),
                       right.Shape.getValue().Located(TopLoc_Location()));
    EXPECT_FALSE(left.Placement.getValue().getRotation().isIdentity());
    EXPECT_FALSE(right.Placement.getValue().getRotation().isIdentity());
    if (compareElementMap) {
        EXPECT_FALSE(elementMapText(left.Shape.getShape()).empty());
        EXPECT_FALSE(elementMapText(right.Shape.getShape()).empty());
    }
}

bool legacyIsolatedPlanDetaches(App::Document& document, App::DocumentObject& object)
{
    const auto request = App::Internal::makeGenericIsolatedRecomputeRequest(
        document,
        object,
        /*recursive=*/false,
        /*preserveLegacyRevisionSemantics=*/true,
        /*ownerThreadExecution=*/false);
    if (request.features.size() != 1) {
        return false;
    }
    return !request.features.front().ownerThreadExecution;
}

Part::Box* addBox(App::Document& document,
                  const char* name,
                  double length = 10,
                  double width = 20,
                  double height = 30)
{
    auto* box = document.addObject<Part::Box>(name);
    EXPECT_NE(box, nullptr);
    if (!box) {
        return nullptr;
    }
    box->Length.setValue(length);
    box->Width.setValue(width);
    box->Height.setValue(height);
    return box;
}

const QualificationField* findField(const PartQualification::QualificationField* fields,
                                    std::size_t count,
                                    const char* name)
{
    for (std::size_t index = 0; index < count; ++index) {
        if (std::strcmp(fields[index].name, name) == 0) {
            return &fields[index];
        }
    }
    return nullptr;
}

class CloseDocument
{
public:
    std::string name;

    ~CloseDocument()
    {
        if (!name.empty() && App::GetApplication().getDocument(name.c_str())) {
            App::GetApplication().closeDocument(name.c_str());
        }
    }
};

class TempArchive
{
public:
    explicit TempArchive(std::filesystem::path path)
        : _path(std::move(path))
    {
        std::error_code error;
        std::filesystem::create_directories(_path.parent_path(), error);
        std::filesystem::remove(_path, error);
    }

    ~TempArchive()
    {
        std::error_code error;
        std::filesystem::remove(_path, error);
        auto backup = _path;
        backup.replace_extension(".FCBak");
        std::filesystem::remove(backup, error);
        std::filesystem::remove(std::filesystem::path(_path.string() + ".FCBak"), error);
    }

    const std::filesystem::path& path() const
    {
        return _path;
    }

private:
    std::filesystem::path _path;
};

bool runPython(const char* code)
{
    Base::PyGILStateLocker lock;
    PyObject* mainModule = PyImport_AddModule("__main__");
    PyObject* globals = PyModule_GetDict(mainModule);
    PyObject* result = PyRun_String(code, Py_file_input, globals, globals);
    if (!result) {
        PyErr_Print();
        return false;
    }
    Py_DECREF(result);
    return true;
}

int proxyCount(App::DocumentObject& object)
{
    Base::PyGILStateLocker lock;
    PyObject* py = object.getPyObject();
    PyObject* proxy = PyObject_GetAttrString(py, "Proxy");
    PyObject* count = proxy ? PyObject_GetAttrString(proxy, "count") : nullptr;
    const int value = count ? static_cast<int>(PyLong_AsLong(count)) : -1;
    Py_XDECREF(count);
    Py_XDECREF(proxy);
    Py_XDECREF(py);
    return value;
}

#if PRC_M0_HAS_PROCESS_COST_PROBE

std::int64_t emptyFreeCadCmdStartupNs(const char* command)
{
    const std::filesystem::path home("/home/rch/Documents/code/agents/PRC/logs/m0-2a-r3-empty");
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(home / "cache");
    std::filesystem::path exe =
        std::filesystem::path(App::Application::getHomePath()) / "bin" / "FreeCADCmd";
    if (!std::filesystem::exists(exe)) {
        exe = std::filesystem::path(App::Application::getHomePath()) / "FreeCADCmd";
    }
    std::vector<std::string> storage;
    for (char** item = environ; item && *item; ++item) {
        const std::string entry(*item);
        if (entry.rfind("FREECAD_USER_HOME=", 0) == 0 || entry.rfind("HOME=", 0) == 0
            || entry.rfind("XDG_CACHE_HOME=", 0) == 0) {
            continue;
        }
        storage.push_back(entry);
    }
    storage.push_back("FREECAD_USER_HOME=" + home.string());
    storage.push_back("HOME=" + home.string());
    storage.push_back("XDG_CACHE_HOME=" + (home / "cache").string());
    std::vector<char*> envp;
    envp.reserve(storage.size() + 1);
    for (auto& entry : storage) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    const std::string exeString = exe.string();
    char* argv[] = {const_cast<char*>(exeString.c_str()),
                    const_cast<char*>("-c"),
                    const_cast<char*>(command),
                    nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const auto started = std::chrono::steady_clock::now();
    const int spawned =
        posix_spawn(&pid, exeString.c_str(), &actions, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        ADD_FAILURE() << "empty FreeCADCmd spawn failed: " << exeString << " errno " << spawned;
        return 0;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const auto elapsed = elapsedNs(started);
    EXPECT_TRUE(WIFEXITED(status)) << status;
    EXPECT_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : status, 0);
    return elapsed;
}

/** Stamps from one isolated FreeCADCmd, nanoseconds after the caller's wall start.
 *  Negative means that file or process was not observed. */
struct ProcessMarks
{
    std::int64_t workspaceNs {-1};
    std::int64_t requestNs {-1};
    std::int64_t spawnNs {-1};
    std::int64_t heartbeatNs {-1};
    std::int64_t partNs {-1};
    std::int64_t resultNs {-1};
    std::int64_t exitNs {-1};
    std::int64_t publishedNs {-1};
    std::int64_t fingerprintStartNs {-1};
    std::int64_t fingerprintEndNs {-1};
    int pid {-1};
};

std::vector<std::filesystem::path> geometryJobRoots()
{
    std::vector<std::filesystem::path> roots;
    auto push = [&](const std::filesystem::path& base) {
        if (base.empty()) {
            return;
        }
        roots.push_back(base / "geometry-jobs");
        roots.push_back(base / "FreeCAD" / "geometry-jobs");
    };
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache && cache[0] != '\0') {
        push(cache);
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        push(std::filesystem::path(home) / ".cache");
    }
    const auto userCache = App::Application::getUserCachePath();
    if (!userCache.empty()) {
        push(std::filesystem::path(userCache));
    }
    return roots;
}

std::string processComm(int pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(in, name);
    return name;
}

bool processUsesWorkspace(int pid, const std::filesystem::path& workspace)
{
    std::error_code error;
    const auto cwd = std::filesystem::weakly_canonical(
        std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/cwd", error),
        error);
    const auto job = std::filesystem::weakly_canonical(workspace, error);
    if (!error && !cwd.empty() && cwd == job) {
        return true;
    }
    std::ifstream environ("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(environ)),
                           std::istreambuf_iterator<char>());
    return data.find(workspace.string()) != std::string::npos;
}

std::int64_t processRchar(int pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/io");
    std::string line;
    while (std::getline(in, line)) {
        constexpr std::string_view key = "rchar:";
        if (!line.starts_with(key)) {
            continue;
        }
        const char* begin = line.data() + key.size();
        const char* end = line.data() + line.size();
        while (begin < end && *begin == ' ') {
            ++begin;
        }
        std::int64_t value = 0;
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec == std::errc {}) {
            return value;
        }
    }
    return -1;
}

bool processMapsPart(int pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("Mod/Part/Part.so") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::set<int> freeCadCmdPids()
{
    std::set<int> pids;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator("/proc", error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        const auto name = iterator->path().filename().string();
        if (name.empty() || !std::isdigit(static_cast<unsigned char>(name.front()))) {
            continue;
        }
        int pid = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (parsed.ec != std::errc {} || pid <= 0) {
            continue;
        }
        if (processComm(pid) == "FreeCADCmd") {
            pids.insert(pid);
        }
    }
    return pids;
}

struct ProcessWatchState
{
    std::vector<std::filesystem::path> roots;
    std::set<std::string> preexisting;
    std::set<int> preexistingPids;
};

ProcessWatchState prepareIsolatedWatch()
{
    ProcessWatchState state;
    state.roots = geometryJobRoots();
    std::error_code error;
    for (const auto& root : state.roots) {
        for (std::filesystem::directory_iterator iterator(root, error), end;
             !error && iterator != end;
             iterator.increment(error)) {
            state.preexisting.insert(iterator->path().string());
        }
        error.clear();
    }
    state.preexistingPids = freeCadCmdPids();
    return state;
}

ProcessMarks watchIsolatedProcess(const std::chrono::steady_clock::time_point wallStart,
                                  const std::atomic_bool& stop,
                                  const ProcessWatchState& state)
{
    const auto& roots = state.roots;
    const auto& preexisting = state.preexisting;
    const auto& preexistingPids = state.preexistingPids;
    std::error_code error;
    ProcessMarks marks;
    std::filesystem::path workspace;
    std::int64_t rcharBase {-1};
    auto sample = [&](const std::int64_t now) {
        if (workspace.empty()) {
            for (const auto& root : roots) {
                for (std::filesystem::directory_iterator iterator(root, error), end;
                     !error && iterator != end;
                     iterator.increment(error)) {
                    const auto path = iterator->path();
                    const auto name = path.filename().string();
                    if (!name.starts_with("job-") || preexisting.contains(path.string())) {
                        continue;
                    }
                    workspace = path;
                    marks.workspaceNs = now;
                    break;
                }
                error.clear();
                if (!workspace.empty()) {
                    break;
                }
            }
        }
        if (!workspace.empty()) {
            if (marks.requestNs < 0
                && std::filesystem::is_regular_file(workspace / "request.fcg", error)) {
                marks.requestNs = now;
            }
            error.clear();
            if (marks.heartbeatNs < 0
                && std::filesystem::is_regular_file(workspace / "heartbeat", error)) {
                marks.heartbeatNs = now;
            }
            error.clear();
            if (marks.resultNs < 0
                && std::filesystem::is_regular_file(workspace / "result.fcg", error)) {
                marks.resultNs = now;
            }
            error.clear();
        }
        if (marks.publishedNs < 0) {
            for (const auto& root : roots) {
                for (std::filesystem::directory_iterator iterator(root, error), end;
                     !error && iterator != end;
                     iterator.increment(error)) {
                    const auto path = iterator->path();
                    const auto name = path.filename().string();
                    if (!name.starts_with("result-") || preexisting.contains(path.string())) {
                        continue;
                    }
                    marks.publishedNs = now;
                    break;
                }
                error.clear();
                if (marks.publishedNs >= 0) {
                    break;
                }
            }
        }
        // A FreeCADCmd that is not this job (it can appear while the parent
        // fingerprints the binary, before the workspace exists) must not own
        // the spawn and exit stamps. Latch only a process whose cwd or
        // environment is this workspace, and drop it if it exits before the
        // result file exists.
        if (!workspace.empty() && marks.pid < 0) {
            for (const auto pid : freeCadCmdPids()) {
                if (preexistingPids.contains(pid) || !processUsesWorkspace(pid, workspace)) {
                    continue;
                }
                marks.pid = pid;
                marks.spawnNs = now;
                break;
            }
        }
        else if (marks.pid > 0 && marks.exitNs < 0
                 && !std::filesystem::exists("/proc/" + std::to_string(marks.pid))) {
            if (marks.resultNs < 0) {
                marks.pid = -1;
                marks.spawnNs = -1;
            }
            else {
                marks.exitNs = now;
            }
        }
        if (marks.pid > 0 && marks.partNs < 0 && marks.exitNs < 0 && processMapsPart(marks.pid)) {
            marks.partNs = now;
        }
        if (marks.pid > 0 && marks.exitNs < 0 && marks.fingerprintEndNs < 0) {
            const auto rchar = processRchar(marks.pid);
            if (rchar >= 0) {
                if (rcharBase < 0) {
                    rcharBase = rchar;
                }
                const auto read = rchar - rcharBase;
                constexpr std::int64_t fingerprintStartBytes = 32LL << 20;
                constexpr std::int64_t fingerprintEndBytes = 80LL << 20;
                if (marks.fingerprintStartNs < 0 && read >= fingerprintStartBytes) {
                    marks.fingerprintStartNs = now;
                }
                if (read >= fingerprintEndBytes) {
                    marks.fingerprintEndNs = now;
                }
            }
        }
    };
    while (!stop.load(std::memory_order_acquire)) {
        sample(elapsedNs(wallStart));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sample(elapsedNs(wallStart));
    return marks;
}

struct ProcessAccount
{
    std::int64_t captureNs {0};
    std::int64_t parentFingerprintNs {0};
    std::int64_t archiveNs {0};
    std::int64_t spawnNs {0};
    std::int64_t workerStartupNs {0};
    std::int64_t importNs {0};
    std::int64_t executeNs {0};
    std::int64_t teardownNs {0};
    std::int64_t decodeNs {0};
    std::int64_t commitNs {0};
    std::int64_t settleNs {0};
    std::int64_t republishNs {-1};
    std::int64_t afterPublishNs {-1};
    std::int64_t wallNs {0};
};

std::int64_t clampStamp(std::int64_t stamp, std::int64_t wallNs)
{
    if (stamp < 0) {
        return stamp;
    }
    return stamp > wallNs ? wallNs : stamp;
}

ProcessAccount accountProcess(ProcessMarks marks, const PhaseSample& sample)
{
    marks.workspaceNs = clampStamp(marks.workspaceNs, sample.wallNs);
    marks.requestNs = clampStamp(marks.requestNs, sample.wallNs);
    marks.spawnNs = clampStamp(marks.spawnNs, sample.wallNs);
    marks.heartbeatNs = clampStamp(marks.heartbeatNs, sample.wallNs);
    marks.resultNs = clampStamp(marks.resultNs, sample.wallNs);
    marks.exitNs = clampStamp(marks.exitNs, sample.wallNs);
    marks.publishedNs = clampStamp(marks.publishedNs, sample.wallNs);
    marks.fingerprintStartNs = clampStamp(marks.fingerprintStartNs, sample.wallNs);
    marks.fingerprintEndNs = clampStamp(marks.fingerprintEndNs, sample.wallNs);
    ProcessAccount account;
    account.wallNs = sample.wallNs;
    account.captureNs = sample.captureNs;
    account.executeNs = sample.executeNs;
    account.decodeNs = sample.transferNs;
    account.commitNs = sample.commitNs;
    const auto ordered = marks.workspaceNs >= 0 && marks.requestNs >= marks.workspaceNs
        && marks.spawnNs >= marks.requestNs && marks.heartbeatNs >= marks.spawnNs
        && marks.resultNs >= marks.heartbeatNs && marks.exitNs >= marks.resultNs
        && sample.wallNs >= marks.exitNs && sample.captureNs <= marks.workspaceNs
        && sample.executeNs <= marks.resultNs - marks.heartbeatNs;
    EXPECT_TRUE(ordered) << "workspace=" << marks.workspaceNs << " request=" << marks.requestNs
                         << " spawn=" << marks.spawnNs << " heartbeat=" << marks.heartbeatNs
                         << " finger_start=" << marks.fingerprintStartNs
                         << " finger_end=" << marks.fingerprintEndNs
                         << " part=" << marks.partNs << " result=" << marks.resultNs
                         << " exit=" << marks.exitNs << " published=" << marks.publishedNs
                         << " capture=" << sample.captureNs << " execute=" << sample.executeNs
                         << " transfer=" << sample.transferNs << " commit=" << sample.commitNs
                         << " wall=" << sample.wallNs << " pid=" << marks.pid;
    if (!ordered) {
        return account;
    }
    account.parentFingerprintNs = marks.workspaceNs - sample.captureNs;
    account.archiveNs = marks.requestNs - marks.workspaceNs;
    account.spawnNs = marks.spawnNs - marks.requestNs;
    // Pid to first heartbeat: Application::init, then the same binary
    // fingerprint the parent just ran, then the start gate. The 1 ms sample
    // cannot see that boundary; rchar over the interval and the direct
    // fingerprint probe identify the hash.
    account.workerStartupNs = marks.heartbeatNs - marks.spawnNs;
    account.importNs = marks.resultNs - marks.heartbeatNs - sample.executeNs;
    account.teardownNs = marks.exitNs - marks.resultNs;
    auto tail = sample.wallNs - marks.exitNs;
    if (account.decodeNs + account.commitNs > tail) {
        const auto overflow = account.decodeNs + account.commitNs - tail;
        EXPECT_LE(overflow, account.teardownNs);
        account.teardownNs -= overflow;
        tail = account.decodeNs + account.commitNs;
    }
    account.settleNs = tail - account.decodeNs - account.commitNs;
    if (marks.publishedNs >= marks.exitNs && marks.exitNs >= 0 && sample.wallNs >= marks.publishedNs) {
        account.republishNs = marks.publishedNs - marks.exitNs;
        account.afterPublishNs = sample.wallNs - marks.publishedNs;
    }
    const auto sum = account.captureNs + account.parentFingerprintNs + account.archiveNs
        + account.spawnNs + account.workerStartupNs + account.importNs + account.executeNs
        + account.teardownNs + account.decodeNs + account.commitNs + account.settleNs;
    EXPECT_EQ(sum, account.wallNs);
    return account;
}

#endif  // PRC_M0_HAS_PROCESS_COST_PROBE

ParameterGrp::handle booleanPreferences()
{
    return App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Part/Boolean");
}

TopoDS_Shape unorientableSolid()
{
    TopoDS_Shape shape;
    BRep_Builder builder;
    const std::string path =
        App::Application::getHomePath() + "/tests/brepfiles/unorientableSolid.brep";
    EXPECT_TRUE(BRepTools::Read(shape, path.c_str(), builder)) << path;
    return shape;
}

App::DocumentRecomputeSnapshot waitIsolated(App::Document& document, App::DocumentObject& object)
{
    auto handle = document.recomputeAsync(
        {&object},
        /*force=*/true,
        /*options=*/0,
        App::RecomputeVenue::Isolated);
    EXPECT_NE(handle, nullptr);
    if (!handle) {
        return {};
    }
    auto snapshot = handle->wait(std::chrono::seconds(180));
    if (!snapshot.terminal()) {
        static_cast<void>(handle->cancel("m0.2a isolated wait timed out"));
        snapshot = handle->wait(std::chrono::seconds(30));
    }
    return snapshot;
}

TopoDS_Shape deepCopyShape(const TopoDS_Shape& shape)
{
    if (shape.IsNull()) {
        return {};
    }
    BRepBuilderAPI_Copy copier(shape);
    return copier.Shape();
}

std::string serializedGeometry(const TopoDS_Shape& shape)
{
    if (shape.IsNull()) {
        return {};
    }
    std::ostringstream out;
    BRepTools::Write(shape, out, /*theWithTriangles=*/false, /*theWithNormals=*/false,
                     TopTools_FormatVersion_VERSION_1);
    return out.str();
}

std::vector<const void*> tshapeIds(const TopoDS_Shape& shape)
{
    std::vector<const void*> ids;
    if (shape.IsNull()) {
        return ids;
    }
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> shapes;
    TopExp::MapShapes(shape, shapes);
    ids.reserve(static_cast<std::size_t>(shapes.Extent()));
    for (int index = 1; index <= shapes.Extent(); ++index) {
        ids.push_back(shapes(index).TShape().get());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool anyPartner(const TopoDS_Shape& left, const TopoDS_Shape& right)
{
    if (left.IsNull() || right.IsNull()) {
        return false;
    }
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> leftShapes;
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> rightShapes;
    TopExp::MapShapes(left, leftShapes);
    TopExp::MapShapes(right, rightShapes);
    for (int i = 1; i <= leftShapes.Extent(); ++i) {
        for (int j = 1; j <= rightShapes.Extent(); ++j) {
            if (leftShapes(i).IsPartner(rightShapes(j))) {
                return true;
            }
        }
    }
    return false;
}

Part::TopoShape namedCopy(const Part::TopoShape& source)
{
    Part::TopoShape named;
    named.setShape(deepCopyShape(source.getShape()), /*resetElementMap=*/true);
    named.Tag = source.Tag;
    named.setElementMap(source.getElementMap());
    return named;
}

// Feature::copyMaterial is protected. The test applies the same owner-side rule
// through the public ShapeMaterial property. See PartFeature.cpp.
void copyOwnerMaterial(Part::Feature& target, App::DocumentObject* source)
{
    auto* feature = dynamic_cast<Part::Feature*>(source);
    if (!feature) {
        return;
    }
    const auto fallback = Materials::MaterialManager::defaultMaterial();
    if (!fallback) {
        return;
    }
    if (target.ShapeMaterial.getValue().getUUID() != feature->ShapeMaterial.getValue().getUUID()
        && target.ShapeMaterial.getValue().getUUID() == fallback->getUUID()) {
        target.ShapeMaterial.setValue(feature->ShapeMaterial.getValue());
    }
}

enum class BooleanOp
{
    Cut,
    Fuse,
    Common
};

const char* booleanOpcode(BooleanOp kind)
{
    switch (kind) {
        case BooleanOp::Fuse:
            return Part::OpCodes::Fuse;
        case BooleanOp::Common:
            return Part::OpCodes::Common;
        case BooleanOp::Cut:
            return Part::OpCodes::Cut;
    }
    return Part::OpCodes::Cut;
}

struct FrozenOperand
{
    TopoDS_Shape shape;
    Part::TopoShape named;
    std::string label;
};

struct FrozenBoolean
{
    BooleanOp kind {BooleanOp::Cut};
    bool checkModel {true};
    double fuzzy {1};
    double confusion {0};
    FrozenOperand base;
    FrozenOperand tool;
};

struct BooleanKernelResult
{
    bool succeeded {false};
    std::string diagnostic;
    TopoDS_Shape shape;
    std::unique_ptr<FCBRepAlgoAPI_BooleanOperation> operation;
    bool validationRejected {false};
    Part::TopoShape named;
};

std::unique_ptr<FCBRepAlgoAPI_BooleanOperation> newBooleanOperation(BooleanOp kind,
                                                                   const TopoDS_Shape& base,
                                                                   const TopoDS_Shape& tool)
{
    switch (kind) {
        case BooleanOp::Fuse:
            return std::unique_ptr<FCBRepAlgoAPI_BooleanOperation>(new FCBRepAlgoAPI_Fuse(base, tool));
        case BooleanOp::Common:
            return std::unique_ptr<FCBRepAlgoAPI_BooleanOperation>(new FCBRepAlgoAPI_Common(base, tool));
        case BooleanOp::Cut:
            return std::unique_ptr<FCBRepAlgoAPI_BooleanOperation>(new FCBRepAlgoAPI_Cut(base, tool));
    }
    return {};
}

std::string notDoneText(const TopoDS_Shape& base,
                        const TopoDS_Shape& tool,
                        const std::string& baseLabel,
                        const std::string& toolLabel)
{
    std::ostringstream error;
    error << "Boolean operation failed";
    if (!base.IsNull() && base.ShapeType() != TopAbs_SOLID) {
        error << '\n' << baseLabel << " is not a solid";
    }
    if (!tool.IsNull() && tool.ShapeType() != TopAbs_SOLID) {
        error << '\n' << toolLabel << " is not a solid";
    }
    return error.str();
}

BooleanKernelResult executeBooleanKernel(BooleanOp kind,
                                         const TopoDS_Shape& base,
                                         const TopoDS_Shape& tool,
                                         const std::string& baseLabel,
                                         const std::string& toolLabel)
{
    // Raw FCBRepAlgoAPI_* on frozen copies. CheckModel is not read here.
    if (base.IsNull()) {
        return {false, "Base shape is null", {}, {}};
    }
    if (tool.IsNull()) {
        return {false, "Tool shape is null", {}, {}};
    }
    try {
        auto operation = newBooleanOperation(kind, base, tool);
        if (!operation || !operation->IsDone()) {
            return {false, notDoneText(base, tool, baseLabel, toolLabel), {}, {}};
        }
        TopoDS_Shape shape = operation->Shape();
        if (shape.IsNull()) {
            return {false, "Resulting shape is null", {}, {}};
        }
        BooleanKernelResult result;
        result.succeeded = true;
        result.shape = shape;
        result.operation = std::move(operation);
        return result;
    }
    catch (const Standard_Failure& error) {
        const char* text = error.GetMessageString();
        return {false, text ? text : "OCC failure", {}, {}};
    }
    catch (const Base::Exception& error) {
        return {false, error.what(), {}, {}};
    }
    catch (const std::exception& error) {
        return {false, error.what(), {}, {}};
    }
}

FrozenBoolean freezeBoolean(const Part::Boolean& boolean, BooleanOp kind)
{
    const auto options = Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform;
    const auto baseShape = Part::Feature::getTopoShape(boolean.Base.getValue(), options);
    const auto toolShape = Part::Feature::getTopoShape(boolean.Tool.getValue(), options);
    FrozenBoolean frozen;
    frozen.kind = kind;
    frozen.checkModel = booleanPreferences()->GetBool("CheckModel", true);
    frozen.fuzzy = Part::FuzzyHelper::getBooleanFuzzy();
    frozen.confusion = Precision::Confusion();
    frozen.base.named = namedCopy(baseShape);
    frozen.base.shape = frozen.base.named.getShape();
    const char* baseLabel = boolean.Base.getValue()->Label.getValue();
    frozen.base.label = baseLabel ? baseLabel : "";
    frozen.tool.named = namedCopy(toolShape);
    frozen.tool.shape = frozen.tool.named.getShape();
    const char* toolLabel = boolean.Tool.getValue()->Label.getValue();
    frozen.tool.label = toolLabel ? toolLabel : "";
    return frozen;
}

bool frozenCheckRejects(const FrozenBoolean& frozen, const TopoDS_Shape& shape)
{
    if (!frozen.checkModel || shape.IsNull()) {
        return false;
    }
    BRepCheck_Analyzer checker(shape);
    return !checker.IsValid();
}

void publishBooleanShape(Part::Boolean& boolean, const Part::TopoShape& named)
{
    RecomputeStatus status(boolean);
    boolean.Shape.setValue(named);
    copyOwnerMaterial(boolean, boolean.Base.getValue());
    boolean.purgeTouched();
}

Part::TopoShape historyNamedShape(BooleanKernelResult& result, const FrozenBoolean& frozen)
{
    Part::TopoShape named;
    std::vector<Part::TopoShape> sources {frozen.base.named, frozen.tool.named};
    named.makeElementShape(*result.operation, sources, booleanOpcode(frozen.kind));
    return named;
}

void completeBooleanComputation(BooleanKernelResult& kernel, const FrozenBoolean& frozen)
{
    if (!kernel.succeeded || !kernel.operation) {
        return;
    }
    if (frozenCheckRejects(frozen, kernel.shape)) {
        kernel.validationRejected = true;
        return;
    }
    kernel.named = historyNamedShape(kernel, frozen);
}

struct SignalShot
{
    int signo {0};
    int flags {0};
    void* handler {nullptr};
    bool valid {false};
#if PRC_M0_HAS_POSIX_SIGNALS
    sigset_t mask {};
#endif
};

struct WorkerEnvironment
{
    bool parallelMode {false};
    bool occtThreads {false};
    double fuzzy {0};
    double confusion {0};
    bool checkModel {true};
    int poolThreads {-1};
    int poolDefaultLaunch {-1};
    bool poolInUse {false};
    std::vector<SignalShot> signals;
};

#if PRC_M0_HAS_POSIX_SIGNALS
const int kPartSignalTypes[] = {SIGFPE, SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGBUS, SIGSYS, SIGSEGV};

const char* signalName(int signo)
{
    switch (signo) {
        case SIGFPE:
            return "SIGFPE";
        case SIGHUP:
            return "SIGHUP";
        case SIGINT:
            return "SIGINT";
        case SIGQUIT:
            return "SIGQUIT";
        case SIGILL:
            return "SIGILL";
        case SIGBUS:
            return "SIGBUS";
        case SIGSYS:
            return "SIGSYS";
        case SIGSEGV:
            return "SIGSEGV";
        default:
            return "SIG";
    }
}

bool sameSignalMask(const sigset_t& left, const sigset_t& right)
{
    // sigset_t padding is not part of the handler. Compare membership only.
    for (int signo = 1; signo < NSIG; ++signo) {
        const int leftSet = sigismember(&left, signo);
        const int rightSet = sigismember(&right, signo);
        if (leftSet < 0 || rightSet < 0) {
            continue;
        }
        if (leftSet != rightSet) {
            return false;
        }
    }
    return true;
}

SignalShot captureSignal(int signo)
{
    SignalShot shot;
    shot.signo = signo;
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    if (sigaction(signo, nullptr, &action) == 0) {
        shot.valid = true;
        shot.flags = action.sa_flags;
        shot.mask = action.sa_mask;
        if ((action.sa_flags & SA_SIGINFO) != 0) {
            shot.handler = reinterpret_cast<void*>(action.sa_sigaction);
        }
        else {
            shot.handler = reinterpret_cast<void*>(action.sa_handler);
        }
    }
    return shot;
}
#endif

WorkerEnvironment captureWorkerEnvironment()
{
    WorkerEnvironment value;
    value.parallelMode = BOPAlgo_Options::GetParallelMode() == Standard_True;
    value.occtThreads = OSD_Parallel::ToUseOcctThreads() == Standard_True;
    value.fuzzy = Part::FuzzyHelper::getBooleanFuzzy();
    value.confusion = Precision::Confusion();
    value.checkModel = booleanPreferences()->GetBool("CheckModel", true);
    occ::handle<OSD_ThreadPool> pool = OSD_ThreadPool::DefaultPool();
    if (!pool.IsNull()) {
        value.poolThreads = pool->NbThreads();
        value.poolDefaultLaunch = pool->NbDefaultThreadsToLaunch();
        value.poolInUse = pool->IsInUse();
    }
#if PRC_M0_HAS_POSIX_SIGNALS
    value.signals.reserve(sizeof(kPartSignalTypes) / sizeof(kPartSignalTypes[0]));
    for (const int signo : kPartSignalTypes) {
        value.signals.push_back(captureSignal(signo));
    }
#endif
    return value;
}

void expectWorkerComputationStable(const WorkerEnvironment& before, const WorkerEnvironment& after)
{
    EXPECT_EQ(before.parallelMode, after.parallelMode);
    EXPECT_EQ(before.occtThreads, after.occtThreads);
    EXPECT_DOUBLE_EQ(before.fuzzy, after.fuzzy);
    EXPECT_DOUBLE_EQ(before.confusion, after.confusion);
    EXPECT_EQ(before.checkModel, after.checkModel);
    EXPECT_EQ(before.poolThreads, after.poolThreads);
    EXPECT_EQ(before.poolDefaultLaunch, after.poolDefaultLaunch);
}

std::string describeWorkerEnvironmentDelta(const WorkerEnvironment& before, const WorkerEnvironment& after)
{
    std::string changed;
    auto add = [&](const std::string& name) {
        if (!changed.empty()) {
            changed += ",";
        }
        changed += name;
    };
    if (before.parallelMode != after.parallelMode) {
        add("parallelMode");
    }
    if (before.occtThreads != after.occtThreads) {
        add("occtThreads");
    }
    if (before.fuzzy != after.fuzzy) {
        add("fuzzy");
    }
    if (before.confusion != after.confusion) {
        add("confusion");
    }
    if (before.checkModel != after.checkModel) {
        add("checkModel");
    }
    if (before.poolThreads != after.poolThreads) {
        add("poolThreads");
    }
    if (before.poolDefaultLaunch != after.poolDefaultLaunch) {
        add("poolDefaultLaunch");
    }
    if (before.poolInUse != after.poolInUse) {
        add("poolInUse");
    }
#if PRC_M0_HAS_POSIX_SIGNALS
    const auto count = std::min(before.signals.size(), after.signals.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& left = before.signals[index];
        const auto& right = after.signals[index];
        if (left.valid != right.valid || left.flags != right.flags || left.handler != right.handler
            || !sameSignalMask(left.mask, right.mask)) {
            add(signalName(left.signo));
        }
    }
#endif
    return changed.empty() ? std::string("none") : changed;
}

TopoDS_Shape prismSolid(int sides, double radius, double height, double shiftX)
{
    BRepBuilderAPI_MakePolygon polygon;
    for (int index = 0; index < sides; ++index) {
        const double angle = 6.283185307179586 * static_cast<double>(index) / static_cast<double>(sides);
        polygon.Add(gp_Pnt(shiftX + radius * std::cos(angle), radius * std::sin(angle), 0.0));
    }
    polygon.Close();
    BRepBuilderAPI_MakeFace face(polygon.Wire());
    BRepPrimAPI_MakePrism prism(face.Face(), gp_Vec(0, 0, height));
    return prism.Shape();
}

void expectGuardProvesTheRefusal(const App::DocumentObject& object, AdmissionRefusal reason)
{
    const auto decision = qualifyForDetachedExecution(object);
    EXPECT_FALSE(decision.admitted);
    EXPECT_FALSE(decision.detached);
    EXPECT_TRUE(decision.has(reason));
    QualificationProbe probe;
    probe.suppress = true;
    probe.guard = reason;
    const auto broken = qualifyForDetachedExecution(object, probe);
    EXPECT_FALSE(broken.has(reason));
    for (const auto other : decision.reasons) {
        if (other != reason) {
            EXPECT_TRUE(broken.has(other)) << "disabling one guard dropped another reason";
        }
    }
    if (decision.reasons.size() == 1) {
        EXPECT_TRUE(broken.reasons.empty());
        EXPECT_TRUE(broken.admitted);
        EXPECT_FALSE(broken.detached);
        EXPECT_FALSE(broken.pending.empty());
    }
    else {
        EXPECT_FALSE(broken.admitted);
    }
}

template <typename FeatureT>
FeatureT* addBoolean(App::Document& document, const char* name)
{
    auto* feature = document.addObject<FeatureT>(name);
    EXPECT_NE(feature, nullptr);
    if (feature) {
        feature->Refine.setValue(false);
    }
    return feature;
}

struct PreparedBoolean
{
    Part::Box* base {nullptr};
    Part::Box* tool {nullptr};
    Part::Boolean* feature {nullptr};
};

template <typename FeatureT>
PreparedBoolean prepareBoolean(App::Document& document, const char* prefix)
{
    PreparedBoolean prepared;
    const std::string baseName = std::string(prefix) + "Base";
    const std::string toolName = std::string(prefix) + "Tool";
    const std::string featureName = std::string(prefix) + "Bool";
    prepared.base = addBox(document, baseName.c_str(), 40, 30, 20);
    prepared.tool = addBox(document, toolName.c_str(), 10, 10, 30);
    prepared.feature = addBoolean<FeatureT>(document, featureName.c_str());
    if (!prepared.base || !prepared.tool || !prepared.feature) {
        return prepared;
    }
    prepared.tool->Placement.setValue(Base::Placement(Base::Vector3d(15, 10, -5), Base::Rotation()));
    prepared.feature->Base.setValue(prepared.base);
    prepared.feature->Tool.setValue(prepared.tool);
    prepared.feature->Placement.setValue(rotatedPlacement());
    return prepared;
}

class CallbackConnections
{
public:
    template <typename Connection>
    void keep(Connection connection)
    {
        _disconnect.push_back([connection]() mutable { connection.disconnect(); });
    }

    ~CallbackConnections()
    {
        for (auto& disconnect : _disconnect) {
            disconnect();
        }
    }

private:
    std::vector<std::function<void()>> _disconnect;
};

constexpr int kWideBooleans = 16;
constexpr int kChainSteps = 8;
constexpr int kHeavySides = 48;

// Globals the binary Boolean path reads or writes. Line numbers are the
// current tree. Helpers that this execute() does not call are listed as
// not-on-path so they are not mistaken for the native contract.
constexpr const char* kBooleanGlobalInventory[] = {
    "src/Mod/Part/App/FuzzyHelper.cpp:31 read/write BooleanFuzzy; binary Build reads it at "
    "FCBRepAlgoAPI_BooleanOperation.cpp:83 and does not call withBooleanFuzzy (FuzzyHelper.cpp:44)",
    "src/Mod/Part/App/FCBRepAlgoAPI_BooleanOperation.cpp:43,64 SetRunParallel(true) sets the "
    "operation flag myRunParallel, not BOPAlgo_Options::SetParallelMode",
    "src/Mod/Part/App/FCBRepAlgoAPI_BooleanOperation.cpp:44,65 SetNonDestructive(true) is per operation",
    "src/Mod/Part/App/FCBRepAlgoAPI_BooleanOperation.cpp:83-85 SetFuzzyValue from BooleanFuzzy, "
    "bounding-box extent and Precision::Confusion",
    "src/Mod/Part/App/FCBRepAlgoAPI_BooleanOperation.cpp:55-60 and FeaturePartBoolean.cpp:56 "
    "BRepCheck_Analyzer reads the shape; this OCC has no mutable BRepCheck mode",
    "usr/include/opencascade/Precision.hxx:165 Precision::Confusion is constexpr 1e-7 in OCC 7.9; "
    "read at FCBRepAlgoAPI_BooleanOperation.cpp:84",
    "src/Mod/Part/App/FCBRepAlgoAPI_BooleanOperation.cpp:101-110 Message_ProgressRange is a local "
    "empty range. Part::ProgressIndicator and Base::Sequencer are not on this path",
    "src/Mod/Part/App/FeaturePartBoolean.cpp:123-124 Base::SignalException installs and restores "
    "SIGSEGV (Base/Exception.cpp:733-747) on the owner execute path only",
    "src/Mod/Part/App/FCBRepAlgoAPI_BooleanOperation.cpp:107 Part::SignalException writes "
    "SignalException.cpp:211 currentSignalMode and installs handlers. The destructor "
    "(SignalException.cpp:225-233) unsets that mode and calls installSegfaultHandler, "
    "which replaces the previous SIGSEGV handler",
    "src/Mod/Part/App/FeaturePartBoolean.cpp:47-59 CheckModel is an application preference read, "
    "not an OCC global",
    "src/Mod/Part/App/TopoShape.cpp:1677-1678 BOPAlgo SetParallelMode/SetRunParallel is the separate "
    "BOP-check helper, not binary Boolean::execute",
    "src/Mod/Part/App/TopoShapeExpansion.cpp:6307-6323 OSD_Parallel::SetUseOcctThreads, "
    "ProgressIndicator and Sequencer belong to the general Boolean helper, not this execute",
    "src/Mod/Part/App/SignalException.cpp:173-174 the Part handler touches SIGFPE, SIGHUP, SIGINT, "
    "SIGQUIT, SIGILL, SIGBUS, SIGSYS and SIGSEGV. The worker compares each full sigaction",
    "OSD_ThreadPool::DefaultPool NbThreads and NbDefaultThreadsToLaunch. SetRunParallel can use "
    "this pool. The worker compares both values across the computation",
};

}  // namespace

TEST(DetachedKernelQualificationTemplate, narrowBoxDeclaresTheAuditContract)
{
    const auto& contract = PartQualification::narrowBoxQualification();
    EXPECT_STREQ(contract.exactType, "Part::Box");
    EXPECT_EQ(contract.liveRecompute, EffectClass::NeedsSerialization);
    EXPECT_EQ(contract.proposedKernel, EffectClass::Unknown);
    ASSERT_NE(contract.failureSemantics, nullptr);
    EXPECT_NE(std::string(contract.failureSemantics).find("does not replace Shape"), std::string::npos);

    const auto* length = findField(contract.reads, contract.readCount, "Length");
    ASSERT_NE(length, nullptr);
    EXPECT_EQ(length->classification, EffectClass::Safe);
    const auto* placement = findField(contract.reads, contract.readCount, "Placement");
    ASSERT_NE(placement, nullptr);
    EXPECT_EQ(placement->classification, EffectClass::NeedsSerialization);
    EXPECT_NE(findField(contract.reads, contract.readCount, "mutating observers"), nullptr);
    EXPECT_NE(findField(contract.reads, contract.readCount, "capture and commit invalidation"), nullptr);

    const auto* shape = findField(contract.outputs, contract.outputCount, "Shape");
    ASSERT_NE(shape, nullptr);
    EXPECT_EQ(shape->classification, EffectClass::NeedsSerialization);
    EXPECT_NE(findField(contract.outputs, contract.outputCount, "ShapeMaterial"), nullptr);

    bool forbidsCache = false;
    bool forbidsPreference = false;
    bool forbidsLinks = false;
    for (std::size_t index = 0; index < contract.forbiddenCount; ++index) {
        const std::string name = contract.forbidden[index].name;
        forbidsCache = forbidsCache || name.find("ShapeCache") != std::string::npos;
        forbidsPreference = forbidsPreference || name.find("preference") != std::string::npos;
        forbidsLinks = forbidsLinks || name.find("links, groups") != std::string::npos;
        EXPECT_EQ(contract.forbidden[index].classification, EffectClass::NeedsSerialization);
    }
    EXPECT_TRUE(forbidsCache);
    EXPECT_TRUE(forbidsPreference);
    EXPECT_TRUE(forbidsLinks);
    ASSERT_EQ(contract.preferenceReadingTypeCount, 3u);
    ASSERT_EQ(contract.pendingCount, 3u);
    EXPECT_NE(std::string(contract.pending[0]).find("capture/commit"), std::string::npos);
    EXPECT_NE(std::string(contract.pending[1]).find("document-level callback"), std::string::npos);
    EXPECT_NE(std::string(contract.pending[2]).find("extension effects"), std::string::npos);
}

TEST(DetachedKernelQualificationTemplate, narrowBooleanDeclaresTheAuditContract)
{
    for (const char* type : {"Part::Cut", "Part::Fuse", "Part::Common"}) {
        const auto& contract = PartQualification::narrowBooleanQualification(type);
        EXPECT_STREQ(contract.exactType, type);
        EXPECT_EQ(contract.liveRecompute, EffectClass::NeedsSerialization);
        EXPECT_EQ(contract.proposedKernel, EffectClass::Unknown);
        EXPECT_EQ(contract.preferenceReadingTypeCount, 0u);
        ASSERT_NE(contract.failureSemantics, nullptr);
        EXPECT_NE(std::string(contract.failureSemantics).find("before the Shape setter"), std::string::npos);
        const auto* checkModel = findField(contract.reads, contract.readCount, "CheckModel");
        ASSERT_NE(checkModel, nullptr);
        EXPECT_EQ(checkModel->classification, EffectClass::Safe);
        const auto* refine = findField(contract.reads, contract.readCount, "Refine");
        ASSERT_NE(refine, nullptr);
        EXPECT_EQ(refine->classification, EffectClass::Safe);
        const auto* fuzzy = findField(contract.reads, contract.readCount, "Boolean fuzzy factor");
        ASSERT_NE(fuzzy, nullptr);
        EXPECT_EQ(fuzzy->classification, EffectClass::Safe);
        EXPECT_NE(findField(contract.reads, contract.readCount, "History"), nullptr);
        const auto* material = findField(contract.outputs, contract.outputCount, "ShapeMaterial");
        ASSERT_NE(material, nullptr);
        EXPECT_EQ(material->classification, EffectClass::NeedsSerialization);
        const auto* materialRead =
            findField(contract.reads, contract.readCount, "Base, target and default ShapeMaterial");
        ASSERT_NE(materialRead, nullptr);
        EXPECT_EQ(materialRead->classification, EffectClass::NeedsSerialization);
        const std::string materialBoundary = materialRead->boundary;
        EXPECT_NE(materialBoundary.find("target's current ShapeMaterial"), std::string::npos);
        EXPECT_NE(materialBoundary.find("invalidates"), std::string::npos);
        EXPECT_NE(std::string(contract.pending[0]).find("target ShapeMaterial"), std::string::npos);
        EXPECT_NE(findField(contract.outputs, contract.outputCount, "History"), nullptr);
        EXPECT_NE(findField(contract.outputs, contract.outputCount, "element map and history-derived names"),
                  nullptr);
        ASSERT_GE(contract.pendingCount, 10u);
        bool mentionsSignal = false;
        bool mentionsRefine = false;
        bool mentionsCapture = false;
        bool mentionsRenaming = false;
        for (std::size_t index = 0; index < contract.pendingCount; ++index) {
            const std::string text = contract.pending[index];
            mentionsSignal = mentionsSignal || text.find("SignalException") != std::string::npos;
            mentionsRefine = mentionsRefine || text.find("Refine") != std::string::npos;
            mentionsCapture = mentionsCapture || text.find("capture/commit") != std::string::npos;
            mentionsRenaming = mentionsRenaming || text.find("owner-side re-naming") != std::string::npos;
        }
        EXPECT_TRUE(mentionsSignal);
        EXPECT_TRUE(mentionsRefine);
        EXPECT_TRUE(mentionsCapture);
        EXPECT_TRUE(mentionsRenaming);
    }
    EXPECT_GE(sizeof(kBooleanGlobalInventory) / sizeof(kBooleanGlobalInventory[0]), 10u);
}

TEST(DetachedKernelQualificationPlatform, posixWorkerSignalHandlers)
{
#if !PRC_M0_HAS_POSIX_SIGNALS
    GTEST_SKIP() << "POSIX sigset_t, sigaction and sigismember are unavailable. "
                    "Worker signal handlers are not compared on this platform.";
#else
    const auto before = captureSignal(SIGSEGV);
    const auto after = captureSignal(SIGSEGV);
    EXPECT_TRUE(before.valid);
    EXPECT_TRUE(after.valid);
    EXPECT_TRUE(sameSignalMask(before.mask, after.mask));
    EXPECT_EQ(before.handler, after.handler);
    EXPECT_EQ(sizeof(kPartSignalTypes) / sizeof(kPartSignalTypes[0]), 8u);
#endif
}

class DetachedKernelQualificationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        App::Internal::ensureGenericIsolatedRecomputeRegistered();
        if (Part::PropertyShapeCache::getClassTypeId().isBad()) {
            Part::PropertyShapeCache::init();
        }
        const auto path = costPath();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream out(path, std::ios::trunc);
    }

    void SetUp() override
    {
        auto group = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        _previousEnabled = group->GetBool("RecomputePhaseTrace", false);
        _previousPath = group->GetASCII("RecomputePhaseTracePath", "");
        _previousCheckModel = booleanPreferences()->GetBool("CheckModel", true);
        setTrace(false);
        _name = "QualBox" + std::to_string(++_sequence);
        _document = App::GetApplication().newDocument(_name.c_str(), "qualification");
        ASSERT_NE(_document, nullptr);
    }

    void TearDown() override
    {
        for (const auto& extra : _extra) {
            if (!extra.empty() && App::GetApplication().getDocument(extra.c_str())) {
                App::GetApplication().closeDocument(extra.c_str());
            }
        }
        _extra.clear();
        if (!_name.empty() && App::GetApplication().getDocument(_name.c_str())) {
            App::GetApplication().closeDocument(_name.c_str());
        }
        _document = nullptr;
        auto group = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        group->SetBool("RecomputePhaseTrace", _previousEnabled);
        group->SetASCII("RecomputePhaseTracePath", _previousPath);
        booleanPreferences()->SetBool("CheckModel", _previousCheckModel);
    }

    void setTrace(bool enabled)
    {
        auto group = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        group->SetBool("RecomputePhaseTrace", enabled);
        group->SetASCII("RecomputePhaseTracePath", "");
    }

    App::Document* makeControl(const char* prefix)
    {
        const auto unique = App::GetApplication().getUniqueDocumentName(prefix);
        auto* document = App::GetApplication().newDocument(unique.c_str(), prefix);
        EXPECT_NE(document, nullptr);
        if (document) {
            _extra.push_back(document->getName());
        }
        return document;
    }

    void expectPendingBlocksDetach(const PartQualification::AdmissionDecision& decision)
    {
        EXPECT_FALSE(decision.pending.empty());
        EXPECT_FALSE(decision.detached);
        bool mentionsCapture = false;
        for (const char* item : decision.pending) {
            ASSERT_NE(item, nullptr);
            mentionsCapture = mentionsCapture
                || std::string(item).find("capture/commit") != std::string::npos;
        }
        EXPECT_TRUE(mentionsCapture);
    }

    PartQualification::AdmissionDecision expectRefused(App::DocumentObject& object,
                                                       AdmissionRefusal reason,
                                                       App::DocumentObject* control = nullptr,
                                                       bool compareDynamicNames = true)
    {
        const auto before = capturePostcondition(object);
        const auto decision = qualifyForDetachedExecution(object);
        EXPECT_FALSE(decision.admitted) << object.getNameInDocument();
        expectPendingBlocksDetach(decision);
        EXPECT_EQ(decision.liveRecompute, EffectClass::NeedsSerialization);
        EXPECT_EQ(decision.proposedKernel, EffectClass::Unknown);
        EXPECT_TRUE(decision.has(reason)) << object.getNameInDocument();
        const char* expected = PartQualification::admissionCitation(reason);
        bool cited = false;
        for (const char* citation : decision.citations) {
            if (citation && expected && std::strcmp(citation, expected) == 0) {
                cited = true;
            }
        }
        EXPECT_TRUE(cited) << expected;
        expectEqualPostcondition(before, capturePostcondition(object), compareDynamicNames);

        object.touch();
        setTrace(true);
        _document->recompute();
        const auto record = _document->lastRecomputeTrace();
        EXPECT_NE(record, nullptr);
        if (record) {
            expectNoTransfer(*record);
            bool executed = false;
            for (const auto& traced : record->objects) {
                if (object.getNameInDocument() && traced.name == object.getNameInDocument()
                    && traced.executed) {
                    executed = true;
                }
            }
            // A plain App::Link is bookkeeping: the owner recompute clears Touch
            // without an Execute span. Geometry features must show Execute.
            if (!dynamic_cast<const App::Link*>(&object)
                && !_document->testStatus(App::Document::PartialDoc)
                && !_document->testStatus(App::Document::SkipRecompute)) {
                EXPECT_TRUE(executed) << object.getNameInDocument();
            }
        }
        setTrace(false);
        if (control && control->getDocument()) {
            control->touch();
            control->getDocument()->recompute();
            expectEqualPostcondition(
                capturePostcondition(object),
                capturePostcondition(*control),
                compareDynamicNames);
        }
        else {
            expectEqualPostcondition(before, capturePostcondition(object), compareDynamicNames);
        }
        return decision;
    }

    void expectColdDocumentFlag(App::Document::Status flag)
    {
        auto* box = addBox(*_document, "Box");
        ASSERT_NE(box, nullptr);
        box->Placement.setValue(rotatedPlacement());
        _document->recompute();
        ASSERT_TRUE(qualifyForDetachedExecution(*box).admitted) << "box was not settled";
        auto* controlDoc = makeControl("QualCold");
        ASSERT_NE(controlDoc, nullptr);
        auto* control = addBox(*controlDoc, "Box");
        ASSERT_NE(control, nullptr);
        control->Placement.setValue(box->Placement.getValue());
        controlDoc->recompute();

        _document->setStatus(flag, true);
        const auto decision = qualifyForDetachedExecution(*box);
        EXPECT_FALSE(decision.admitted);
        EXPECT_TRUE(decision.has(AdmissionRefusal::ColdOrRestore));
        expectPendingBlocksDetach(decision);

        box->touch();
        _document->recompute();
        control->touch();
        controlDoc->recompute();
        // PartialDoc makes Document::recompute return before execute, so the
        // touched box stays Touched. The control is recomputed without that flag.
        if (flag == App::Document::PartialDoc) {
            const auto observed = capturePostcondition(*box);
            const auto controlState = capturePostcondition(*control);
            EXPECT_EQ(observed.status, "Touched");
            EXPECT_EQ(controlState.status, "Valid");
            expectEqualPostcondition(observed, controlState, true, false);
        }
        else {
            expectEqualPostcondition(capturePostcondition(*box), capturePostcondition(*control));
        }

        _document->setStatus(flag, false);
        EXPECT_TRUE(qualifyForDetachedExecution(*box).admitted);
    }

    App::Document* _document {nullptr};
    std::string _name;
    std::vector<std::string> _extra;
    bool _previousEnabled {false};
    bool _previousCheckModel {true};
    std::string _previousPath;
    static int _sequence;
};

int DetachedKernelQualificationTest::_sequence = 0;

TEST_F(DetachedKernelQualificationTest, settledExactBoxIsAdmittedAndStaysOnTheOwner)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    box->Placement.setValue(rotatedPlacement());
    _document->recompute();
    const auto before = capturePostcondition(*box);
    EXPECT_NEAR(before.volume, 6000.0, 1e-4);
    EXPECT_FALSE(before.elementMapVersion.empty());

    const auto decision = qualifyForDetachedExecution(*box);
    if (!decision.admitted) {
        for (const char* citation : decision.citations) {
            ADD_FAILURE() << (citation ? citation : "");
        }
        ADD_FAILURE() << "stored='" << box->_ElementMapVersion.getStrValue() << "' current='"
                      << box->getElementMapVersion(box->getPropertyOfGeometry()) << "'";
    }
    EXPECT_TRUE(decision.admitted);
    EXPECT_TRUE(decision.reasons.empty());
    expectPendingBlocksDetach(decision);
    EXPECT_EQ(decision.liveRecompute, EffectClass::NeedsSerialization);
    EXPECT_EQ(decision.proposedKernel, EffectClass::Unknown);
    expectEqualPostcondition(before, capturePostcondition(*box));
    EXPECT_TRUE(legacyIsolatedPlanDetaches(*_document, *box));

    box->touch();
    setTrace(true);
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectNoTransfer(*record);
    expectEqualPostcondition(before, capturePostcondition(*box));
}

TEST_F(DetachedKernelQualificationTest, linksAreNotAdmitted)
{
    auto* box = addBox(*_document, "Box");
    auto* link = dynamic_cast<App::Link*>(_document->addObject("App::Link", "Link"));
    ASSERT_NE(box, nullptr);
    ASSERT_NE(link, nullptr);
    link->setLink(-1, box);
    _document->recompute();

    auto* controlDoc = makeControl("QualLink");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();

    const auto decision = expectRefused(*link, AdmissionRefusal::Link);
    EXPECT_FALSE(decision.has(AdmissionRefusal::ExternalReference));
    EXPECT_FALSE(decision.has(AdmissionRefusal::ScalingOrPlacementTransform));
    EXPECT_TRUE(legacyIsolatedPlanDetaches(*_document, *link));
    control->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*box), capturePostcondition(*control));

    const auto linked = capturePostcondition(*link);
    EXPECT_EQ(linked.linkedName, "Box");
    EXPECT_NEAR(linked.scale, 1.0, 1e-12);
    EXPECT_NEAR(linked.scaleVector.x, 1.0, 1e-12);
    EXPECT_NEAR(linked.scaleVector.y, 1.0, 1e-12);
    EXPECT_NEAR(linked.scaleVector.z, 1.0, 1e-12);
    EXPECT_FALSE(linked.error);
    EXPECT_EQ(linked.status, capturePostcondition(*control).status);
    const auto resolved = Part::Feature::getTopoShape(link, Part::ShapeOption::NoFlag);
    expectVerticesNear(resolved.getShape(), control->Shape.getValue());
    EXPECT_EQ(elementMapText(resolved), elementMapText(control->Shape.getShape()));
}

TEST_F(DetachedKernelQualificationTest, plainSameDocumentLinkIsProcessEligible)
{
    // Records the current public recomputeAsync(..., Isolated) result for a
    // plain same-document Link: process eligibility, not detached admission.
    auto* box = addBox(*_document, "Box");
    auto* link = dynamic_cast<App::Link*>(_document->addObject("App::Link", "Link"));
    ASSERT_NE(box, nullptr);
    ASSERT_NE(link, nullptr);
    link->setLink(-1, box);
    _document->recompute();
    const auto admission = qualifyForDetachedExecution(*link);
    EXPECT_TRUE(admission.has(AdmissionRefusal::Link));
    EXPECT_FALSE(admission.detached);

    const auto snapshot = waitIsolated(*_document, *link);
    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    const App::DocumentRecomputeFeatureSnapshot* linkFeature = nullptr;
    for (const auto& feature : snapshot.features) {
        if (feature.featureId == "Link") {
            linkFeature = &feature;
        }
    }
    ASSERT_NE(linkFeature, nullptr) << snapshot.diagnostic;
    // Current public behaviour: a forced Isolated plan treats the plain
    // same-document Link as process-eligible. The linked Box is in the same
    // plan. This is not detached-thread admission.
    EXPECT_FALSE(linkFeature->ownerThreadExecution) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.ownerThreadFeatures, 0u);
    EXPECT_TRUE(qualifyForDetachedExecution(*link).has(AdmissionRefusal::Link));
    EXPECT_FALSE(qualifyForDetachedExecution(*link).detached);
}

TEST_F(DetachedKernelQualificationTest, groupsAreNotAdmitted)
{
    auto* part = _document->addObject<App::Part>("Part");
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(part, nullptr);
    ASSERT_NE(box, nullptr);
    part->addObject(box);
    _document->recompute();

    auto* controlDoc = makeControl("QualGroup");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();

    const auto grouped = expectRefused(*box, AdmissionRefusal::Group, control);
    EXPECT_FALSE(grouped.has(AdmissionRefusal::ScalingOrPlacementTransform));
    EXPECT_TRUE(legacyIsolatedPlanDetaches(*_document, *box));
    const auto partDecision = qualifyForDetachedExecution(*part);
    EXPECT_FALSE(partDecision.admitted);
    EXPECT_FALSE(partDecision.detached);
    EXPECT_TRUE(partDecision.has(AdmissionRefusal::Group));
}

TEST_F(DetachedKernelQualificationTest, nonUnitScaleIsNotAdmitted)
{
    auto* source = addBox(*_document, "ScaleSource");
    auto* link = dynamic_cast<App::Link*>(_document->addObject("App::Link", "Scaled"));
    ASSERT_NE(source, nullptr);
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->Scale.setValue(2.0);
    link->ScaleVector.setValue(2.0, 2.0, 2.0);
    _document->recompute();

    auto* controlDoc = makeControl("QualScale");
    ASSERT_NE(controlDoc, nullptr);
    auto* controlSource = addBox(*controlDoc, "ScaleSource");
    auto* controlLink = dynamic_cast<App::Link*>(controlDoc->addObject("App::Link", "Scaled"));
    auto* controlScaled = addBox(*controlDoc, "ScaledSolid", 20, 40, 60);
    ASSERT_NE(controlSource, nullptr);
    ASSERT_NE(controlLink, nullptr);
    ASSERT_NE(controlScaled, nullptr);
    controlLink->setLink(-1, controlSource);
    controlLink->Scale.setValue(2.0);
    controlLink->ScaleVector.setValue(2.0, 2.0, 2.0);
    controlDoc->recompute();

    const auto scaled = qualifyForDetachedExecution(*link);
    EXPECT_FALSE(scaled.admitted);
    EXPECT_FALSE(scaled.detached);
    EXPECT_TRUE(scaled.has(AdmissionRefusal::ScalingOrPlacementTransform));
    EXPECT_TRUE(scaled.has(AdmissionRefusal::Link));
    EXPECT_FALSE(scaled.has(AdmissionRefusal::ExternalReference));
    EXPECT_FALSE(scaled.has(AdmissionRefusal::Group));

    source->touch();
    link->touch();
    _document->recompute();
    controlSource->touch();
    controlLink->touch();
    controlScaled->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*source), capturePostcondition(*controlSource));
    expectEqualPostcondition(captureLinkOutput(*link), captureLinkOutput(*controlLink));
    const auto resolved = Part::Feature::getTopoShape(link, Part::ShapeOption::NoFlag);
    expectVerticesNear(resolved.getShape(), controlScaled->Shape.getValue());
    EXPECT_NEAR(PartTestHelpers::getVolume(resolved.getShape()), 48000.0, 1e-2);
}

TEST_F(DetachedKernelQualificationTest, parentTransformIsNotAdmitted)
{
    auto* part = _document->addObject<App::Part>("Part");
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(part, nullptr);
    ASSERT_NE(box, nullptr);
    part->addObject(box);
    part->Placement.setValue(
        Base::Placement(Base::Vector3d(12, 3, 0), Base::Rotation(Base::Vector3d(0, 0, 1), 0.3)));
    _document->recompute();

    auto* controlDoc = makeControl("QualParent");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();

    const auto moved = qualifyForDetachedExecution(*box);
    EXPECT_FALSE(moved.admitted);
    EXPECT_FALSE(moved.detached);
    EXPECT_TRUE(moved.has(AdmissionRefusal::ScalingOrPlacementTransform));
    EXPECT_FALSE(moved.has(AdmissionRefusal::Link));

    box->touch();
    _document->recompute();
    control->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*box), capturePostcondition(*control));
    const auto local = box->Placement.getValue();
    const auto global = App::GeoFeature::getGlobalPlacement(box);
    EXPECT_FALSE(global.isSame(local, 1e-9));
    EXPECT_TRUE(global.isSame(part->Placement.getValue() * local, 1e-7));
    EXPECT_FALSE(global.isSame(App::GeoFeature::getGlobalPlacement(control), 1e-7));
}

TEST_F(DetachedKernelQualificationTest, externalReferencesAreNotAdmitted)
{
    CloseDocument foreignGuard;
    foreignGuard.name = App::GetApplication().getUniqueDocumentName("QualForeign");
    auto* foreign = App::GetApplication().newDocument(foreignGuard.name.c_str(), "foreign");
    ASSERT_NE(foreign, nullptr);
    auto* foreignBox = addBox(*foreign, "ForeignBox");
    ASSERT_NE(foreignBox, nullptr);
    foreign->recompute();

    auto* controlDoc = makeControl("QualExternal");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "ForeignBox");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();
    auto* controlLink = dynamic_cast<App::Link*>(controlDoc->addObject("App::Link", "External"));
    ASSERT_NE(controlLink, nullptr);

    auto* link = dynamic_cast<App::Link*>(_document->addObject("App::Link", "External"));
    ASSERT_NE(link, nullptr);
    TempArchive ownerArchive(
        std::filesystem::temp_directory_path() / "prc-m0-2a" / "external-owner.FCStd");
    TempArchive foreignArchive(
        std::filesystem::temp_directory_path() / "prc-m0-2a" / "external-foreign.FCStd");
    TempArchive controlArchive(
        std::filesystem::temp_directory_path() / "prc-m0-2a" / "external-control.FCStd");
    ASSERT_TRUE(_document->saveAs(ownerArchive.path().string().c_str()));
    ASSERT_TRUE(foreign->saveAs(foreignArchive.path().string().c_str()));
    ASSERT_TRUE(controlDoc->saveAs(controlArchive.path().string().c_str()));
    link->setLink(-1, foreignBox);
    controlLink->setLink(-1, foreignBox);

    const auto decision = qualifyForDetachedExecution(*link);
    EXPECT_FALSE(decision.admitted);
    EXPECT_FALSE(decision.detached);
    EXPECT_TRUE(decision.has(AdmissionRefusal::ExternalReference));
    EXPECT_TRUE(decision.has(AdmissionRefusal::Link));
    expectPendingBlocksDetach(decision);

    link->touch();
    setTrace(true);
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectNoTransfer(*record);
    setTrace(false);
    foreignBox->touch();
    foreign->recompute();
    control->touch();
    controlLink->touch();
    link->touch();
    controlDoc->recompute();
    _document->recompute();
    expectEqualPostcondition(capturePostcondition(*foreignBox), capturePostcondition(*control));
    expectEqualPostcondition(captureLinkOutput(*link), captureLinkOutput(*controlLink));
}

TEST_F(DetachedKernelQualificationTest, externalClosureIsRefused)
{
    CloseDocument foreignGuard;
    foreignGuard.name = App::GetApplication().getUniqueDocumentName("QualClosure");
    auto* foreign = App::GetApplication().newDocument(foreignGuard.name.c_str(), "closure");
    ASSERT_NE(foreign, nullptr);
    auto* foreignBox = addBox(*foreign, "ForeignBox");
    ASSERT_NE(foreignBox, nullptr);
    foreign->recompute();
    auto* link = dynamic_cast<App::Link*>(_document->addObject("App::Link", "External"));
    ASSERT_NE(link, nullptr);
    TempArchive ownerArchive(
        std::filesystem::temp_directory_path() / "prc-m0-2a" / "closure-owner.FCStd");
    TempArchive foreignArchive(
        std::filesystem::temp_directory_path() / "prc-m0-2a" / "closure-foreign.FCStd");
    ASSERT_TRUE(_document->saveAs(ownerArchive.path().string().c_str()));
    ASSERT_TRUE(foreign->saveAs(foreignArchive.path().string().c_str()));
    link->setLink(-1, foreignBox);

    auto* controlDoc = makeControl("QualClosureControl");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "ForeignBox");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();

    // force_execution skips the touched-link bookkeeping shortcut and reaches
    // collectClosure(), which refuses the foreign dependency.
    bool closureRefused = false;
    std::string detail;
    try {
        App::CollaborativeOperationIntent intent {
            std::string(App::GenericIsolatedRecomputeOperationType),
            {{"feature", "External"}, {"force_execution", "1"}}};
        const auto preparation =
            App::CollaborativeOperationRegistry::instance().prepare(*_document, intent);
        detail = "returned policy " + std::to_string(static_cast<int>(preparation.policy));
    }
    catch (const std::exception& error) {
        detail = error.what();
        closureRefused = detail.find("unresolved cross-document dependency") != std::string::npos;
    }
    EXPECT_TRUE(closureRefused) << detail;
    foreignBox->touch();
    foreign->recompute();
    control->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*foreignBox), capturePostcondition(*control));
}

TEST_F(DetachedKernelQualificationTest, pythonFeaturesAreNotAdmitted)
{
    auto* first = _document->addObject("Part::FeaturePython", "PyCount");
    auto* sibling = addBox(*_document, "Sibling");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(sibling, nullptr);
    ASSERT_TRUE(runPython(
        "class QualCountingProxy:\n"
        "    def __init__(self):\n"
        "        self.count = 0\n"
        "    def execute(self, obj):\n"
        "        self.count += 1\n"
        "        return True\n"));
    {
        Base::PyGILStateLocker lock;
        PyObject* mainModule = PyImport_AddModule("__main__");
        PyObject* globals = PyModule_GetDict(mainModule);
        PyObject* py = first->getPyObject();
        ASSERT_EQ(PyDict_SetItemString(globals, "qual_py", py), 0);
        Py_DECREF(py);
        ASSERT_TRUE(runPython("qual_py.Proxy = QualCountingProxy()\n"));
    }
    EXPECT_FALSE(first->canRecomputeOnWorker());
    EXPECT_FALSE(legacyIsolatedPlanDetaches(*_document, *first));

    auto* controlDoc = makeControl("QualPython");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Sibling");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();
    _document->recompute();
    EXPECT_GE(proxyCount(*first), 1);

    const auto decision = expectRefused(*first, AdmissionRefusal::PythonFeature, nullptr);
    EXPECT_TRUE(decision.has(AdmissionRefusal::NotExactType));
    EXPECT_GE(proxyCount(*first), 2);
    control->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*sibling), capturePostcondition(*control));

    App::CollaborativeOperationIntent intent {
        std::string(App::GenericIsolatedRecomputeOperationType),
        {{"feature", "PyCount"}}};
    const auto preparation =
        App::CollaborativeOperationRegistry::instance().prepare(*_document, intent);
    EXPECT_NE(preparation.policy, App::PreparationPolicy::IsolatedProcess);
    EXPECT_EQ(preparation.policy, App::PreparationPolicy::DetachedInProcess);
}

TEST_F(DetachedKernelQualificationTest, expressionsThatReadOtherObjectsAreNotAdmitted)
{
    auto* source = addBox(*_document, "Source", 15, 20, 30);
    auto* driven = addBox(*_document, "Driven", 10, 20, 30);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(driven, nullptr);
    auto expression = std::shared_ptr<App::Expression>(App::Expression::parse(driven, "Source.Length"));
    ASSERT_NE(expression, nullptr);
    driven->setExpression(App::ObjectIdentifier(driven->Length), expression);
    _document->recompute();
    EXPECT_NEAR(driven->Length.getValue(), 15.0, 1e-9);

    auto* controlDoc = makeControl("QualExpr");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Driven", 15, 20, 30);
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();

    const auto decision =
        expectRefused(*driven, AdmissionRefusal::ExpressionReadsOtherObject, control);
    EXPECT_TRUE(decision.has(AdmissionRefusal::ExpressionBinding));
    EXPECT_TRUE(legacyIsolatedPlanDetaches(*_document, *driven));
    EXPECT_NEAR(driven->Length.getValue(), 15.0, 1e-9);
    EXPECT_TRUE(qualifyForDetachedExecution(*source).admitted);
}

TEST_F(DetachedKernelQualificationTest, settledCutFreezesCheckModelAndIsNotDetached)
{
    // M0.2a refused every Cut because execute reads CheckModel. The Boolean
    // contract freezes that value on the owner instead. A direct Refine=false
    // Cut is admitted and still not detached: the pending rows stay unchecked.
    auto* base = addBox(*_document, "Base", 40, 30, 20);
    auto* tool = addBox(*_document, "Tool", 10, 10, 30);
    auto* cut = _document->addObject<Part::Cut>("Cut");
    ASSERT_NE(base, nullptr);
    ASSERT_NE(tool, nullptr);
    ASSERT_NE(cut, nullptr);
    tool->Placement.setValue(Base::Placement(Base::Vector3d(15, 10, -5), Base::Rotation()));
    cut->Base.setValue(base);
    cut->Tool.setValue(tool);
    cut->Refine.setValue(false);
    _document->recompute();

    auto* controlDoc = makeControl("QualPref");
    ASSERT_NE(controlDoc, nullptr);
    auto* controlBase = addBox(*controlDoc, "Base", 40, 30, 20);
    auto* controlTool = addBox(*controlDoc, "Tool", 10, 10, 30);
    auto* controlCut = controlDoc->addObject<Part::Cut>("Cut");
    ASSERT_NE(controlBase, nullptr);
    ASSERT_NE(controlTool, nullptr);
    ASSERT_NE(controlCut, nullptr);
    controlTool->Placement.setValue(tool->Placement.getValue());
    controlCut->Base.setValue(controlBase);
    controlCut->Tool.setValue(controlTool);
    controlCut->Refine.setValue(false);
    controlDoc->recompute();

    const auto before = capturePostcondition(*cut);
    const auto decision = qualifyForDetachedExecution(*cut);
    if (!decision.admitted) {
        for (const char* citation : decision.citations) {
            ADD_FAILURE() << (citation ? citation : "");
        }
    }
    EXPECT_TRUE(decision.admitted);
    EXPECT_FALSE(decision.has(AdmissionRefusal::PreferenceReadDuringExecute));
    EXPECT_FALSE(decision.has(AdmissionRefusal::Link));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandLink));
    expectPendingBlocksDetach(decision);
    EXPECT_EQ(decision.proposedKernel, EffectClass::Unknown);
    const auto& contract = PartQualification::narrowBooleanQualification("Part::Cut");
    const auto* checkModel = findField(contract.reads, contract.readCount, "CheckModel");
    ASSERT_NE(checkModel, nullptr);
    EXPECT_EQ(checkModel->classification, EffectClass::Safe);
    EXPECT_EQ(contract.preferenceReadingTypeCount, 0u);
    EXPECT_TRUE(legacyIsolatedPlanDetaches(*_document, *cut));

    cut->touch();
    setTrace(true);
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectNoTransfer(*record);
    setTrace(false);
    controlCut->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*cut), capturePostcondition(*controlCut));
    expectEqualPostcondition(before, capturePostcondition(*cut));
    EXPECT_FALSE(capturePostcondition(*cut).error);
    EXPECT_EQ(cut->History.getSize(), 0);
}

TEST_F(DetachedKernelQualificationTest, checkModelPreferenceChangesBooleanOutcome)
{
    // Same Cut, CheckModel off then on. The operand is the unorientable solid
    // BRepCheck already rejects; the boolean still completes. Execute reads the
    // preference: off publishes that shape, on returns the check error.
    // TearDown restores the saved CheckModel value.
    auto* base = _document->addObject<Part::Feature>("Base");
    auto* tool = addBox(*_document, "Tool", 10, 10, 10);
    auto* cut = _document->addObject<Part::Cut>("Cut");
    ASSERT_NE(base, nullptr);
    ASSERT_NE(tool, nullptr);
    ASSERT_NE(cut, nullptr);
    const auto solid = unorientableSolid();
    ASSERT_FALSE(solid.IsNull());
    ASSERT_FALSE(BRepCheck_Analyzer(solid).IsValid());
    base->Shape.setValue(solid);
    tool->Placement.setValue(Base::Placement(Base::Vector3d(1000, 0, 0), Base::Rotation()));
    cut->Base.setValue(base);
    cut->Tool.setValue(tool);
    cut->Refine.setValue(false);

    booleanPreferences()->SetBool("CheckModel", false);
    _document->recompute();
    EXPECT_FALSE(cut->isError()) << cut->getStatusString();
    EXPECT_EQ(std::string(cut->getStatusString()), "Valid");
    ASSERT_FALSE(cut->Shape.getValue().IsNull());
    EXPECT_FALSE(BRepCheck_Analyzer(cut->Shape.getValue()).IsValid());

    booleanPreferences()->SetBool("CheckModel", true);
    cut->touch();
    _document->recompute();
    EXPECT_TRUE(cut->isError());
    const std::string status = cut->getStatusString();
    EXPECT_NE(status.find("Resulting shape is invalid"), std::string::npos) << status;
}

TEST_F(DetachedKernelQualificationTest, dynamicShapeCacheIsNotAdmitted)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    auto* cache = box->addDynamicProperty(
        "Part::PropertyShapeCache",
        "_Part_ShapeCache",
        "Part",
        "Shape cache",
        App::Prop_NoPersist | App::Prop_Output | App::Prop_Hidden);
    ASSERT_NE(cache, nullptr);
    _document->recompute();

    auto* controlDoc = makeControl("QualCache");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();

    const auto decision =
        expectRefused(*box, AdmissionRefusal::DynamicShapeCache, control, /*compareDynamicNames=*/false);
    EXPECT_FALSE(decision.has(AdmissionRefusal::DynamicProperty));
    EXPECT_TRUE(legacyIsolatedPlanDetaches(*_document, *box));
    EXPECT_NE(box->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    EXPECT_EQ(control->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);

    auto* coldSource = addBox(*_document, "ColdSource");
    auto* coldLink = dynamic_cast<App::Link*>(_document->addObject("App::Link", "ColdLink"));
    ASSERT_NE(coldSource, nullptr);
    ASSERT_NE(coldLink, nullptr);
    coldLink->setLink(-1, coldSource);
    coldLink->Scale.setValue(2.0);
    _document->recompute();
    EXPECT_EQ(coldLink->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    const auto resolved = Part::Feature::getTopoShape(coldLink, Part::ShapeOption::NoFlag);
    EXPECT_FALSE(resolved.isNull());
    EXPECT_NE(coldLink->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    Part::TopoShape cached;
    EXPECT_TRUE(Part::PropertyShapeCache::getShape(coldLink, cached));
    EXPECT_FALSE(cached.isNull());
    const auto cold = qualifyForDetachedExecution(*coldLink);
    EXPECT_TRUE(cold.has(AdmissionRefusal::DynamicShapeCache));
    EXPECT_FALSE(cold.detached);
    expectEqualPostcondition(capturePostcondition(*coldSource), capturePostcondition(*control));
}

TEST_F(DetachedKernelQualificationTest, extensionSchemaIsNotAdmitted)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*box).admitted);
    if (Part::PrismExtension::getExtensionClassTypeId().isBad()) {
        Part::PrismExtension::init();
    }
    auto extra = std::make_unique<Part::PrismExtension>();
    extra->initExtension(box);

    auto* controlDoc = makeControl("QualExt");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();
    const auto decision = expectRefused(*box, AdmissionRefusal::ExtensionSchema, control);
    EXPECT_FALSE(decision.has(AdmissionRefusal::Group));

    App::GetApplication().closeDocument(_name.c_str());
    _document = nullptr;
    _name.clear();
}

TEST_F(DetachedKernelQualificationTest, unsettledElementMapVersionIsNotAdmitted)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*box).admitted);
    const auto current = box->getElementMapVersion(box->getPropertyOfGeometry());
    ASSERT_FALSE(current.empty());
    box->_ElementMapVersion.setValue("qualification-stale");
    const auto decision = qualifyForDetachedExecution(*box);
    EXPECT_FALSE(decision.admitted);
    EXPECT_FALSE(decision.detached);
    EXPECT_TRUE(decision.has(AdmissionRefusal::UnsettledElementMapVersion));

    auto* controlDoc = makeControl("QualVersion");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();
    box->touch();
    _document->recompute();
    control->touch();
    controlDoc->recompute();
    auto left = capturePostcondition(*box);
    auto right = capturePostcondition(*control);
    left.elementMapVersion.clear();
    right.elementMapVersion.clear();
    expectEqualPostcondition(left, right);
    EXPECT_NE(box->_ElementMapVersion.getStrValue(), "qualification-stale");
    EXPECT_TRUE(qualifyForDetachedExecution(*box).admitted);
}

TEST_F(DetachedKernelQualificationTest, mutatingObserversAreNotAdmitted)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*box).admitted);
    int hits = 0;
    auto connection = box->signalChanged.connect(
        [&hits](const App::DocumentObject&, const App::Property&) { ++hits; });
    const auto decision = qualifyForDetachedExecution(*box);
    EXPECT_FALSE(decision.admitted);
    EXPECT_TRUE(decision.has(AdmissionRefusal::MutatingObserver));
    expectPendingBlocksDetach(decision);

    auto* controlDoc = makeControl("QualObs");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    controlDoc->recompute();
    box->touch();
    _document->recompute();
    control->touch();
    controlDoc->recompute();
    EXPECT_GT(hits, 0);
    expectEqualPostcondition(capturePostcondition(*box), capturePostcondition(*control));
    connection.disconnect();
    EXPECT_TRUE(qualifyForDetachedExecution(*box).admitted);
}

TEST_F(DetachedKernelQualificationTest, frozenObjectIsNotAdmitted)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    box->Placement.setValue(rotatedPlacement());
    _document->recompute();
    auto* controlDoc = makeControl("QualFreeze");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    control->Placement.setValue(box->Placement.getValue());
    controlDoc->recompute();

    box->freeze();
    const auto decision = qualifyForDetachedExecution(*box);
    EXPECT_TRUE(decision.has(AdmissionRefusal::FreezeOrSkip));
    EXPECT_FALSE(decision.detached);
    box->touch();
    _document->recompute();
    control->touch();
    controlDoc->recompute();
    const auto frozenState = capturePostcondition(*box);
    const auto controlState = capturePostcondition(*control);
    EXPECT_EQ(frozenState.status, "Freezed");
    EXPECT_EQ(controlState.status, "Valid");
    expectEqualPostcondition(frozenState, controlState, true, false);
    box->unfreeze(/*noRecompute=*/true);
    EXPECT_TRUE(qualifyForDetachedExecution(*box).admitted);
}

TEST_F(DetachedKernelQualificationTest, skipRecomputeIsNotAdmitted)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    box->Placement.setValue(rotatedPlacement());
    _document->recompute();
    auto* controlDoc = makeControl("QualSkip");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    control->Placement.setValue(box->Placement.getValue());
    controlDoc->recompute();

    _document->setStatus(App::Document::SkipRecompute, true);
    const auto decision = qualifyForDetachedExecution(*box);
    EXPECT_TRUE(decision.has(AdmissionRefusal::FreezeOrSkip));
    EXPECT_FALSE(decision.has(AdmissionRefusal::ColdOrRestore));
    box->touch();
    _document->recompute(/*objs=*/{}, /*force=*/false);
    control->touch();
    controlDoc->recompute();
    const auto skipped = capturePostcondition(*box);
    const auto controlState = capturePostcondition(*control);
    EXPECT_EQ(skipped.status, "Touched");
    EXPECT_EQ(controlState.status, "Valid");
    expectEqualPostcondition(skipped, controlState, true, false);
    _document->setStatus(App::Document::SkipRecompute, false);
    EXPECT_TRUE(qualifyForDetachedExecution(*box).admitted);
}

TEST_F(DetachedKernelQualificationTest, partialDocIsNotAdmitted)
{
    expectColdDocumentFlag(App::Document::PartialDoc);
}

TEST_F(DetachedKernelQualificationTest, partialRestoreIsNotAdmitted)
{
    expectColdDocumentFlag(App::Document::PartialRestore);
}

TEST_F(DetachedKernelQualificationTest, recomputeOnRestoreIsNotAdmitted)
{
    expectColdDocumentFlag(App::Document::RecomputeOnRestore);
}

TEST_F(DetachedKernelQualificationTest, documentRestoringIsNotAdmitted)
{
    expectColdDocumentFlag(App::Document::Restoring);
}

TEST_F(DetachedKernelQualificationTest, objectRestoringIsNotAdmitted)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    box->Placement.setValue(rotatedPlacement());
    _document->recompute();
    auto* controlDoc = makeControl("QualObjRestore");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    control->Placement.setValue(box->Placement.getValue());
    controlDoc->recompute();

    box->setStatus(App::Restore, true);
    const auto decision = qualifyForDetachedExecution(*box);
    EXPECT_TRUE(decision.has(AdmissionRefusal::ColdOrRestore));
    EXPECT_FALSE(decision.detached);
    box->touch();
    _document->recompute();
    control->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*box), capturePostcondition(*control));
    box->setStatus(App::Restore, false);
    EXPECT_TRUE(qualifyForDetachedExecution(*box).admitted);
}

TEST_F(DetachedKernelQualificationTest, shapeUser1IsNotAdmitted)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    box->Placement.setValue(rotatedPlacement());
    _document->recompute();
    auto* controlDoc = makeControl("QualUser1");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    control->Placement.setValue(box->Placement.getValue());
    controlDoc->recompute();

    box->Shape.setStatus(App::Property::User1, true);
    const auto decision = qualifyForDetachedExecution(*box);
    EXPECT_TRUE(decision.has(AdmissionRefusal::ColdOrRestore));
    EXPECT_FALSE(decision.detached);
    EXPECT_TRUE(box->Shape.testStatus(App::Property::User1));
    box->Shape.touch();
    control->Shape.touch();
    EXPECT_FALSE(box->Shape.testStatus(App::Property::User1));
    expectEqualPostcondition(capturePostcondition(*box), capturePostcondition(*control));
    EXPECT_TRUE(qualifyForDetachedExecution(*box).admitted);
}

TEST_F(DetachedKernelQualificationTest, restoredDocumentMatchesControlWithoutClearingFlags)
{
    auto* box = addBox(*_document, "Box");
    ASSERT_NE(box, nullptr);
    box->Placement.setValue(rotatedPlacement());
    _document->recompute();
    auto* controlDoc = makeControl("QualOpen");
    ASSERT_NE(controlDoc, nullptr);
    auto* control = addBox(*controlDoc, "Box");
    ASSERT_NE(control, nullptr);
    control->Placement.setValue(box->Placement.getValue());
    controlDoc->recompute();

    TempArchive archive(std::filesystem::temp_directory_path() / "prc-m0-2a" / "restore-box.FCStd");
    ASSERT_TRUE(_document->saveAs(archive.path().string().c_str()));
    App::GetApplication().closeDocument(_name.c_str());
    _document = nullptr;
    auto* restored = App::GetApplication().openDocument(archive.path().string().c_str());
    ASSERT_NE(restored, nullptr);
    _document = restored;
    _name = restored->getName();
    auto* restoredBox = dynamic_cast<Part::Box*>(restored->getObject("Box"));
    ASSERT_NE(restoredBox, nullptr);
    const auto opened = qualifyForDetachedExecution(*restoredBox);
    expectPendingBlocksDetach(opened);
    restoredBox->touch();
    setTrace(true);
    restored->recompute();
    const auto record = restored->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectNoTransfer(*record);
    setTrace(false);
    control->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*restoredBox), capturePostcondition(*control));
    EXPECT_FALSE(capturePostcondition(*restoredBox).error);
}

TEST_F(DetachedKernelQualificationTest, detachedBoxHarnessMatchesOwnerThreadAndIsolatedProcess)
{
    // Venue (a) is the owner thread: Document::recompute.
    // Venue (b) is one std::thread. Its raw shape is published by a test-only
    // owner step (seeded element map, rotated placement, recomputing Shape set).
    // Venue (c) is RecomputeVenue::Isolated through GenericIsolatedRecompute
    // and GeometryProcessBackend, which spawns FreeCADCmd. There is no Box-only
    // process. Production scheduling is unchanged.
    const auto placement = rotatedPlacement();
    auto* owner = addBox(*_document, "Owner");
    ASSERT_NE(owner, nullptr);
    owner->Placement.setValue(placement);
    _document->recompute();
    installSeededMap(*owner);

    CloseDocument processGuard;
    processGuard.name = App::GetApplication().getUniqueDocumentName("QualProcess");
    auto* processDoc = App::GetApplication().newDocument(processGuard.name.c_str(), "process");
    ASSERT_NE(processDoc, nullptr);
    auto* processBox = addBox(*processDoc, "Process");
    ASSERT_NE(processBox, nullptr);
    processBox->Placement.setValue(placement);
    processDoc->recompute();
    installSeededMap(*processBox);

    CloseDocument publishGuard;
    publishGuard.name = App::GetApplication().getUniqueDocumentName("QualPublish");
    auto* publishDoc = App::GetApplication().newDocument(publishGuard.name.c_str(), "publish");
    ASSERT_NE(publishDoc, nullptr);
    auto* published = addBox(*publishDoc, "Published");
    ASSERT_NE(published, nullptr);
    published->Placement.setValue(placement);
    publishDoc->recompute();

    owner->touch();
    setTrace(true);
    _document->recompute();
    const auto ownerTrace = _document->lastRecomputeTrace();
    ASSERT_NE(ownerTrace, nullptr);
    expectNoTransfer(*ownerTrace);

    processBox->touch();
    const auto processSnapshot = waitIsolated(*processDoc, *processBox);
    ASSERT_TRUE(processSnapshot.terminal()) << processSnapshot.diagnostic;
    EXPECT_EQ(processSnapshot.state, App::DocumentRecomputeState::Completed)
        << processSnapshot.diagnostic;
    const auto processTrace = processDoc->lastRecomputeTrace();
    ASSERT_NE(processTrace, nullptr);
    EXPECT_GT(phaseSum(*processTrace, App::RecomputePhase::Transfer), 0);
    setTrace(false);

    const auto frozen = freezeBox(*owner);
    EXPECT_FALSE(frozen.placement.getRotation().isIdentity());
    const auto beforeThread = capturePostcondition(*owner);
    const auto ownerKernel = executeBoxKernel(frozen);
    std::int64_t threadExecuteNs = 0;
    BoxKernelResult threadKernel;
    const auto wallStart = std::chrono::steady_clock::now();
    std::thread worker([frozen, &threadKernel, &threadExecuteNs] {
        const auto started = std::chrono::steady_clock::now();
        threadKernel = executeBoxKernel(frozen);
        threadExecuteNs = elapsedNs(started);
    });
    worker.join();
    static_cast<void>(elapsedNs(wallStart));
    static_cast<void>(threadExecuteNs);
    expectEqualPostcondition(beforeThread, capturePostcondition(*owner));
    ASSERT_TRUE(ownerKernel.succeeded);
    ASSERT_TRUE(threadKernel.succeeded);
    EXPECT_EQ(ownerKernel.diagnostic, threadKernel.diagnostic);
    expectBoxTopology(ownerKernel.shape);
    expectVerticesNear(ownerKernel.shape, threadKernel.shape);
    expectVerticesNear(ownerKernel.shape, owner->Shape.getValue().Located(TopLoc_Location()));

    publishKernel(*published, threadKernel.shape);
    // Document::recompute clears Touch after execute. The test-only owner
    // step writes Shape directly, so clear that touch before comparing status.
    published->purgeTouched();
    expectVenueEqual(*owner, *published);
    // The isolated worker archive stores the OCC brep. The seeded element map
    // does not come back with the process result. Geometry, placement and
    // status still match. The same owner step applied to that raw shape
    // reproduces the seeded map.
    expectVenueEqual(*owner, *processBox, /*compareElementMap=*/false);
    EXPECT_TRUE(elementMapText(processBox->Shape.getShape()).empty());
    auto* namedFromProcess = addBox(*publishDoc, "FromProcess");
    ASSERT_NE(namedFromProcess, nullptr);
    namedFromProcess->Placement.setValue(placement);
    publishDoc->recompute();
    publishKernel(*namedFromProcess,
                  processBox->Shape.getValue().Located(TopLoc_Location()));
    namedFromProcess->purgeTouched();
    expectVenueEqual(*published, *namedFromProcess);

    const FrozenBoxInputs invalid {1e-12, 20, 30, Precision::Confusion(), placement};
    const auto invalidOwner = executeBoxKernel(invalid);
    BoxKernelResult invalidThread;
    std::thread invalidWorker([invalid, &invalidThread] { invalidThread = executeBoxKernel(invalid); });
    invalidWorker.join();
    EXPECT_FALSE(invalidOwner.succeeded);
    EXPECT_FALSE(invalidThread.succeeded);
    EXPECT_TRUE(invalidOwner.shape.IsNull());
    EXPECT_TRUE(invalidThread.shape.IsNull());
    EXPECT_EQ(invalidOwner.diagnostic, "Length of box too small");
    EXPECT_EQ(invalidThread.diagnostic, invalidOwner.diagnostic);

    auto* failedOwner = addBox(*_document, "FailedOwner");
    ASSERT_NE(failedOwner, nullptr);
    failedOwner->Placement.setValue(placement);
    _document->recompute();
    installSeededMap(*failedOwner);
    const auto seededMap = elementMapText(failedOwner->Shape.getShape());
    EXPECT_FALSE(seededMap.empty());
    const auto failedBefore = capturePostcondition(*failedOwner);
    failedOwner->Length.setValue(1e-12);
    auto* failedProcess = addBox(*processDoc, "FailedProcess");
    ASSERT_NE(failedProcess, nullptr);
    failedProcess->Placement.setValue(placement);
    processDoc->recompute();
    installSeededMap(*failedProcess);
    EXPECT_EQ(elementMapText(failedProcess->Shape.getShape()), seededMap);
    failedProcess->Length.setValue(1e-12);
    failedOwner->touch();
    _document->recompute();
    const auto failedSnapshot = waitIsolated(*processDoc, *failedProcess);
    ASSERT_TRUE(failedSnapshot.terminal()) << failedSnapshot.diagnostic;
    EXPECT_TRUE(failedOwner->isError());
    EXPECT_TRUE(failedProcess->isError());
    EXPECT_EQ(std::string(failedOwner->getStatusString()), std::string(failedProcess->getStatusString()));
    EXPECT_NE(std::string(failedOwner->getStatusString()).find("Length of box too small"),
              std::string::npos);
    EXPECT_NEAR(PartTestHelpers::getVolume(failedOwner->Shape.getValue()), failedBefore.volume, 1e-4);
    EXPECT_NEAR(PartTestHelpers::getVolume(failedProcess->Shape.getValue()), failedBefore.volume, 1e-4);
    EXPECT_EQ(elementMapText(failedOwner->Shape.getShape()), seededMap);
    EXPECT_EQ(elementMapText(failedProcess->Shape.getShape()), seededMap);
    EXPECT_TRUE(failedOwner->Placement.getValue().isSame(placement, 1e-12));
    EXPECT_TRUE(failedProcess->Placement.getValue().isSame(placement, 1e-12));
}

TEST_F(DetachedKernelQualificationTest, boxVenueCostsWideAndNarrow)
{
#if !PRC_M0_HAS_PROCESS_COST_PROBE
    GTEST_SKIP() << "Isolated-process cost probe needs Linux /proc and posix_spawn. "
                    "Qualification and harness tests still run.";
#else
    // Paid is the end-to-end wall on every venue: it starts before capture and
    // stops after commit. Owner wall is Document::recompute. Process wall is
    // waitIsolated. Thread wall is the freeze, the worker, and the test-only
    // owner Shape apply. The same warmup and repetition counts are used.
    // Results stay alive until after the timed region. Validation is outside
    // that region. Thread transfer stays 0: that column is the isolated-process
    // decode span only. The process wall is partitioned from the phase trace
    // and from the worker workspace files and FreeCADCmd lifetime.
    setTrace(true);
    auto* narrow = addBox(*_document, "Narrow");
    ASSERT_NE(narrow, nullptr);
    narrow->Placement.setValue(rotatedPlacement());
    _document->recompute();
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        narrow->touch();
        _document->recompute();
    }
    std::vector<PhaseSample> ownerNarrow;
    for (int rep = 0; rep < kRepetitions; ++rep) {
        narrow->touch();
        const auto wallStart = std::chrono::steady_clock::now();
        _document->recompute();
        const auto wallNs = elapsedNs(wallStart);
        const auto record = _document->lastRecomputeTrace();
        ASSERT_NE(record, nullptr);
        expectNoTransfer(*record);
        ownerNarrow.push_back({phaseSum(*record, App::RecomputePhase::Capture),
                               phaseSum(*record, App::RecomputePhase::Execute),
                               0,
                               phaseSum(*record, App::RecomputePhase::Commit),
                               wallNs});
    }

    std::vector<BoxKernelResult> threadNarrow;
    std::vector<PhaseSample> threadNarrowCost;
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        const auto inputs = freezeBox(*narrow);
        BoxKernelResult warmupResult;
        std::thread warmupThread([inputs, &warmupResult] {
            warmupResult = executeBoxKernel(inputs);
        });
        warmupThread.join();
        applyRawShape(*narrow, warmupResult.shape);
    }
    for (int rep = 0; rep < kRepetitions; ++rep) {
        BoxKernelResult result;
        std::int64_t captureNs = 0;
        std::int64_t executeNs = 0;
        std::int64_t commitNs = 0;
        const auto wallStart = std::chrono::steady_clock::now();
        const auto inputs = freezeBox(*narrow);
        captureNs = elapsedNs(wallStart);
        std::thread worker([inputs, &result, &executeNs] {
            const auto started = std::chrono::steady_clock::now();
            result = executeBoxKernel(inputs);
            executeNs = elapsedNs(started);
        });
        worker.join();
        const auto commitStart = std::chrono::steady_clock::now();
        applyRawShape(*narrow, result.shape);
        commitNs = elapsedNs(commitStart);
        const auto wallNs = elapsedNs(wallStart);
        threadNarrow.push_back(std::move(result));
        threadNarrowCost.push_back({captureNs, executeNs, 0, commitNs, wallNs});
    }

    CloseDocument processGuard;
    processGuard.name = App::GetApplication().getUniqueDocumentName("QualCostProcess");
    auto* processDoc = App::GetApplication().newDocument(processGuard.name.c_str(), "cost-process");
    ASSERT_NE(processDoc, nullptr);
    auto* processBox = addBox(*processDoc, "Process");
    ASSERT_NE(processBox, nullptr);
    processBox->Placement.setValue(narrow->Placement.getValue());
    processDoc->recompute();
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        processBox->touch();
        const auto snapshot = waitIsolated(*processDoc, *processBox);
        ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    }
    std::vector<PhaseSample> processNarrow;
    std::vector<ProcessAccount> processAccounts;
    for (int rep = 0; rep < kRepetitions; ++rep) {
        processBox->touch();
        const auto watchState = prepareIsolatedWatch();
        std::atomic_bool stop {false};
        ProcessMarks marks;
        const auto wallStart = std::chrono::steady_clock::now();
        std::thread watcher(
            [&] { marks = watchIsolatedProcess(wallStart, stop, watchState); });
        const auto snapshot = waitIsolated(*processDoc, *processBox);
        const auto wallNs = elapsedNs(wallStart);
        stop.store(true, std::memory_order_release);
        watcher.join();
        ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
        EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed) << snapshot.diagnostic;
        const auto record = processDoc->lastRecomputeTrace();
        ASSERT_NE(record, nullptr);
        PhaseSample sample {phaseSum(*record, App::RecomputePhase::Capture),
                            phaseSum(*record, App::RecomputePhase::Execute),
                            phaseSum(*record, App::RecomputePhase::Transfer),
                            phaseSum(*record, App::RecomputePhase::Commit),
                            wallNs};
        appendCost(std::string("process-stamps rep=") + std::to_string(rep)
                   + " workspace_ns=" + std::to_string(marks.workspaceNs)
                   + " request_ns=" + std::to_string(marks.requestNs)
                   + " spawn_ns=" + std::to_string(marks.spawnNs)
                   + " finger_start_ns=" + std::to_string(marks.fingerprintStartNs)
                   + " finger_end_ns=" + std::to_string(marks.fingerprintEndNs)
                   + " heartbeat_ns=" + std::to_string(marks.heartbeatNs)
                   + " part_ns=" + std::to_string(marks.partNs)
                   + " result_ns=" + std::to_string(marks.resultNs)
                   + " exit_ns=" + std::to_string(marks.exitNs)
                   + " published_ns=" + std::to_string(marks.publishedNs)
                   + " pid=" + std::to_string(marks.pid)
                   + " capture_ns=" + std::to_string(sample.captureNs)
                   + " execute_ns=" + std::to_string(sample.executeNs)
                   + " transfer_ns=" + std::to_string(sample.transferNs)
                   + " commit_ns=" + std::to_string(sample.commitNs)
                   + " wall_ns=" + std::to_string(wallNs));
        processNarrow.push_back(sample);
        processAccounts.push_back(accountProcess(marks, sample));
    }

    auto probeCommand = [](const char* command) {
        for (int warmup = 0; warmup < kWarmup; ++warmup) {
            static_cast<void>(emptyFreeCadCmdStartupNs(command));
        }
        std::vector<std::int64_t> samples;
        for (int rep = 0; rep < kRepetitions; ++rep) {
            samples.push_back(emptyFreeCadCmdStartupNs(command));
        }
        appendCost(std::string("probe command=") + command
                   + " ms=" + milliseconds(medianOf(samples)));
    };
    probeCommand("import FreeCAD");
    probeCommand("import Part");
    {
        auto exe = std::filesystem::path(App::Application::getHomePath()) / "bin" / "FreeCADCmd";
        if (!std::filesystem::exists(exe)) {
            exe = std::filesystem::path(App::Application::getHomePath()) / "FreeCADCmd";
        }
        const auto started = std::chrono::steady_clock::now();
        const auto fingerprint = App::Internal::geometryWorkerBuildFingerprint(exe);
        EXPECT_FALSE(fingerprint.empty());
        appendCost("probe fingerprint_ms=" + milliseconds(elapsedNs(started)));
    }

    std::vector<Part::Box*> wideBoxes;
    for (int index = 0; index < kWideBoxes; ++index) {
        auto* box = addBox(*_document, ("Wide" + std::to_string(index)).c_str());
        ASSERT_NE(box, nullptr);
        box->Placement.setValue(Base::Placement(
            Base::Vector3d(60.0 * index, 1, 2),
            Base::Rotation(Base::Vector3d(0, 0, 1), 0.2)));
        wideBoxes.push_back(box);
    }
    _document->recompute();
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        for (auto* box : wideBoxes) {
            box->touch();
        }
        _document->recompute();
    }
    std::vector<PhaseSample> ownerWide;
    std::size_t wideWidth = 0;
    for (int rep = 0; rep < kRepetitions; ++rep) {
        for (auto* box : wideBoxes) {
            box->touch();
        }
        const auto wallStart = std::chrono::steady_clock::now();
        _document->recompute();
        const auto wallNs = elapsedNs(wallStart);
        const auto record = _document->lastRecomputeTrace();
        ASSERT_NE(record, nullptr);
        expectNoTransfer(*record);
        wideWidth = record->dag.maxWidth;
        ownerWide.push_back({phaseSum(*record, App::RecomputePhase::Capture),
                             phaseSum(*record, App::RecomputePhase::Execute),
                             0,
                             phaseSum(*record, App::RecomputePhase::Commit),
                             wallNs});
    }

    std::vector<std::vector<BoxKernelResult>> threadWide;
    std::vector<PhaseSample> threadWideCost;
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        std::vector<FrozenBoxInputs> inputs;
        for (auto* box : wideBoxes) {
            inputs.push_back(freezeBox(*box));
        }
        std::vector<BoxKernelResult> warmupResults(inputs.size());
        std::thread warmupThread([&inputs, &warmupResults] {
            for (std::size_t index = 0; index < inputs.size(); ++index) {
                warmupResults[index] = executeBoxKernel(inputs[index]);
            }
        });
        warmupThread.join();
        for (std::size_t index = 0; index < warmupResults.size(); ++index) {
            applyRawShape(*wideBoxes[index], warmupResults[index].shape);
        }
    }
    for (int rep = 0; rep < kRepetitions; ++rep) {
        std::vector<FrozenBoxInputs> inputs;
        inputs.reserve(wideBoxes.size());
        std::vector<BoxKernelResult> results(wideBoxes.size());
        std::int64_t captureNs = 0;
        std::int64_t executeNs = 0;
        std::int64_t commitNs = 0;
        const auto wallStart = std::chrono::steady_clock::now();
        for (auto* box : wideBoxes) {
            inputs.push_back(freezeBox(*box));
        }
        captureNs = elapsedNs(wallStart);
        std::thread worker([&inputs, &results, &executeNs] {
            const auto started = std::chrono::steady_clock::now();
            for (std::size_t index = 0; index < inputs.size(); ++index) {
                results[index] = executeBoxKernel(inputs[index]);
            }
            executeNs = elapsedNs(started);
        });
        worker.join();
        const auto commitStart = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < results.size(); ++index) {
            applyRawShape(*wideBoxes[index], results[index].shape);
        }
        commitNs = elapsedNs(commitStart);
        const auto wallNs = elapsedNs(wallStart);
        threadWide.push_back(std::move(results));
        threadWideCost.push_back({captureNs, executeNs, 0, commitNs, wallNs});
    }
    setTrace(false);

    EXPECT_NEAR(PartTestHelpers::getVolume(narrow->Shape.getValue()), 6000.0, 1e-4);
    EXPECT_TRUE(narrow->Placement.getValue().getRotation().isIdentity() == false);
    for (const auto& result : threadNarrow) {
        EXPECT_TRUE(result.succeeded);
        EXPECT_NEAR(PartTestHelpers::getVolume(result.shape), 6000.0, 1e-4);
        EXPECT_FALSE(result.shape.IsNull());
    }
    EXPECT_NEAR(PartTestHelpers::getVolume(processBox->Shape.getValue()), 6000.0, 1e-4);
    EXPECT_EQ(wideWidth, static_cast<std::size_t>(kWideBoxes));
    for (const auto& rep : threadWide) {
        ASSERT_EQ(rep.size(), static_cast<std::size_t>(kWideBoxes));
        for (const auto& result : rep) {
            EXPECT_TRUE(result.succeeded);
            EXPECT_NEAR(PartTestHelpers::getVolume(result.shape), 6000.0, 1e-4);
        }
    }
    for (auto* box : wideBoxes) {
        EXPECT_FALSE(box->Placement.getValue().isSame(Base::Placement(), 1e-9));
    }

    const auto summarize = [&](const char* venue,
                               const char* workload,
                               const std::vector<PhaseSample>& samples) {
        std::vector<std::int64_t> capture;
        std::vector<std::int64_t> execute;
        std::vector<std::int64_t> transfer;
        std::vector<std::int64_t> commit;
        std::vector<std::int64_t> wall;
        for (const auto& sample : samples) {
            capture.push_back(sample.captureNs);
            execute.push_back(sample.executeNs);
            transfer.push_back(sample.transferNs);
            commit.push_back(sample.commitNs);
            wall.push_back(sample.wallNs);
        }
        const auto captureNs = medianOf(capture);
        const auto executeNs = medianOf(execute);
        const auto transferNs = medianOf(transfer);
        const auto commitNs = medianOf(commit);
        const auto wallNs = medianOf(wall);
        appendCost(std::string("summary venue=") + venue + " workload=" + workload
                   + " warmup=" + std::to_string(kWarmup) + " reps=" + std::to_string(kRepetitions)
                   + " capture_ms=" + milliseconds(captureNs) + " execute_ms=" + milliseconds(executeNs)
                   + " transfer_ms=" + milliseconds(transferNs) + " commit_ms=" + milliseconds(commitNs)
                   + " wall_ms=" + milliseconds(wallNs) + " paid_ms=" + milliseconds(wallNs));
    };
    summarize("owner", "box:1", ownerNarrow);
    summarize("thread", "box:1", threadNarrowCost);
    summarize("process", "box:1", processNarrow);
    summarize("owner", "box:32", ownerWide);
    summarize("thread", "box:32", threadWideCost);
    ASSERT_EQ(processAccounts.size(), processNarrow.size());
    std::vector<std::size_t> order(processNarrow.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return processNarrow[left].wallNs < processNarrow[right].wallNs;
    });
    const auto& paid = processAccounts[order[order.size() / 2]];
    const auto paidSum = paid.captureNs + paid.parentFingerprintNs + paid.archiveNs + paid.spawnNs
        + paid.workerStartupNs + paid.importNs + paid.executeNs + paid.teardownNs + paid.decodeNs
        + paid.commitNs + paid.settleNs;
    EXPECT_NEAR(static_cast<double>(paidSum),
                static_cast<double>(paid.wallNs),
                static_cast<double>(paid.wallNs) * 0.05);
    appendCost(std::string("process-breakdown workload=box:1")
               + " capture_ms=" + milliseconds(paid.captureNs)
               + " parent_fingerprint_ms=" + milliseconds(paid.parentFingerprintNs)
               + " archive_ms=" + milliseconds(paid.archiveNs)
               + " spawn_ms=" + milliseconds(paid.spawnNs)
               + " worker_startup_ms=" + milliseconds(paid.workerStartupNs)
               + " import_ms=" + milliseconds(paid.importNs)
               + " execute_ms=" + milliseconds(paid.executeNs)
               + " teardown_ms=" + milliseconds(paid.teardownNs)
               + " decode_ms=" + milliseconds(paid.decodeNs)
               + " commit_ms=" + milliseconds(paid.commitNs)
               + " settle_ms=" + milliseconds(paid.settleNs)
               + " sum_ms=" + milliseconds(paidSum)
               + " wall_ms=" + milliseconds(paid.wallNs));
#endif
}

TEST_F(DetachedKernelQualificationTest, settledFuseAndCommonAreAdmittedAndStayOnTheOwner)
{
    for (const auto kind : {BooleanOp::Fuse, BooleanOp::Common}) {
        auto* document = kind == BooleanOp::Fuse ? _document : makeControl("QualCommonDoc");
        ASSERT_NE(document, nullptr);
        const auto prepared = kind == BooleanOp::Fuse
            ? prepareBoolean<Part::Fuse>(*document, "F")
            : prepareBoolean<Part::Common>(*document, "C");
        ASSERT_NE(prepared.feature, nullptr);
        document->recompute();
        const auto decision = qualifyForDetachedExecution(*prepared.feature);
        if (!decision.admitted) {
            for (const char* citation : decision.citations) {
                ADD_FAILURE() << (kind == BooleanOp::Fuse ? "fuse " : "common ")
                              << (citation ? citation : "");
            }
        }
        EXPECT_TRUE(decision.admitted);
        EXPECT_FALSE(decision.detached);
        EXPECT_FALSE(decision.pending.empty());
        EXPECT_EQ(prepared.feature->History.getSize(), 0);
        EXPECT_FALSE(prepared.feature->isError());
        prepared.feature->touch();
        document->recompute();
        EXPECT_FALSE(prepared.feature->isError());
        EXPECT_EQ(prepared.feature->History.getSize(), 0);
    }
}

TEST_F(DetachedKernelQualificationTest, operandFromLinkIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Lk");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    auto* link = dynamic_cast<App::Link*>(_document->addObject("App::Link", "LkLink"));
    ASSERT_NE(link, nullptr);
    link->setLink(-1, prepared.tool);
    prepared.feature->Tool.setValue(link);

    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandLink);
    const auto decision = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandScaledTransform));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandGroup));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandExternal));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandShapeCache));
    EXPECT_FALSE(decision.has(AdmissionRefusal::Link));
    EXPECT_EQ(link->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);

    auto* controlDoc = makeControl("QualOpLink");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Lk");
    ASSERT_NE(control.feature, nullptr);
    auto* controlLink = dynamic_cast<App::Link*>(controlDoc->addObject("App::Link", "LkLink"));
    ASSERT_NE(controlLink, nullptr);
    controlLink->setLink(-1, control.tool);
    control.feature->Tool.setValue(controlLink);
    controlDoc->recompute();
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectNoTransfer(*record);
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_FALSE(prepared.feature->isError());
    EXPECT_EQ(link->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    EXPECT_EQ(prepared.feature->History.getSize(), 0);
}

TEST_F(DetachedKernelQualificationTest, operandFromGroupIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Gp");
    ASSERT_NE(prepared.feature, nullptr);
    // App::Part is a GeoFeatureGroup and pulls the Boolean in with the tool.
    // A plain group refuses only the operand.
    auto* group = _document->addObject<App::DocumentObjectGroup>("GpGroup");
    ASSERT_NE(group, nullptr);
    group->addObject(prepared.tool);
    _document->recompute();
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandGroup);
    const auto decision = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_EQ(decision.reasons.size(), 1u);
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandLink));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandScaledTransform));
    EXPECT_FALSE(decision.has(AdmissionRefusal::ScalingOrPlacementTransform));

    auto* controlDoc = makeControl("QualOpGroup");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Gp");
    ASSERT_NE(control.feature, nullptr);
    auto* controlGroup = controlDoc->addObject<App::DocumentObjectGroup>("GpGroup");
    ASSERT_NE(controlGroup, nullptr);
    controlGroup->addObject(control.tool);
    controlDoc->recompute();
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_FALSE(prepared.feature->isError());
}

TEST_F(DetachedKernelQualificationTest, operandFromExternalDocumentIsNotAdmitted)
{
    CloseDocument foreignGuard;
    foreignGuard.name = App::GetApplication().getUniqueDocumentName("QualBoolForeign");
    auto* foreign = App::GetApplication().newDocument(foreignGuard.name.c_str(), "foreign");
    ASSERT_NE(foreign, nullptr);
    auto* foreignBox = addBox(*foreign, "ForeignBox", 10, 10, 30);
    ASSERT_NE(foreignBox, nullptr);
    foreignBox->Placement.setValue(Base::Placement(Base::Vector3d(15, 10, -5), Base::Rotation()));
    foreign->recompute();

    auto prepared = prepareBoolean<Part::Cut>(*_document, "Ex");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    auto* link = dynamic_cast<App::Link*>(_document->addObject("App::Link", "ExLink"));
    ASSERT_NE(link, nullptr);
    TempArchive ownerArchive(
        std::filesystem::temp_directory_path() / "prc-m0-2b" / "external-owner.FCStd");
    TempArchive foreignArchive(
        std::filesystem::temp_directory_path() / "prc-m0-2b" / "external-foreign.FCStd");
    ASSERT_TRUE(_document->saveAs(ownerArchive.path().string().c_str()));
    ASSERT_TRUE(foreign->saveAs(foreignArchive.path().string().c_str()));
    link->setLink(-1, foreignBox);
    prepared.feature->Tool.setValue(link);

    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandExternal);
    const auto decision = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_TRUE(decision.has(AdmissionRefusal::OperandLink));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandGroup));
    QualificationProbe probe;
    probe.suppress = true;
    probe.guard = AdmissionRefusal::OperandExternal;
    const auto broken = qualifyForDetachedExecution(*prepared.feature, probe);
    EXPECT_FALSE(broken.has(AdmissionRefusal::OperandExternal));
    EXPECT_TRUE(broken.has(AdmissionRefusal::OperandLink));

    auto* controlDoc = makeControl("QualOpExt");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Ex");
    ASSERT_NE(control.feature, nullptr);
    auto* controlLink = dynamic_cast<App::Link*>(controlDoc->addObject("App::Link", "ExLink"));
    ASSERT_NE(controlLink, nullptr);
    TempArchive controlArchive(
        std::filesystem::temp_directory_path() / "prc-m0-2b" / "external-control.FCStd");
    ASSERT_TRUE(controlDoc->saveAs(controlArchive.path().string().c_str()));
    controlLink->setLink(-1, foreignBox);
    control.feature->Tool.setValue(controlLink);
    controlDoc->recompute();
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_FALSE(prepared.feature->isError());
    EXPECT_EQ(prepared.feature->History.getSize(), 0);
}

TEST_F(DetachedKernelQualificationTest, operandScaledTransformIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Sc");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    gp_Trsf scale;
    scale.SetScale(gp_Pnt(0, 0, 0), 2.0);
    auto scaled = prepared.tool->Shape.getValue();
    scaled.Location(TopLoc_Location(scale));
    prepared.tool->Shape.setValue(scaled);
    ASSERT_FALSE(prepared.tool->Shape.getValue().IsNull());

    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandScaledTransform);
    const auto decision = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_EQ(decision.reasons.size(), 1u);
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandLink));

    auto* controlDoc = makeControl("QualOpScale");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Sc");
    ASSERT_NE(control.feature, nullptr);
    controlDoc->recompute();
    auto controlScaled = control.tool->Shape.getValue();
    controlScaled.Location(TopLoc_Location(scale));
    control.tool->Shape.setValue(controlScaled);
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_FALSE(prepared.feature->isError());
}

TEST_F(DetachedKernelQualificationTest, pythonOperandIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Py");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    auto* python = _document->addObject("Part::FeaturePython", "PyTool");
    ASSERT_NE(python, nullptr);
    prepared.feature->Tool.setValue(python);
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandPython);
    const auto decision = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_EQ(decision.reasons.size(), 1u);
    EXPECT_FALSE(decision.has(AdmissionRefusal::PythonFeature));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandLink));

    auto* controlDoc = makeControl("QualOpPy");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Py");
    ASSERT_NE(control.feature, nullptr);
    controlDoc->recompute();
    auto* controlPython = controlDoc->addObject("Part::FeaturePython", "PyTool");
    ASSERT_NE(controlPython, nullptr);
    control.feature->Tool.setValue(controlPython);
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_TRUE(prepared.feature->isError());
    EXPECT_TRUE(control.feature->isError());
}

TEST_F(DetachedKernelQualificationTest, operandShapeCacheIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Ca");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    EXPECT_EQ(prepared.tool->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    const auto direct = Part::Feature::getTopoShape(
        prepared.tool,
        Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform);
    EXPECT_FALSE(direct.isNull());
    EXPECT_EQ(prepared.tool->getDynamicPropertyByName("_Part_ShapeCache"), nullptr)
        << "a direct Part operand must not take the cache-writing branch";

    auto* cache = prepared.tool->addDynamicProperty(
        "Part::PropertyShapeCache",
        "_Part_ShapeCache",
        "Part",
        "Shape cache",
        App::Prop_NoPersist | App::Prop_Output | App::Prop_Hidden);
    ASSERT_NE(cache, nullptr);
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandShapeCache);
    const auto injected = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_EQ(injected.reasons.size(), 1u);
    EXPECT_FALSE(injected.has(AdmissionRefusal::DynamicShapeCache));

    auto* controlDoc = makeControl("QualOpCache");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Ca");
    ASSERT_NE(control.feature, nullptr);
    controlDoc->recompute();
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_NE(prepared.tool->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    EXPECT_EQ(control.tool->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);

    auto* source = addBox(*_document, "CacheSource", 10, 10, 10);
    auto* link = dynamic_cast<App::Link*>(_document->addObject("App::Link", "CacheLink"));
    ASSERT_NE(source, nullptr);
    ASSERT_NE(link, nullptr);
    link->setLink(-1, source);
    link->Scale.setValue(2.0);
    EXPECT_EQ(link->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    const auto resolved = Part::Feature::getTopoShape(
        link,
        Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform);
    EXPECT_FALSE(resolved.isNull());
    EXPECT_NE(link->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    prepared.feature->Tool.setValue(link);
    const auto populated = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_TRUE(populated.has(AdmissionRefusal::OperandShapeCache));
    EXPECT_TRUE(populated.has(AdmissionRefusal::OperandLink));
    EXPECT_TRUE(populated.has(AdmissionRefusal::OperandScaledTransform));
    QualificationProbe probe;
    probe.suppress = true;
    probe.guard = AdmissionRefusal::OperandShapeCache;
    const auto broken = qualifyForDetachedExecution(*prepared.feature, probe);
    EXPECT_FALSE(broken.has(AdmissionRefusal::OperandShapeCache));
    EXPECT_TRUE(broken.has(AdmissionRefusal::OperandLink));
    EXPECT_TRUE(broken.has(AdmissionRefusal::OperandScaledTransform));

    auto* populatedDoc = makeControl("QualOpCachePop");
    ASSERT_NE(populatedDoc, nullptr);
    auto populatedControl = prepareBoolean<Part::Cut>(*populatedDoc, "Ca");
    ASSERT_NE(populatedControl.feature, nullptr);
    auto* populatedSource = addBox(*populatedDoc, "CacheSource", 10, 10, 10);
    auto* populatedLink = dynamic_cast<App::Link*>(populatedDoc->addObject("App::Link", "CacheLink"));
    ASSERT_NE(populatedSource, nullptr);
    ASSERT_NE(populatedLink, nullptr);
    populatedLink->setLink(-1, populatedSource);
    populatedLink->Scale.setValue(2.0);
    const auto populatedResolved = Part::Feature::getTopoShape(
        populatedLink,
        Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform);
    EXPECT_FALSE(populatedResolved.isNull());
    EXPECT_NE(populatedLink->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    populatedControl.feature->Tool.setValue(populatedLink);
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    populatedControl.feature->touch();
    populatedDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*populatedControl.feature));
    EXPECT_NE(link->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    EXPECT_NE(populatedLink->getDynamicPropertyByName("_Part_ShapeCache"), nullptr);
    EXPECT_EQ(prepared.feature->History.getSize(), 0);
    EXPECT_EQ(populatedControl.feature->History.getSize(), 0);
}

TEST_F(DetachedKernelQualificationTest, refineEnabledIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Rf");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    prepared.feature->Refine.setValue(true);
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::RefineEnabled);
    EXPECT_EQ(qualifyForDetachedExecution(*prepared.feature).reasons.size(), 1u);

    auto* controlDoc = makeControl("QualRefine");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Rf");
    ASSERT_NE(control.feature, nullptr);
    control.feature->Refine.setValue(true);
    controlDoc->recompute();
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_TRUE(prepared.feature->Refine.getValue());
    EXPECT_FALSE(prepared.feature->isError());
}

TEST_F(DetachedKernelQualificationTest, historyWrittenIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Hi");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    ASSERT_EQ(prepared.feature->History.getSize(), 0);
    prepared.feature->History.setSize(1);
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::HistoryWritten);
    EXPECT_EQ(qualifyForDetachedExecution(*prepared.feature).reasons.size(), 1u);

    auto* controlDoc = makeControl("QualHist");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Hi");
    ASSERT_NE(control.feature, nullptr);
    controlDoc->recompute();
    control.feature->History.setSize(1);
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_EQ(prepared.feature->History.getSize(), 1);
    EXPECT_EQ(control.feature->History.getSize(), 1);
    EXPECT_FALSE(prepared.feature->isError());
}

TEST_F(DetachedKernelQualificationTest, operandNotFeatureIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Nf");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    auto* line = _document->addObject<App::Line>("NfLine");
    ASSERT_NE(line, nullptr);
    prepared.feature->Tool.setValue(line);
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandNotFeature);
    const auto decision = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_EQ(decision.reasons.size(), 1u);
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandLink));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandPython));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandExtension));

    auto* controlDoc = makeControl("QualOpNotFeature");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Nf");
    ASSERT_NE(control.feature, nullptr);
    controlDoc->recompute();
    auto* controlLine = controlDoc->addObject<App::Line>("NfLine");
    ASSERT_NE(controlLine, nullptr);
    control.feature->Tool.setValue(controlLine);
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
}

TEST_F(DetachedKernelQualificationTest, operandExtensionIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Exs");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    auto* extension = new Part::PrismExtension();
    extension->initExtension(prepared.tool);
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandExtension);
    const auto decision = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_EQ(decision.reasons.size(), 1u);
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandNotFeature));
    EXPECT_FALSE(decision.has(AdmissionRefusal::ExtensionSchema));

    auto* controlDoc = makeControl("QualOpExten");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Exs");
    ASSERT_NE(control.feature, nullptr);
    controlDoc->recompute();
    auto* controlExtension = new Part::PrismExtension();
    controlExtension->initExtension(control.tool);
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
}

TEST_F(DetachedKernelQualificationTest, operandAttachmentIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "At");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    auto* attachment = prepared.tool->getExtensionByType<Part::AttachExtension>(/*no_except=*/true);
    ASSERT_NE(attachment, nullptr);
    attachment->AttachmentSupport.setValue(prepared.base);
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandAttachment);
    const auto decision = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_EQ(decision.reasons.size(), 1u);
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandLink));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandSourceClosure));
    EXPECT_FALSE(decision.has(AdmissionRefusal::ActiveAttachment));

    auto* controlDoc = makeControl("QualOpAttach");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "At");
    ASSERT_NE(control.feature, nullptr);
    controlDoc->recompute();
    auto* controlAttachment = control.tool->getExtensionByType<Part::AttachExtension>(/*no_except=*/true);
    ASSERT_NE(controlAttachment, nullptr);
    controlAttachment->AttachmentSupport.setValue(control.base);
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
}

TEST_F(DetachedKernelQualificationTest, operandSourceClosureIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "So");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    auto* source = addBox(*_document, "SoSource", 8, 8, 8);
    auto* midBase = addBox(*_document, "SoMidBase", 20, 20, 20);
    auto* link = dynamic_cast<App::Link*>(_document->addObject("App::Link", "SoLink"));
    auto* mid = addBoolean<Part::Cut>(*_document, "SoMid");
    ASSERT_NE(source, nullptr);
    ASSERT_NE(midBase, nullptr);
    ASSERT_NE(link, nullptr);
    ASSERT_NE(mid, nullptr);
    link->setLink(-1, source);
    mid->Base.setValue(midBase);
    mid->Tool.setValue(link);
    _document->recompute();
    prepared.feature->Tool.setValue(mid);
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandSourceClosure);
    const auto decision = qualifyForDetachedExecution(*prepared.feature);
    EXPECT_EQ(decision.reasons.size(), 1u);
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandLink));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandNotFeature));
    EXPECT_FALSE(decision.has(AdmissionRefusal::OperandShapeCache));

    auto* controlDoc = makeControl("QualOpSource");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "So");
    ASSERT_NE(control.feature, nullptr);
    auto* controlSource = addBox(*controlDoc, "SoSource", 8, 8, 8);
    auto* controlMidBase = addBox(*controlDoc, "SoMidBase", 20, 20, 20);
    auto* controlLink = dynamic_cast<App::Link*>(controlDoc->addObject("App::Link", "SoLink"));
    auto* controlMid = addBoolean<Part::Cut>(*controlDoc, "SoMid");
    ASSERT_NE(controlSource, nullptr);
    ASSERT_NE(controlMidBase, nullptr);
    ASSERT_NE(controlLink, nullptr);
    ASSERT_NE(controlMid, nullptr);
    controlLink->setLink(-1, controlSource);
    controlMid->Base.setValue(controlMidBase);
    controlMid->Tool.setValue(controlLink);
    controlDoc->recompute();
    control.feature->Tool.setValue(controlMid);
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_FALSE(prepared.feature->isError());
    EXPECT_EQ(prepared.feature->History.getSize(), 0);
}

TEST_F(DetachedKernelQualificationTest, missingOperandIsNotAdmitted)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Ms");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    ASSERT_TRUE(qualifyForDetachedExecution(*prepared.feature).admitted);
    const auto before = capturePostcondition(*prepared.feature);
    prepared.feature->Tool.setValue(nullptr);
    expectGuardProvesTheRefusal(*prepared.feature, AdmissionRefusal::OperandMissing);
    EXPECT_EQ(qualifyForDetachedExecution(*prepared.feature).reasons.size(), 1u);

    auto* controlDoc = makeControl("QualMissing");
    ASSERT_NE(controlDoc, nullptr);
    auto control = prepareBoolean<Part::Cut>(*controlDoc, "Ms");
    ASSERT_NE(control.feature, nullptr);
    controlDoc->recompute();
    control.feature->Tool.setValue(nullptr);
    prepared.feature->touch();
    setTrace(true);
    _document->recompute();
    ASSERT_NE(_document->lastRecomputeTrace(), nullptr);
    expectNoTransfer(*_document->lastRecomputeTrace());
    setTrace(false);
    control.feature->touch();
    controlDoc->recompute();
    expectEqualPostcondition(capturePostcondition(*prepared.feature),
                             capturePostcondition(*control.feature));
    EXPECT_TRUE(prepared.feature->isError());
    EXPECT_NEAR(capturePostcondition(*prepared.feature).volume, before.volume, 1e-4);
    EXPECT_EQ(prepared.feature->History.getSize(), 0);
    EXPECT_EQ(capturePostcondition(*prepared.feature).elementMap,
              before.elementMap);
}

std::pair<Materials::Material, Materials::Material> twoLibraryMaterials()
{
    const auto fallback = Materials::MaterialManager::defaultMaterial();
    EXPECT_TRUE(static_cast<bool>(fallback));
    Materials::Material first;
    Materials::Material second;
    if (fallback) {
        first = *fallback;
        second = *fallback;
    }
    const auto local = Materials::MaterialManager::getManager().getLocalMaterials();
    EXPECT_TRUE(static_cast<bool>(local));
    std::size_t count = 0;
    if (fallback && local) {
        for (const auto& entry : *local) {
            if (!entry.second || entry.second->getUUID() == fallback->getUUID()) {
                continue;
            }
            if (count == 0) {
                first = *entry.second;
            }
            else {
                second = *entry.second;
                ++count;
                break;
            }
            ++count;
        }
    }
    EXPECT_EQ(count, 2u);
    if (fallback) {
        EXPECT_NE(first.getUUID(), fallback->getUUID());
        EXPECT_NE(second.getUUID(), fallback->getUUID());
    }
    EXPECT_NE(first.getUUID(), second.getUUID());
    return {first, second};
}

bool mapContains(const std::vector<std::string>& lines, const char* text)
{
    for (const auto& line : lines) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct DetachedBooleanRun
{
    BooleanKernelResult kernel;
    WorkerEnvironment before;
    WorkerEnvironment after;
    std::string globalDelta;
};

DetachedBooleanRun runDetachedBoolean(const FrozenBoolean& frozen, bool nameResult)
{
    DetachedBooleanRun run;
    std::thread worker([&] {
        run.before = captureWorkerEnvironment();
        run.kernel = executeBooleanKernel(
            frozen.kind,
            frozen.base.shape,
            frozen.tool.shape,
            frozen.base.label,
            frozen.tool.label);
        if (nameResult) {
            completeBooleanComputation(run.kernel, frozen);
        }
        else if (run.kernel.succeeded && run.kernel.operation) {
            run.kernel.validationRejected = frozenCheckRejects(frozen, run.kernel.shape);
        }
        run.after = captureWorkerEnvironment();
    });
    worker.join();
    run.globalDelta = describeWorkerEnvironmentDelta(run.before, run.after);
    expectWorkerComputationStable(run.before, run.after);
    appendCost(std::string("worker-globals changed=") + run.globalDelta
               + " pool_threads=" + std::to_string(run.after.poolThreads)
               + " pool_default_launch=" + std::to_string(run.after.poolDefaultLaunch)
               + " pool_in_use=" + (run.after.poolInUse ? "1" : "0")
#if !PRC_M0_HAS_POSIX_SIGNALS
               + " signals=skipped"
#endif
    );
    return run;
}

void expectBooleanTopology(const TopoDS_Shape& left, const TopoDS_Shape& right)
{
    ASSERT_FALSE(left.IsNull());
    ASSERT_FALSE(right.IsNull());
    EXPECT_EQ(countShapes(left, TopAbs_SOLID), countShapes(right, TopAbs_SOLID));
    EXPECT_EQ(countShapes(left, TopAbs_FACE), countShapes(right, TopAbs_FACE));
    EXPECT_EQ(countShapes(left, TopAbs_EDGE), countShapes(right, TopAbs_EDGE));
    EXPECT_EQ(countShapes(left, TopAbs_VERTEX), countShapes(right, TopAbs_VERTEX));
    EXPECT_NEAR(PartTestHelpers::getVolume(left), PartTestHelpers::getVolume(right), 1e-3);
    expectVerticesNear(left, right);
    expectVerticesNear(left.Located(TopLoc_Location()), right.Located(TopLoc_Location()));
}

struct SerialBoolean
{
    TopoDS_Shape shape;
    std::vector<std::string> elementMap;
    Postcondition post;
};

SerialBoolean snapshotBoolean(const Part::Feature& feature)
{
    SerialBoolean serial;
    serial.shape = feature.Shape.getValue();
    serial.elementMap = elementMapText(feature.Shape.getShape());
    serial.post = capturePostcondition(feature);
    return serial;
}

int testPartCount()
{
    return ::testing::UnitTest::GetInstance()->current_test_info()->result()->total_part_count();
}

// Harness comparator: topology plus the canonical element map. A false return
// means this result must not be timed.
bool harnessGeometryMatches(const TopoDS_Shape& actual, const SerialBoolean& serial)
{
    const int before = testPartCount();
    // Feature Placement is owner state baked into Shape.getShape() as a location.
    // The raw kernel shape does not carry it. Compare the unlocated solids.
    expectBooleanTopology(serial.shape.Located(TopLoc_Location()), actual.Located(TopLoc_Location()));
    return testPartCount() == before;
}

bool harnessBooleanMatches(const TopoDS_Shape& actual, const std::vector<std::string>& actualMap,
                           const SerialBoolean& serial)
{
    const int before = testPartCount();
    if (!harnessGeometryMatches(actual, serial)) {
        return false;
    }
    EXPECT_EQ(canonicalElementMap(serial.elementMap), canonicalElementMap(actualMap));
    return testPartCount() == before;
}

bool publishedBooleanMatches(const Part::Boolean& feature, const SerialBoolean& serial)
{
    const int before = testPartCount();
    expectEqualPostcondition(serial.post, capturePostcondition(feature));
    expectBooleanTopology(serial.shape, feature.Shape.getValue());
    return testPartCount() == before;
}

bool processBooleanMatches(const Part::Boolean& feature, const App::DocumentRecomputeSnapshot& snapshot,
                           const SerialBoolean& serial)
{
    if (!snapshot.terminal() || snapshot.state != App::DocumentRecomputeState::Completed
        || feature.isError() || serial.post.error) {
        ADD_FAILURE() << snapshot.diagnostic << " status=" << feature.getStatusString();
        return false;
    }
    const int before = testPartCount();
    expectBooleanTopology(serial.shape, feature.Shape.getValue());
    const auto published = capturePostcondition(feature);
    EXPECT_EQ(published.historySize, serial.post.historySize);
    EXPECT_EQ(published.materialUuid, serial.post.materialUuid);
    EXPECT_EQ(published.status, serial.post.status);
    EXPECT_FALSE(published.error);
    EXPECT_TRUE(feature.Placement.getValue().isSame(serial.post.placement, 1e-9));
    return testPartCount() == before;
}

TEST_F(DetachedKernelQualificationTest, booleanHarnessMatchesOwnerThreadAndIsolatedProcess)
{
    // Owner: Document::recompute. Thread: frozen copies, FCBRepAlgoAPI_*, BRepCheck
    // and makeElementShape inside the worker, then a test-only owner publication.
    // Process: RecomputeVenue::Isolated. Production scheduling is unchanged.
    const auto libraryMaterials = twoLibraryMaterials();
    const auto copiedMaterial = libraryMaterials.first;
    const auto preservedMaterial = libraryMaterials.second;
    const auto fallbackMaterial = Materials::MaterialManager::defaultMaterial();
    ASSERT_TRUE(static_cast<bool>(fallbackMaterial));
    const BooleanOp kinds[] = {BooleanOp::Cut, BooleanOp::Fuse, BooleanOp::Common};
    for (const auto kind : kinds) {
        const char* prefix = kind == BooleanOp::Cut ? "Hv" : kind == BooleanOp::Fuse ? "Hf" : "Hc";
        auto owner = kind == BooleanOp::Fuse ? prepareBoolean<Part::Fuse>(*_document, prefix)
            : kind == BooleanOp::Common      ? prepareBoolean<Part::Common>(*_document, prefix)
                                              : prepareBoolean<Part::Cut>(*_document, prefix);
        ASSERT_NE(owner.feature, nullptr);
        auto* processDoc = makeControl(prefix);
        ASSERT_NE(processDoc, nullptr);
        auto process = kind == BooleanOp::Fuse ? prepareBoolean<Part::Fuse>(*processDoc, prefix)
            : kind == BooleanOp::Common        ? prepareBoolean<Part::Common>(*processDoc, prefix)
                                                : prepareBoolean<Part::Cut>(*processDoc, prefix);
        ASSERT_NE(process.feature, nullptr);
        auto* publishDoc = makeControl("QualBoolPublish");
        ASSERT_NE(publishDoc, nullptr);
        auto published = kind == BooleanOp::Fuse ? prepareBoolean<Part::Fuse>(*publishDoc, prefix)
            : kind == BooleanOp::Common          ? prepareBoolean<Part::Common>(*publishDoc, prefix)
                                                  : prepareBoolean<Part::Cut>(*publishDoc, prefix);
        ASSERT_NE(published.feature, nullptr);
        if (kind == BooleanOp::Cut) {
            owner.base->ShapeMaterial.setValue(copiedMaterial);
            process.base->ShapeMaterial.setValue(copiedMaterial);
            published.base->ShapeMaterial.setValue(copiedMaterial);
        }
        else if (kind == BooleanOp::Fuse) {
            owner.feature->ShapeMaterial.setValue(preservedMaterial);
            process.feature->ShapeMaterial.setValue(preservedMaterial);
            published.feature->ShapeMaterial.setValue(preservedMaterial);
        }

        _document->recompute();
        installSeededMap(*owner.base);
        installSeededMap(*owner.tool);
        owner.feature->touch();
        setTrace(true);
        _document->recompute();
        const auto ownerTrace = _document->lastRecomputeTrace();
        ASSERT_NE(ownerTrace, nullptr);
        expectNoTransfer(*ownerTrace);
        setTrace(false);
        ASSERT_FALSE(owner.feature->isError()) << owner.feature->getStatusString();
        EXPECT_FALSE(elementMapText(owner.feature->Shape.getShape()).empty());
        EXPECT_EQ(owner.feature->History.getSize(), 0);
        EXPECT_FALSE(owner.feature->Placement.getValue().getRotation().isIdentity());

        processDoc->recompute();
        installSeededMap(*process.base);
        installSeededMap(*process.tool);
        process.feature->touch();
        const auto processSnapshot = waitIsolated(*processDoc, *process.feature);
        ASSERT_TRUE(processSnapshot.terminal()) << processSnapshot.diagnostic;
        EXPECT_EQ(processSnapshot.state, App::DocumentRecomputeState::Completed)
            << processSnapshot.diagnostic;
        ASSERT_FALSE(process.feature->isError()) << process.feature->getStatusString();

        publishDoc->recompute();
        installSeededMap(*published.base);
        installSeededMap(*published.tool);
        published.feature->touch();
        publishDoc->recompute();

        const auto liveBase = owner.base->Shape.getValue();
        const auto liveTool = owner.tool->Shape.getValue();
        const auto liveBaseIds = tshapeIds(liveBase);
        const auto liveToolIds = tshapeIds(liveTool);
        const auto baseMap = elementMapText(owner.base->Shape.getShape());
        const auto toolMap = elementMapText(owner.tool->Shape.getShape());
        const auto baseGeometry = serializedGeometry(liveBase);
        const auto toolGeometry = serializedGeometry(liveTool);
        if (kind == BooleanOp::Cut) {
            EXPECT_TRUE(TopoDS_Shape(liveBase).IsPartner(liveBase));
            EXPECT_FALSE(deepCopyShape(liveBase).IsPartner(liveBase));
            EXPECT_FALSE(anyPartner(deepCopyShape(liveBase), liveBase));
        }

        const auto frozen = freezeBoolean(*owner.feature, kind);
        EXPECT_FALSE(frozen.base.shape.IsPartner(liveBase));
        EXPECT_FALSE(frozen.tool.shape.IsPartner(liveTool));
        EXPECT_FALSE(anyPartner(frozen.base.shape, liveBase));
        EXPECT_FALSE(anyPartner(frozen.tool.shape, liveTool));
        std::vector<std::string> callbacksDuringKernel;
        CallbackConnections callbacks;
        callbacks.keep(_document->signalBeforeRecompute.connect(
            [&](const App::Document&) { callbacksDuringKernel.push_back("signalBeforeRecompute"); }));
        callbacks.keep(_document->signalRecomputedObject.connect(
            [&](const App::DocumentObject&) { callbacksDuringKernel.push_back("signalRecomputedObject"); }));
        callbacks.keep(owner.feature->signalChanged.connect(
            [&](const App::DocumentObject&, const App::Property&) {
                callbacksDuringKernel.push_back("signalChanged");
            }));

        const auto run = runDetachedBoolean(frozen, /*nameResult=*/true);
        EXPECT_TRUE(callbacksDuringKernel.empty());
        EXPECT_TRUE(owner.base->Shape.getValue().IsSame(liveBase));
        EXPECT_TRUE(owner.tool->Shape.getValue().IsSame(liveTool));
        EXPECT_EQ(tshapeIds(owner.base->Shape.getValue()), liveBaseIds);
        EXPECT_EQ(tshapeIds(owner.tool->Shape.getValue()), liveToolIds);
        EXPECT_EQ(elementMapText(owner.base->Shape.getShape()), baseMap);
        EXPECT_EQ(elementMapText(owner.tool->Shape.getShape()), toolMap);
        EXPECT_EQ(serializedGeometry(owner.base->Shape.getValue()), baseGeometry);
        EXPECT_EQ(serializedGeometry(owner.tool->Shape.getValue()), toolGeometry);
        ASSERT_TRUE(run.kernel.succeeded) << run.kernel.diagnostic;
        ASSERT_FALSE(run.kernel.validationRejected);
        ASSERT_NE(run.kernel.operation, nullptr);
        ASSERT_FALSE(run.kernel.named.isNull());
        EXPECT_TRUE(run.kernel.operation->RunParallel());
        EXPECT_FALSE(anyPartner(run.kernel.shape, liveBase));
        EXPECT_FALSE(anyPartner(run.kernel.shape, liveTool));
        Bnd_Box bounds;
        BRepBndLib::Add(frozen.base.shape, bounds);
        BRepBndLib::Add(frozen.tool.shape, bounds);
        const double expectedFuzzy =
            frozen.fuzzy * std::sqrt(bounds.SquareExtent()) * frozen.confusion;
        EXPECT_NEAR(run.kernel.operation->FuzzyValue(), expectedFuzzy, 1e-9);

        const auto stale = BRepPrimAPI_MakeBox(1.0, 2.0, 3.0).Shape();
        const double ownerVolume = PartTestHelpers::getVolume(owner.feature->Shape.getValue());
        const double staleVolume = PartTestHelpers::getVolume(stale);
        EXPECT_GT(std::abs(ownerVolume - staleVolume), 1.0);
        {
            RecomputeStatus status(*published.feature);
            published.feature->Shape.setValue(stale);
        }
        if (kind == BooleanOp::Cut) {
            published.feature->ShapeMaterial.setValue(*fallbackMaterial);
            EXPECT_EQ(published.feature->ShapeMaterial.getValue().getUUID(), fallbackMaterial->getUUID());
            EXPECT_NE(owner.base->ShapeMaterial.getValue().getUUID(), fallbackMaterial->getUUID());
        }
        else if (kind == BooleanOp::Fuse) {
            EXPECT_EQ(published.feature->ShapeMaterial.getValue().getUUID(), preservedMaterial.getUUID());
            EXPECT_NE(preservedMaterial.getUUID(), fallbackMaterial->getUUID());
        }
        published.feature->Placement.setValue(rotatedPlacement());
        EXPECT_NEAR(PartTestHelpers::getVolume(published.feature->Shape.getValue()), staleVolume, 1e-4);

        publishBooleanShape(*published.feature, run.kernel.named);
        EXPECT_GT(std::abs(PartTestHelpers::getVolume(published.feature->Shape.getValue()) - staleVolume), 1.0);
        if (kind == BooleanOp::Cut) {
            EXPECT_EQ(published.feature->ShapeMaterial.getValue().getUUID(), copiedMaterial.getUUID());
            EXPECT_EQ(owner.feature->ShapeMaterial.getValue().getUUID(), copiedMaterial.getUUID());
        }
        else if (kind == BooleanOp::Fuse) {
            EXPECT_EQ(published.feature->ShapeMaterial.getValue().getUUID(), preservedMaterial.getUUID());
            EXPECT_EQ(owner.feature->ShapeMaterial.getValue().getUUID(), preservedMaterial.getUUID());
            EXPECT_NE(published.feature->ShapeMaterial.getValue().getUUID(),
                      owner.base->ShapeMaterial.getValue().getUUID());
        }
        expectEqualPostcondition(capturePostcondition(*owner.feature),
                                 capturePostcondition(*published.feature));
        expectBooleanTopology(owner.feature->Shape.getValue(), published.feature->Shape.getValue());
        EXPECT_FALSE(elementMapText(published.feature->Shape.getShape()).empty());
        const auto ownerMap = canonicalElementMap(elementMapText(owner.feature->Shape.getShape()));
        EXPECT_TRUE(mapContains(ownerMap, "Qual"));
        EXPECT_EQ(ownerMap, canonicalElementMap(elementMapText(published.feature->Shape.getShape())));
        EXPECT_EQ(ownerMap, canonicalElementMap(elementMapText(run.kernel.named)));
        EXPECT_EQ(owner.feature->History.getSize(), 0);
        EXPECT_EQ(published.feature->History.getSize(), 0);
        EXPECT_TRUE(owner.feature->Placement.getValue().isSame(
            published.feature->Placement.getValue(),
            1e-9));

        expectBooleanTopology(owner.feature->Shape.getValue(), process.feature->Shape.getValue());
        EXPECT_EQ(process.feature->History.getSize(), 0);
        EXPECT_EQ(capturePostcondition(*owner.feature).materialUuid,
                  capturePostcondition(*process.feature).materialUuid);
        EXPECT_TRUE(owner.feature->Placement.getValue().isSame(process.feature->Placement.getValue(), 1e-9));
        EXPECT_EQ(std::string(owner.feature->getStatusString()),
                  std::string(process.feature->getStatusString()));
        // PropertyPartShape::Save omits element maps while isExporting, so the
        // child never sees the Qual seeds. Its execute builds a different map.
        // That map is not empty, and it is not the seeded names.
        const auto processMap = elementMapText(process.feature->Shape.getShape());
        EXPECT_FALSE(processMap.empty());
        EXPECT_FALSE(mapContains(processMap, "Qual"));
        EXPECT_NE(canonicalElementMap(processMap), ownerMap);
    }
}

TEST_F(DetachedKernelQualificationTest, ownerNamingOnFrozenInputsReproducesSeededNames)
{
    // Owner naming (makeElementShape on frozen operand inputs) reproduces the
    // seeded names. Export drops those seeds, and the restored process shape
    // carries the child's own map, which does not contain them. That is not
    // cross-process seeded-name equivalence. A process venue needs owner-side
    // re-naming.
    const BooleanOp kinds[] = {BooleanOp::Cut, BooleanOp::Fuse, BooleanOp::Common};
    for (const auto kind : kinds) {
        const char* prefix = kind == BooleanOp::Cut ? "Nm" : kind == BooleanOp::Fuse ? "Nf" : "Nc";
        auto owner = kind == BooleanOp::Fuse ? prepareBoolean<Part::Fuse>(*_document, prefix)
            : kind == BooleanOp::Common      ? prepareBoolean<Part::Common>(*_document, prefix)
                                              : prepareBoolean<Part::Cut>(*_document, prefix);
        ASSERT_NE(owner.feature, nullptr);
        _document->recompute();
        installSeededMap(*owner.base);
        installSeededMap(*owner.tool);
        owner.feature->touch();
        _document->recompute();
        ASSERT_FALSE(owner.feature->isError()) << owner.feature->getStatusString();
        const auto ownerMap = canonicalElementMap(elementMapText(owner.feature->Shape.getShape()));
        EXPECT_TRUE(mapContains(ownerMap, "Qual"));

        const auto frozen = freezeBoolean(*owner.feature, kind);
        EXPECT_TRUE(mapContains(elementMapText(frozen.base.named), "Qual"));
        EXPECT_TRUE(mapContains(elementMapText(frozen.tool.named), "Qual"));
        auto named = executeBooleanKernel(
            kind, frozen.base.shape, frozen.tool.shape, frozen.base.label, frozen.tool.label);
        completeBooleanComputation(named, frozen);
        ASSERT_TRUE(named.succeeded) << named.diagnostic;
        ASSERT_FALSE(named.validationRejected);
        ASSERT_FALSE(named.named.isNull());
        const auto namedMap = canonicalElementMap(elementMapText(named.named));
        EXPECT_TRUE(mapContains(namedMap, "Qual"));
        EXPECT_EQ(ownerMap, namedMap);

        auto* processDoc = makeControl(prefix);
        ASSERT_NE(processDoc, nullptr);
        auto process = kind == BooleanOp::Fuse ? prepareBoolean<Part::Fuse>(*processDoc, prefix)
            : kind == BooleanOp::Common        ? prepareBoolean<Part::Common>(*processDoc, prefix)
                                                : prepareBoolean<Part::Cut>(*processDoc, prefix);
        ASSERT_NE(process.feature, nullptr);
        processDoc->recompute();
        installSeededMap(*process.base);
        installSeededMap(*process.tool);
        process.feature->touch();
        const auto snapshot = waitIsolated(*processDoc, *process.feature);
        ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
        EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed) << snapshot.diagnostic;
        ASSERT_FALSE(process.feature->isError()) << process.feature->getStatusString();
        const auto processMap = elementMapText(process.feature->Shape.getShape());
        EXPECT_FALSE(processMap.empty());
        EXPECT_FALSE(mapContains(processMap, "Qual"));
        EXPECT_NE(canonicalElementMap(processMap), ownerMap);
    }
}

TEST_F(DetachedKernelQualificationTest, booleanOwnerCallbacksDoNotFireOnTheDetachedKernel)
{
    auto prepared = prepareBoolean<Part::Cut>(*_document, "Cb");
    ASSERT_NE(prepared.feature, nullptr);
    _document->recompute();
    std::vector<std::string> fired;
    CallbackConnections callbacks;
    callbacks.keep(_document->signalBeforeRecompute.connect(
        [&](const App::Document&) { fired.push_back("signalBeforeRecompute"); }));
    callbacks.keep(_document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>&) {
            fired.push_back("signalRecomputed");
        }));
    callbacks.keep(_document->signalRecomputedObject.connect(
        [&](const App::DocumentObject&) { fired.push_back("signalRecomputedObject"); }));
    callbacks.keep(_document->signalChangedObject.connect(
        [&](const App::DocumentObject&, const App::Property&) { fired.push_back("signalChangedObject"); }));
    callbacks.keep(_document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject&, const App::Property&) {
            fired.push_back("signalBeforeChangeObject");
        }));
    callbacks.keep(_document->signalBeforeChange.connect(
        [&](const App::Document&, const App::Property&) { fired.push_back("signalBeforeChange"); }));
    callbacks.keep(prepared.feature->signalChanged.connect(
        [&](const App::DocumentObject&, const App::Property&) { fired.push_back("feature.signalChanged"); }));
    callbacks.keep(prepared.feature->signalBeforeChange.connect(
        [&](const App::DocumentObject&, const App::Property&) {
            fired.push_back("feature.signalBeforeChange");
        }));
    callbacks.keep(prepared.base->signalChanged.connect(
        [&](const App::DocumentObject&, const App::Property&) { fired.push_back("base.signalChanged"); }));
    callbacks.keep(prepared.tool->signalChanged.connect(
        [&](const App::DocumentObject&, const App::Property&) { fired.push_back("tool.signalChanged"); }));

    fired.clear();
    prepared.feature->touch();
    _document->recompute();
    std::set<std::string> unique(fired.begin(), fired.end());
    EXPECT_TRUE(unique.count("signalBeforeRecompute"));
    EXPECT_TRUE(unique.count("signalRecomputed"));
    EXPECT_TRUE(unique.count("signalRecomputedObject"));
    std::string recorded;
    for (const auto& name : unique) {
        if (!recorded.empty()) {
            recorded += ",";
        }
        recorded += name;
    }
    appendCost("callbacks " + recorded);
    EXPECT_FALSE(recorded.empty());

    const auto frozen = freezeBoolean(*prepared.feature, BooleanOp::Cut);
    fired.clear();
    const auto run = runDetachedBoolean(frozen, /*nameResult=*/true);
    EXPECT_TRUE(fired.empty());
    ASSERT_TRUE(run.kernel.succeeded) << run.kernel.diagnostic;
    ASSERT_FALSE(run.kernel.named.isNull());

    publishBooleanShape(*prepared.feature, run.kernel.named);
    EXPECT_FALSE(fired.empty());
}

TEST_F(DetachedKernelQualificationTest, booleanFailurePreservesShapeOnEveryVenue)
{
    auto* base = addBox(*_document, "FailBase", 40, 30, 20);
    auto* tool = _document->addObject<Part::Feature>("FailEdge");
    auto* cut = addBoolean<Part::Cut>(*_document, "FailCut");
    ASSERT_NE(base, nullptr);
    ASSERT_NE(tool, nullptr);
    ASSERT_NE(cut, nullptr);
    BRepBuilderAPI_MakeEdge edge(gp_Pnt(0, 0, 0), gp_Pnt(12, 0, 0));
    tool->Shape.setValue(edge.Edge());
    cut->Base.setValue(base);
    cut->Tool.setValue(tool);
    const auto previous = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
    cut->Shape.setValue(previous);
    installSeededMap(*cut);
    // Shape::setValue outside recompute copies the shape placement back onto
    // the feature. Set the rotated placement after that write.
    cut->Placement.setValue(rotatedPlacement());
    const auto seeded = elementMapText(cut->Shape.getShape());
    const auto before = capturePostcondition(*cut);
    ASSERT_FALSE(seeded.empty());

    cut->touch();
    _document->recompute();
    EXPECT_TRUE(cut->isError()) << cut->getStatusString();
    const std::string ownerStatus = cut->getStatusString();
    EXPECT_NE(ownerStatus.find("Boolean operation failed"), std::string::npos) << ownerStatus;
    EXPECT_NEAR(PartTestHelpers::getVolume(cut->Shape.getValue()), before.volume, 1e-4);
    EXPECT_EQ(elementMapText(cut->Shape.getShape()), seeded);
    EXPECT_EQ(cut->History.getSize(), 0);
    EXPECT_TRUE(cut->Placement.getValue().isSame(rotatedPlacement(), 1e-9));

    auto* processDoc = makeControl("QualFailProcess");
    ASSERT_NE(processDoc, nullptr);
    auto* processBase = addBox(*processDoc, "FailBase", 40, 30, 20);
    auto* processTool = processDoc->addObject<Part::Feature>("FailEdge");
    auto* processCut = addBoolean<Part::Cut>(*processDoc, "FailCut");
    ASSERT_NE(processBase, nullptr);
    ASSERT_NE(processTool, nullptr);
    ASSERT_NE(processCut, nullptr);
    processTool->Shape.setValue(edge.Edge());
    processCut->Base.setValue(processBase);
    processCut->Tool.setValue(processTool);
    processCut->Shape.setValue(previous);
    installSeededMap(*processCut);
    processCut->Placement.setValue(rotatedPlacement());
    processCut->touch();
    const auto snapshot = waitIsolated(*processDoc, *processCut);
    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_TRUE(processCut->isError()) << processCut->getStatusString();
    EXPECT_EQ(std::string(processCut->getStatusString()), ownerStatus);
    EXPECT_NEAR(PartTestHelpers::getVolume(processCut->Shape.getValue()), before.volume, 1e-4);
    EXPECT_EQ(elementMapText(processCut->Shape.getShape()), seeded);
    EXPECT_EQ(processCut->History.getSize(), 0);
    EXPECT_TRUE(processCut->Placement.getValue().isSame(rotatedPlacement(), 1e-9));

    const auto frozen = freezeBoolean(*cut, BooleanOp::Cut);
    const auto run = runDetachedBoolean(frozen, /*nameResult=*/false);
    EXPECT_FALSE(run.kernel.succeeded);
    EXPECT_NE(run.kernel.diagnostic.find("Boolean operation failed"), std::string::npos) << run.kernel.diagnostic;
    EXPECT_NE(ownerStatus.find(run.kernel.diagnostic), std::string::npos) << run.kernel.diagnostic;
    EXPECT_TRUE(run.kernel.shape.IsNull());
    EXPECT_EQ(elementMapText(cut->Shape.getShape()), seeded);
    EXPECT_NEAR(PartTestHelpers::getVolume(cut->Shape.getValue()), before.volume, 1e-4);
}

TEST_F(DetachedKernelQualificationTest, booleanCheckModelInvalidPreservesShape)
{
    booleanPreferences()->SetBool("CheckModel", true);
    auto* base = _document->addObject<Part::Feature>("BadBase");
    auto* tool = addBox(*_document, "FarTool", 10, 10, 10);
    auto* cut = addBoolean<Part::Cut>(*_document, "BadCut");
    ASSERT_NE(base, nullptr);
    ASSERT_NE(tool, nullptr);
    ASSERT_NE(cut, nullptr);
    const auto solid = unorientableSolid();
    ASSERT_FALSE(solid.IsNull());
    ASSERT_FALSE(BRepCheck_Analyzer(solid).IsValid());
    base->Shape.setValue(solid);
    tool->Placement.setValue(Base::Placement(Base::Vector3d(1000, 0, 0), Base::Rotation()));
    cut->Base.setValue(base);
    cut->Tool.setValue(tool);
    const auto previous = BRepPrimAPI_MakeBox(8, 8, 8).Shape();
    cut->Shape.setValue(previous);
    installSeededMap(*cut);
    const auto seeded = elementMapText(cut->Shape.getShape());
    const auto before = capturePostcondition(*cut);

    cut->touch();
    _document->recompute();
    EXPECT_TRUE(cut->isError());
    const std::string ownerStatus = cut->getStatusString();
    EXPECT_NE(ownerStatus.find("Resulting shape is invalid"), std::string::npos) << ownerStatus;
    EXPECT_NEAR(PartTestHelpers::getVolume(cut->Shape.getValue()), before.volume, 1e-4);
    EXPECT_EQ(elementMapText(cut->Shape.getShape()), seeded);
    EXPECT_EQ(cut->History.getSize(), 0);
    EXPECT_EQ(capturePostcondition(*cut).materialUuid, before.materialUuid);

    auto* processDoc = makeControl("QualCheckProcess");
    ASSERT_NE(processDoc, nullptr);
    auto* processBase = processDoc->addObject<Part::Feature>("BadBase");
    auto* processTool = addBox(*processDoc, "FarTool", 10, 10, 10);
    auto* processCut = addBoolean<Part::Cut>(*processDoc, "BadCut");
    ASSERT_NE(processBase, nullptr);
    ASSERT_NE(processTool, nullptr);
    ASSERT_NE(processCut, nullptr);
    processBase->Shape.setValue(solid);
    processTool->Placement.setValue(tool->Placement.getValue());
    processCut->Base.setValue(processBase);
    processCut->Tool.setValue(processTool);
    processCut->Shape.setValue(previous);
    installSeededMap(*processCut);
    processCut->touch();
    const auto snapshot = waitIsolated(*processDoc, *processCut);
    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_TRUE(processCut->isError()) << processCut->getStatusString();
    EXPECT_NE(std::string(processCut->getStatusString()).find("Resulting shape is invalid"),
              std::string::npos)
        << processCut->getStatusString();
    EXPECT_EQ(elementMapText(processCut->Shape.getShape()), seeded);
    EXPECT_NEAR(PartTestHelpers::getVolume(processCut->Shape.getValue()), before.volume, 1e-4);

    const auto frozen = freezeBoolean(*cut, BooleanOp::Cut);
    EXPECT_TRUE(frozen.checkModel);
    const auto run = runDetachedBoolean(frozen, /*nameResult=*/false);
    ASSERT_TRUE(run.kernel.succeeded) << run.kernel.diagnostic;
    EXPECT_TRUE(run.kernel.validationRejected);
    EXPECT_EQ(elementMapText(cut->Shape.getShape()), seeded);
    EXPECT_NEAR(PartTestHelpers::getVolume(cut->Shape.getValue()), before.volume, 1e-4);
    EXPECT_EQ(cut->History.getSize(), 0);
}

TEST_F(DetachedKernelQualificationTest, booleanVenueCostsWideAndNarrow)
{
#if !PRC_M0_HAS_PROCESS_COST_PROBE
    GTEST_SKIP() << "Isolated-process cost probe needs Linux /proc and posix_spawn. "
                    "Qualification and harness tests still run.";
#else
    // Paid wall starts before capture and stops after the owner publication of
    // the named shape and the conditional material copy. One warmup and the
    // median of three repetitions, same as the Box venues. The thread execute
    // clock includes the raw Boolean, BRepCheck and makeElementShape. A
    // persistent worker is not implemented.
    setTrace(true);
    auto commitShape = [](Part::Boolean& feature, const Part::TopoShape& named) {
        publishBooleanShape(feature, named);
    };
    auto summarize = [&](const char* venue, const char* workload, const std::vector<PhaseSample>& samples) {
        std::vector<std::int64_t> capture;
        std::vector<std::int64_t> execute;
        std::vector<std::int64_t> transfer;
        std::vector<std::int64_t> commit;
        std::vector<std::int64_t> wall;
        for (const auto& sample : samples) {
            capture.push_back(sample.captureNs);
            execute.push_back(sample.executeNs);
            transfer.push_back(sample.transferNs);
            commit.push_back(sample.commitNs);
            wall.push_back(sample.wallNs);
        }
        const auto captureNs = medianOf(capture);
        const auto executeNs = medianOf(execute);
        const auto wallNs = medianOf(wall);
        const double share = wallNs == 0 ? 0.0 : static_cast<double>(executeNs) / static_cast<double>(wallNs);
        std::ostringstream shareText;
        shareText << std::fixed << std::setprecision(3) << share;
        appendCost(std::string("summary venue=") + venue + " workload=" + workload
                   + " warmup=" + std::to_string(kWarmup) + " reps=" + std::to_string(kRepetitions)
                   + " capture_ms=" + milliseconds(captureNs) + " execute_ms=" + milliseconds(executeNs)
                   + " transfer_ms=" + milliseconds(medianOf(transfer))
                   + " commit_ms=" + milliseconds(medianOf(commit)) + " wall_ms=" + milliseconds(wallNs)
                   + " paid_ms=" + milliseconds(wallNs) + " execute_share=" + shareText.str());
        return wallNs;
    };

    auto narrow = prepareBoolean<Part::Cut>(*_document, "N");
    ASSERT_NE(narrow.feature, nullptr);
    _document->recompute();
    ASSERT_FALSE(narrow.feature->isError());

    std::vector<PhaseSample> ownerNarrow;
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        narrow.feature->touch();
        _document->recompute();
    }
    for (int rep = 0; rep < kRepetitions; ++rep) {
        narrow.feature->touch();
        const auto wallStart = std::chrono::steady_clock::now();
        _document->recompute();
        const auto wallNs = elapsedNs(wallStart);
        const auto record = _document->lastRecomputeTrace();
        ASSERT_NE(record, nullptr);
        expectNoTransfer(*record);
        ownerNarrow.push_back({phaseSum(*record, App::RecomputePhase::Capture),
                               phaseSum(*record, App::RecomputePhase::Execute),
                               0,
                               phaseSum(*record, App::RecomputePhase::Commit),
                               wallNs});
    }

    const auto narrowSerial = snapshotBoolean(*narrow.feature);
    std::vector<PhaseSample> threadNarrow;
    for (int pass = 0; pass < kWarmup + kRepetitions; ++pass) {
        BooleanKernelResult kernel;
        std::int64_t captureNs = 0;
        std::int64_t executeNs = 0;
        std::int64_t commitNs = 0;
        const auto wallStart = std::chrono::steady_clock::now();
        const auto frozen = freezeBoolean(*narrow.feature, BooleanOp::Cut);
        captureNs = elapsedNs(wallStart);
        std::thread worker([&frozen, &kernel, &executeNs] {
            const auto started = std::chrono::steady_clock::now();
            kernel = executeBooleanKernel(
                BooleanOp::Cut,
                frozen.base.shape,
                frozen.tool.shape,
                frozen.base.label,
                frozen.tool.label);
            completeBooleanComputation(kernel, frozen);
            executeNs = elapsedNs(started);
        });
        worker.join();
        const auto commitStart = std::chrono::steady_clock::now();
        if (kernel.succeeded && !kernel.validationRejected && !kernel.named.isNull()) {
            commitShape(*narrow.feature, kernel.named);
        }
        commitNs = elapsedNs(commitStart);
        const auto wallNs = elapsedNs(wallStart);
        if (pass >= kWarmup) {
            ASSERT_TRUE(kernel.succeeded) << kernel.diagnostic;
            ASSERT_FALSE(kernel.validationRejected);
            ASSERT_FALSE(kernel.named.isNull());
            ASSERT_TRUE(harnessBooleanMatches(
                kernel.named.getShape(), elementMapText(kernel.named), narrowSerial));
            ASSERT_TRUE(publishedBooleanMatches(*narrow.feature, narrowSerial));
            threadNarrow.push_back({captureNs, executeNs, 0, commitNs, wallNs});
        }
    }

    auto* processDoc = makeControl("QualBoolCost");
    ASSERT_NE(processDoc, nullptr);
    auto process = prepareBoolean<Part::Cut>(*processDoc, "P");
    ASSERT_NE(process.feature, nullptr);
    processDoc->recompute();
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        process.feature->touch();
        const auto snapshot = waitIsolated(*processDoc, *process.feature);
        ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    }
    std::vector<PhaseSample> processNarrow;
    std::vector<ProcessAccount> processAccounts;
    for (int rep = 0; rep < kRepetitions; ++rep) {
        process.feature->touch();
        const auto watchState = prepareIsolatedWatch();
        std::atomic_bool stop {false};
        ProcessMarks marks;
        const auto wallStart = std::chrono::steady_clock::now();
        std::thread watcher([&] { marks = watchIsolatedProcess(wallStart, stop, watchState); });
        const auto snapshot = waitIsolated(*processDoc, *process.feature);
        const auto wallNs = elapsedNs(wallStart);
        stop.store(true, std::memory_order_release);
        watcher.join();
        ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
        const auto record = processDoc->lastRecomputeTrace();
        ASSERT_NE(record, nullptr);
        PhaseSample sample {phaseSum(*record, App::RecomputePhase::Capture),
                            phaseSum(*record, App::RecomputePhase::Execute),
                            phaseSum(*record, App::RecomputePhase::Transfer),
                            phaseSum(*record, App::RecomputePhase::Commit),
                            wallNs};
        ASSERT_TRUE(processBooleanMatches(*process.feature, snapshot, narrowSerial));
        processNarrow.push_back(sample);
        processAccounts.push_back(accountProcess(marks, sample));
    }

    auto* wideDoc = makeControl("QualBoolWide");
    ASSERT_NE(wideDoc, nullptr);
    std::vector<Part::Boolean*> wide;
    for (int index = 0; index < kWideBooleans; ++index) {
        const auto prepared = prepareBoolean<Part::Cut>(*wideDoc, ("W" + std::to_string(index)).c_str());
        ASSERT_NE(prepared.feature, nullptr);
        wide.push_back(prepared.feature);
    }
    wideDoc->recompute();
    std::vector<PhaseSample> ownerWide;
    std::size_t wideWidth = 0;
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        for (auto* feature : wide) {
            feature->touch();
        }
        wideDoc->recompute();
    }
    for (int rep = 0; rep < kRepetitions; ++rep) {
        for (auto* feature : wide) {
            feature->touch();
        }
        const auto wallStart = std::chrono::steady_clock::now();
        wideDoc->recompute();
        const auto wallNs = elapsedNs(wallStart);
        const auto record = wideDoc->lastRecomputeTrace();
        ASSERT_NE(record, nullptr);
        wideWidth = record->dag.maxWidth;
        ownerWide.push_back({phaseSum(*record, App::RecomputePhase::Capture),
                             phaseSum(*record, App::RecomputePhase::Execute),
                             0,
                             phaseSum(*record, App::RecomputePhase::Commit),
                             wallNs});
    }
    std::vector<SerialBoolean> wideSerial;
    wideSerial.reserve(wide.size());
    for (auto* feature : wide) {
        wideSerial.push_back(snapshotBoolean(*feature));
    }
    std::vector<PhaseSample> threadWide;
    for (int pass = 0; pass < kWarmup + kRepetitions; ++pass) {
        std::vector<FrozenBoolean> frozen(wide.size());
        std::vector<BooleanKernelResult> kernels(wide.size());
        std::int64_t captureNs = 0;
        std::int64_t executeNs = 0;
        std::int64_t commitNs = 0;
        const auto wallStart = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < wide.size(); ++index) {
            frozen[index] = freezeBoolean(*wide[index], BooleanOp::Cut);
        }
        captureNs = elapsedNs(wallStart);
        std::thread worker([&] {
            const auto started = std::chrono::steady_clock::now();
            for (std::size_t index = 0; index < frozen.size(); ++index) {
                kernels[index] = executeBooleanKernel(
                    BooleanOp::Cut,
                    frozen[index].base.shape,
                    frozen[index].tool.shape,
                    frozen[index].base.label,
                    frozen[index].tool.label);
                completeBooleanComputation(kernels[index], frozen[index]);
            }
            executeNs = elapsedNs(started);
        });
        worker.join();
        const auto commitStart = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < kernels.size(); ++index) {
            if (kernels[index].succeeded && !kernels[index].validationRejected
                && !kernels[index].named.isNull()) {
                commitShape(*wide[index], kernels[index].named);
            }
        }
        commitNs = elapsedNs(commitStart);
        const auto wallNs = elapsedNs(wallStart);
        if (pass >= kWarmup) {
            ASSERT_EQ(kernels.size(), wideSerial.size());
            for (std::size_t index = 0; index < kernels.size(); ++index) {
                ASSERT_TRUE(kernels[index].succeeded) << kernels[index].diagnostic;
                ASSERT_FALSE(kernels[index].validationRejected);
                ASSERT_FALSE(kernels[index].named.isNull());
                ASSERT_TRUE(harnessBooleanMatches(kernels[index].named.getShape(),
                                                  elementMapText(kernels[index].named),
                                                  wideSerial[index]))
                    << index;
                ASSERT_TRUE(publishedBooleanMatches(*wide[index], wideSerial[index])) << index;
            }
            threadWide.push_back({captureNs, executeNs, 0, commitNs, wallNs});
        }
    }

    auto* chainDoc = makeControl("QualBoolChain");
    ASSERT_NE(chainDoc, nullptr);
    auto* root = addBox(*chainDoc, "ChainRoot", 80, 30, 20);
    ASSERT_NE(root, nullptr);
    std::vector<Part::Box*> chainTools;
    std::vector<Part::Cut*> chain;
    Part::Feature* previous = root;
    for (int index = 0; index < kChainSteps; ++index) {
        auto* tool = addBox(*chainDoc, ("ChainTool" + std::to_string(index)).c_str(), 6, 6, 40);
        ASSERT_NE(tool, nullptr);
        tool->Placement.setValue(
            Base::Placement(Base::Vector3d(8.0 * index, 8, -5), Base::Rotation()));
        auto* cut = addBoolean<Part::Cut>(*chainDoc, ("ChainCut" + std::to_string(index)).c_str());
        ASSERT_NE(cut, nullptr);
        cut->Base.setValue(previous);
        cut->Tool.setValue(tool);
        cut->Refine.setValue(false);
        chainTools.push_back(tool);
        chain.push_back(cut);
        previous = cut;
    }
    chainDoc->recompute();
    ASSERT_FALSE(chain.back()->isError()) << chain.back()->getStatusString();
    std::vector<PhaseSample> ownerChain;
    std::size_t chainPath = 0;
    std::size_t chainWidth = 0;
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        for (auto* cut : chain) {
            cut->touch();
        }
        chainDoc->recompute();
    }
    for (int rep = 0; rep < kRepetitions; ++rep) {
        for (auto* cut : chain) {
            cut->touch();
        }
        const auto wallStart = std::chrono::steady_clock::now();
        chainDoc->recompute();
        const auto wallNs = elapsedNs(wallStart);
        const auto record = chainDoc->lastRecomputeTrace();
        ASSERT_NE(record, nullptr);
        chainPath = record->dag.longestPath;
        chainWidth = record->dag.maxWidth;
        ownerChain.push_back({phaseSum(*record, App::RecomputePhase::Capture),
                              phaseSum(*record, App::RecomputePhase::Execute),
                              0,
                              phaseSum(*record, App::RecomputePhase::Commit),
                              wallNs});
    }
    std::vector<SerialBoolean> chainSerial;
    chainSerial.reserve(chain.size());
    for (auto* cut : chain) {
        chainSerial.push_back(snapshotBoolean(*cut));
    }
    std::vector<PhaseSample> threadChain;
    const bool chainCheckModel = booleanPreferences()->GetBool("CheckModel", true);
    const double chainFuzzy = Part::FuzzyHelper::getBooleanFuzzy();
    const double chainConfusion = Precision::Confusion();
    const auto chainOptions = Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform;
    for (int pass = 0; pass < kWarmup + kRepetitions; ++pass) {
        Part::TopoShape currentNamed;
        std::vector<Part::TopoShape> tools;
        std::vector<BooleanKernelResult> kernels(chain.size());
        std::int64_t captureNs = 0;
        std::int64_t executeNs = 0;
        std::int64_t commitNs = 0;
        const auto wallStart = std::chrono::steady_clock::now();
        currentNamed = namedCopy(Part::Feature::getTopoShape(root, chainOptions));
        for (auto* tool : chainTools) {
            tools.push_back(namedCopy(Part::Feature::getTopoShape(tool, chainOptions)));
        }
        captureNs = elapsedNs(wallStart);
        // The serial chain reads each previous Cut through getTopoShape after
        // Shape.setValue. That publication hashes the child map. Feeding the raw
        // kernel into the next step drops one element postfix, so the result
        // would not match serial. Publish on the owner between steps, and run
        // each kernel on its own std::thread from that published input.
        std::vector<std::vector<std::string>> kernelMaps(chain.size());
        for (std::size_t index = 0; index < tools.size(); ++index) {
            FrozenBoolean step;
            step.kind = BooleanOp::Cut;
            step.checkModel = chainCheckModel;
            step.fuzzy = chainFuzzy;
            step.confusion = chainConfusion;
            step.base.named = currentNamed;
            step.base.shape = currentNamed.getShape();
            step.base.label = "base";
            step.tool.named = tools[index];
            step.tool.shape = tools[index].getShape();
            step.tool.label = "tool";
            BooleanKernelResult kernel;
            std::int64_t stepExecuteNs = 0;
            std::thread worker([&] {
                const auto started = std::chrono::steady_clock::now();
                kernel = executeBooleanKernel(
                    BooleanOp::Cut, step.base.shape, step.tool.shape, step.base.label, step.tool.label);
                completeBooleanComputation(kernel, step);
                stepExecuteNs = elapsedNs(started);
            });
            worker.join();
            executeNs += stepExecuteNs;
            if (kernel.succeeded && !kernel.named.isNull()) {
                kernelMaps[index] = elementMapText(kernel.named);
            }
            kernels[index] = std::move(kernel);
            const auto commitStart = std::chrono::steady_clock::now();
            if (kernels[index].succeeded && !kernels[index].validationRejected
                && !kernels[index].named.isNull()) {
                commitShape(*chain[index], kernels[index].named);
                currentNamed = namedCopy(Part::Feature::getTopoShape(chain[index], chainOptions));
            }
            commitNs += elapsedNs(commitStart);
        }
        const auto wallNs = elapsedNs(wallStart);
        if (pass >= kWarmup) {
            ASSERT_EQ(kernels.size(), chainSerial.size());
            for (std::size_t index = 0; index < kernels.size(); ++index) {
                ASSERT_TRUE(kernels[index].succeeded) << kernels[index].diagnostic;
                ASSERT_FALSE(kernels[index].validationRejected);
                ASSERT_FALSE(kernels[index].named.isNull());
                ASSERT_TRUE(harnessBooleanMatches(kernels[index].named.getShape(),
                                                  kernelMaps[index],
                                                  chainSerial[index]))
                    << index;
                ASSERT_TRUE(publishedBooleanMatches(*chain[index], chainSerial[index])) << index;
            }
            threadChain.push_back({captureNs, executeNs, 0, commitNs, wallNs});
        }
    }

    auto* heavyDoc = makeControl("QualBoolHeavy");
    ASSERT_NE(heavyDoc, nullptr);
    auto* heavyBase = heavyDoc->addObject<Part::Feature>("HeavyBase");
    auto* heavyTool = heavyDoc->addObject<Part::Feature>("HeavyTool");
    auto* heavyCut = addBoolean<Part::Cut>(*heavyDoc, "HeavyCut");
    ASSERT_NE(heavyBase, nullptr);
    ASSERT_NE(heavyTool, nullptr);
    ASSERT_NE(heavyCut, nullptr);
    heavyBase->Shape.setValue(prismSolid(kHeavySides, 20, 15, 0));
    heavyTool->Shape.setValue(prismSolid(kHeavySides, 20, 15, 6));
    heavyCut->Base.setValue(heavyBase);
    heavyCut->Tool.setValue(heavyTool);
    heavyDoc->recompute();
    ASSERT_FALSE(heavyCut->isError()) << heavyCut->getStatusString();
    const int heavyFaces = countShapes(heavyBase->Shape.getValue(), TopAbs_FACE);
    appendCost("heavy_operand_faces=" + std::to_string(heavyFaces));
    std::vector<PhaseSample> ownerHeavy;
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        heavyCut->touch();
        heavyDoc->recompute();
    }
    for (int rep = 0; rep < kRepetitions; ++rep) {
        heavyCut->touch();
        const auto wallStart = std::chrono::steady_clock::now();
        heavyDoc->recompute();
        const auto wallNs = elapsedNs(wallStart);
        const auto record = heavyDoc->lastRecomputeTrace();
        ASSERT_NE(record, nullptr);
        ownerHeavy.push_back({phaseSum(*record, App::RecomputePhase::Capture),
                              phaseSum(*record, App::RecomputePhase::Execute),
                              0,
                              phaseSum(*record, App::RecomputePhase::Commit),
                              wallNs});
    }
    const auto heavySerial = snapshotBoolean(*heavyCut);
    std::vector<PhaseSample> threadHeavy;
    for (int pass = 0; pass < kWarmup + kRepetitions; ++pass) {
        BooleanKernelResult kernel;
        std::int64_t captureNs = 0;
        std::int64_t executeNs = 0;
        std::int64_t commitNs = 0;
        const auto wallStart = std::chrono::steady_clock::now();
        const auto frozen = freezeBoolean(*heavyCut, BooleanOp::Cut);
        captureNs = elapsedNs(wallStart);
        std::thread worker([&frozen, &kernel, &executeNs] {
            const auto started = std::chrono::steady_clock::now();
            kernel = executeBooleanKernel(
                BooleanOp::Cut,
                frozen.base.shape,
                frozen.tool.shape,
                frozen.base.label,
                frozen.tool.label);
            completeBooleanComputation(kernel, frozen);
            executeNs = elapsedNs(started);
        });
        worker.join();
        const auto commitStart = std::chrono::steady_clock::now();
        if (kernel.succeeded && !kernel.validationRejected && !kernel.named.isNull()) {
            commitShape(*heavyCut, kernel.named);
        }
        commitNs = elapsedNs(commitStart);
        const auto wallNs = elapsedNs(wallStart);
        if (pass >= kWarmup) {
            ASSERT_TRUE(kernel.succeeded) << kernel.diagnostic;
            ASSERT_FALSE(kernel.validationRejected);
            ASSERT_FALSE(kernel.named.isNull());
            ASSERT_TRUE(harnessBooleanMatches(
                kernel.named.getShape(), elementMapText(kernel.named), heavySerial));
            ASSERT_TRUE(publishedBooleanMatches(*heavyCut, heavySerial));
            threadHeavy.push_back({captureNs, executeNs, 0, commitNs, wallNs});
        }
    }
    std::vector<PhaseSample> processHeavy;
    for (int warmup = 0; warmup < kWarmup; ++warmup) {
        heavyCut->touch();
        const auto snapshot = waitIsolated(*heavyDoc, *heavyCut);
        ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    }
    for (int rep = 0; rep < kRepetitions; ++rep) {
        heavyCut->touch();
        const auto wallStart = std::chrono::steady_clock::now();
        const auto snapshot = waitIsolated(*heavyDoc, *heavyCut);
        const auto wallNs = elapsedNs(wallStart);
        ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
        const auto record = heavyDoc->lastRecomputeTrace();
        ASSERT_NE(record, nullptr);
        ASSERT_TRUE(processBooleanMatches(*heavyCut, snapshot, heavySerial));
        processHeavy.push_back({phaseSum(*record, App::RecomputePhase::Capture),
                                phaseSum(*record, App::RecomputePhase::Execute),
                                phaseSum(*record, App::RecomputePhase::Transfer),
                                phaseSum(*record, App::RecomputePhase::Commit),
                                wallNs});
    }
    setTrace(false);

    ASSERT_EQ(threadNarrow.size(), static_cast<std::size_t>(kRepetitions));
    ASSERT_EQ(processNarrow.size(), static_cast<std::size_t>(kRepetitions));
    ASSERT_EQ(threadWide.size(), static_cast<std::size_t>(kRepetitions));
    ASSERT_EQ(threadChain.size(), static_cast<std::size_t>(kRepetitions));
    ASSERT_EQ(threadHeavy.size(), static_cast<std::size_t>(kRepetitions));
    ASSERT_EQ(processHeavy.size(), static_cast<std::size_t>(kRepetitions));
    EXPECT_FALSE(narrow.feature->isError());
    EXPECT_GT(PartTestHelpers::getVolume(narrow.feature->Shape.getValue()), 0.0);
    EXPECT_EQ(narrow.feature->History.getSize(), 0);
    EXPECT_FALSE(process.feature->isError());
    EXPECT_EQ(wideWidth, static_cast<std::size_t>(kWideBooleans));
    EXPECT_GE(chainPath, static_cast<std::size_t>(kChainSteps - 1));
    EXPECT_GT(PartTestHelpers::getVolume(chain.back()->Shape.getValue()), 0.0);
    EXPECT_LT(PartTestHelpers::getVolume(chain.back()->Shape.getValue()), 48000.0);
    EXPECT_GT(heavyFaces, 6);
    EXPECT_FALSE(heavyCut->isError());

    const auto ownerNarrowWall = summarize("owner", "cut:1", ownerNarrow);
    const auto threadNarrowWall = summarize("thread", "cut:1", threadNarrow);
    const auto processNarrowWall = summarize("process", "cut:1", processNarrow);
    const auto ownerWideWall = summarize("owner", "cut:16", ownerWide);
    const auto threadWideWall = summarize("thread", "cut:16", threadWide);
    const auto ownerChainWall = summarize("owner", "chain:8", ownerChain);
    const auto threadChainWall = summarize("thread", "chain:8", threadChain);
    const auto ownerHeavyWall = summarize("owner", "heavy:1", ownerHeavy);
    const auto threadHeavyWall = summarize("thread", "heavy:1", threadHeavy);
    const auto processHeavyWall = summarize("process", "heavy:1", processHeavy);
    appendCost(std::string("dag wide_width=") + std::to_string(wideWidth)
               + " chain_width=" + std::to_string(chainWidth)
               + " chain_longest_path=" + std::to_string(chainPath));

    ASSERT_EQ(processAccounts.size(), processNarrow.size());
    std::vector<std::size_t> order(processNarrow.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return processNarrow[left].wallNs < processNarrow[right].wallNs;
    });
    const auto& paid = processAccounts[order[order.size() / 2]];
    const auto persistentNs = paid.captureNs + paid.archiveNs + paid.executeNs + paid.decodeNs
        + paid.commitNs + paid.settleNs;
    appendCost(std::string("process-breakdown workload=cut:1")
               + " capture_ms=" + milliseconds(paid.captureNs)
               + " parent_fingerprint_ms=" + milliseconds(paid.parentFingerprintNs)
               + " archive_ms=" + milliseconds(paid.archiveNs)
               + " spawn_ms=" + milliseconds(paid.spawnNs)
               + " worker_startup_ms=" + milliseconds(paid.workerStartupNs)
               + " import_ms=" + milliseconds(paid.importNs)
               + " execute_ms=" + milliseconds(paid.executeNs)
               + " teardown_ms=" + milliseconds(paid.teardownNs)
               + " decode_ms=" + milliseconds(paid.decodeNs)
               + " commit_ms=" + milliseconds(paid.commitNs)
               + " settle_ms=" + milliseconds(paid.settleNs)
               + " exit_to_published_ms=" + milliseconds(paid.republishNs)
               + " published_to_return_ms=" + milliseconds(paid.afterPublishNs)
               + " settle_cause=unresolved"
               + " wall_ms=" + milliseconds(paid.wallNs)
               + " persistent_cached_fingerprint_ms=" + milliseconds(persistentNs));
    appendCost(std::string("crossover thread_vs_owner_cut1=")
               + (threadNarrowWall < ownerNarrowWall ? "thread" : "owner")
               + " thread_vs_owner_cut16=" + (threadWideWall < ownerWideWall ? "thread" : "owner")
               + " thread_vs_owner_chain8=" + (threadChainWall < ownerChainWall ? "thread" : "owner")
               + " thread_vs_owner_heavy1=" + (threadHeavyWall < ownerHeavyWall ? "thread" : "owner")
               + " process_cut1_ms=" + milliseconds(processNarrowWall)
               + " process_heavy1_ms=" + milliseconds(processHeavyWall)
               + " persistent_cut1_ms=" + milliseconds(persistentNs));
    // M0.2a box:1 fixed costs a warm worker would not repeat: parent fingerprint,
    // worker startup, Part import/restore, teardown, and the per-job spawn.
    constexpr double kM02aAmortizedMs = 598.573 + 662.540 + 161.763 + 60.468 + 65.339;
    appendCost(std::string("m0.2a_amortized_fixed_ms=") + std::to_string(kM02aAmortizedMs));
#endif
}

namespace
{

FrozenOperand privateOperand(const FrozenOperand& source, long tag)
{
    FrozenOperand copy;
    copy.label = source.label;
    Part::TopoShape named;
    named.Hasher = nullptr;
    named.setShape(deepCopyShape(source.shape), /*resetElementMap=*/true);
    named.Tag = tag;
    named.resetElementMap(seededElementMap());
    copy.named = std::move(named);
    copy.shape = copy.named.getShape();
    return copy;
}

struct ConcurrentKernelInputs
{
    FrozenBoxInputs box;
    BooleanOp kind {BooleanOp::Cut};
    bool checkModel {true};
    double fuzzy {1};
    double confusion {0};
    FrozenOperand base;
    FrozenOperand tool;
};

void runConcurrentKernels(const ConcurrentKernelInputs& inputs,
                          std::atomic<bool>& go,
                          std::atomic<bool>& stop,
                          std::atomic<int>& ready,
                          std::atomic<int>& active,
                          std::atomic<int>& peak,
                          std::atomic<int>& boxOk,
                          std::atomic<int>& booleanOk,
                          std::atomic<int>& failures)
{
    ready.fetch_add(1, std::memory_order_release);
    ready.notify_all();
    go.wait(false, std::memory_order_acquire);
    while (!stop.load(std::memory_order_acquire)) {
        const int now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
        int seen = peak.load(std::memory_order_relaxed);
        while (now > seen
               && !peak.compare_exchange_weak(
                   seen, now, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
        struct LeaveActive
        {
            std::atomic<int>& active;
            ~LeaveActive()
            {
                active.fetch_sub(1, std::memory_order_acq_rel);
            }
        } leave {active};

        try {
            const auto box = executeBoxKernel(inputs.box);
            if (box.succeeded && !box.shape.IsNull()) {
                boxOk.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                failures.fetch_add(1, std::memory_order_relaxed);
            }

            FrozenBoolean frozen;
            frozen.kind = inputs.kind;
            frozen.checkModel = inputs.checkModel;
            frozen.fuzzy = inputs.fuzzy;
            frozen.confusion = inputs.confusion;
            frozen.base = inputs.base;
            frozen.tool = inputs.tool;
            auto kernel = executeBooleanKernel(frozen.kind,
                                               frozen.base.shape,
                                               frozen.tool.shape,
                                               frozen.base.label,
                                               frozen.tool.label);
            if (kernel.succeeded && kernel.operation && !kernel.shape.IsNull()) {
                completeBooleanComputation(kernel, frozen);
                if (!kernel.validationRejected && !kernel.named.isNull()) {
                    booleanOk.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        catch (const std::exception&) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        catch (...) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace

TEST_F(DetachedKernelQualificationTest, concurrentFrozenKernelsWithOwnerRecompute)
{
    // M0.3a stress. N workers run the Box and Boolean kernels on private frozen
    // copies while the owner recomputes unrelated features. Production
    // scheduling is unchanged. No sleep is used to manufacture overlap.
    constexpr int kWorkers = 4;
    constexpr int kOwnerPasses = 4;
    const BooleanOp kinds[] = {BooleanOp::Cut, BooleanOp::Fuse, BooleanOp::Common};

    auto* source = addBox(*_document, "RaceSource");
    ASSERT_NE(source, nullptr);
    source->Placement.setValue(rotatedPlacement());
    auto raceCut = prepareBoolean<Part::Cut>(*_document, "Race");
    ASSERT_NE(raceCut.feature, nullptr);
    _document->recompute();
    ASSERT_FALSE(source->isError());
    ASSERT_FALSE(raceCut.feature->isError());

    const auto frozenBox = freezeBox(*source);
    const auto frozenCut = freezeBoolean(*raceCut.feature, BooleanOp::Cut);
    const auto liveShape = source->Shape.getValue();
    const auto liveBase = raceCut.base->Shape.getValue();
    const auto liveTool = raceCut.tool->Shape.getValue();
    const auto liveVolume = PartTestHelpers::getVolume(liveShape);
    const auto liveBaseIds = tshapeIds(liveBase);
    const auto liveToolIds = tshapeIds(liveTool);

    std::vector<Part::Box*> unrelated;
    unrelated.reserve(4);
    for (int index = 0; index < 4; ++index) {
        const std::string name = "Unrelated" + std::to_string(index);
        auto* box = addBox(*_document, name.c_str(), 12.0 + index, 8.0, 6.0);
        ASSERT_NE(box, nullptr);
        box->Placement.setValue(
            Base::Placement(Base::Vector3d(80.0 + (20.0 * index), 0, 0), Base::Rotation()));
        unrelated.push_back(box);
    }
    auto unrelatedCut = prepareBoolean<Part::Cut>(*_document, "Owner");
    ASSERT_NE(unrelatedCut.feature, nullptr);
    _document->recompute();
    ASSERT_FALSE(unrelatedCut.feature->isError());
    for (const auto* box : unrelated) {
        ASSERT_FALSE(box->isError());
    }

    std::vector<ConcurrentKernelInputs> inputs(static_cast<std::size_t>(kWorkers));
    for (int index = 0; index < kWorkers; ++index) {
        auto& slot = inputs[static_cast<std::size_t>(index)];
        slot.box = frozenBox;
        slot.kind = kinds[index % 3];
        slot.checkModel = frozenCut.checkModel;
        slot.fuzzy = frozenCut.fuzzy;
        slot.confusion = frozenCut.confusion;
        const long tag = 1000L + index;
        slot.base = privateOperand(frozenCut.base, tag);
        slot.tool = privateOperand(frozenCut.tool, tag + 100L);
        EXPECT_FALSE(anyPartner(slot.base.shape, liveBase));
        EXPECT_FALSE(anyPartner(slot.tool.shape, liveTool));
        EXPECT_FALSE(anyPartner(slot.base.shape, slot.tool.shape));
    }

    std::atomic<bool> go {false};
    std::atomic<bool> stop {false};
    std::atomic<int> ready {0};
    std::atomic<int> active {0};
    std::atomic<int> peak {0};
    std::atomic<int> boxOk {0};
    std::atomic<int> booleanOk {0};
    std::atomic<int> failures {0};
    std::atomic<int> ownerSawActive {0};

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(kWorkers));
    for (int index = 0; index < kWorkers; ++index) {
        workers.emplace_back([&, index] {
            runConcurrentKernels(inputs[static_cast<std::size_t>(index)],
                                 go,
                                 stop,
                                 ready,
                                 active,
                                 peak,
                                 boxOk,
                                 booleanOk,
                                 failures);
        });
    }

    int arrived = ready.load(std::memory_order_acquire);
    while (arrived < kWorkers) {
        ready.wait(arrived, std::memory_order_acquire);
        arrived = ready.load(std::memory_order_acquire);
    }
    go.store(true, std::memory_order_release);
    go.notify_all();

    int passes = 0;
    while (passes < kOwnerPasses || booleanOk.load(std::memory_order_acquire) < kWorkers) {
        unrelated[0]->Length.setValue(12.0 + passes);
        unrelated[0]->touch();
        unrelatedCut.base->touch();
        unrelatedCut.tool->touch();
        unrelatedCut.feature->touch();
        const bool overlapped = active.load(std::memory_order_acquire) > 0;
        _document->recompute();
        if (overlapped || active.load(std::memory_order_acquire) > 0) {
            ownerSawActive.fetch_add(1, std::memory_order_relaxed);
        }
        ++passes;
        if (passes > kOwnerPasses + 8) {
            break;
        }
    }

    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_GE(peak.load(), 2);
    EXPECT_GT(ownerSawActive.load(), 0);
    EXPECT_GE(boxOk.load(), kWorkers);
    EXPECT_GE(booleanOk.load(), kWorkers);
    EXPECT_EQ(failures.load(), 0);
    EXPECT_TRUE(source->Shape.getValue().IsSame(liveShape));
    EXPECT_TRUE(raceCut.base->Shape.getValue().IsSame(liveBase));
    EXPECT_TRUE(raceCut.tool->Shape.getValue().IsSame(liveTool));
    EXPECT_EQ(tshapeIds(raceCut.base->Shape.getValue()), liveBaseIds);
    EXPECT_EQ(tshapeIds(raceCut.tool->Shape.getValue()), liveToolIds);
    EXPECT_NEAR(PartTestHelpers::getVolume(source->Shape.getValue()), liveVolume, 1e-6);
    EXPECT_FALSE(source->isError());
    EXPECT_FALSE(raceCut.feature->isError());
    EXPECT_FALSE(unrelatedCut.feature->isError());
    for (const auto* box : unrelated) {
        EXPECT_FALSE(box->isError());
    }
}
