// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DetachedKernelQualification.h"

#include <gtest/gtest.h>

#include <src/App/InitApplication.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/GeometryWorkerMain.h>
#include <App/DocumentObject.h>
#include <App/ElementMap.h>
#include <App/Expression.h>
#include <App/GenericIsolatedRecompute.h>
#include <App/GeoFeature.h>
#include <App/GeoFeatureGroupExtension.h>
#include <App/Link.h>
#include <App/ObjectIdentifier.h>
#include <App/Part.h>
#include <App/RecomputeHandle.h>
#include <App/RecomputePhaseTrace.h>
#include <Base/Interpreter.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <Mod/Part/App/FeaturePartCut.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/PrismExtension.h>
#include <Mod/Part/App/PropertyTopoShape.h>

#include "PartTestHelpers.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
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
#include <fstream>
#include <iomanip>
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

#if PRC_M0_HAS_PROCESS_COST_PROBE
# include <fcntl.h>
# include <spawn.h>
# include <sys/wait.h>
extern char** environ;
#endif

using PartQualification::AdmissionRefusal;
using PartQualification::EffectClass;
using PartQualification::QualificationField;
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

void installSeededMap(Part::Box& box)
{
    Part::TopoShape shape = box.Shape.getShape();
    shape.Tag = box.getID();
    shape.resetElementMap(seededElementMap());
    RecomputeStatus status(box);
    box.Shape.setValue(shape);
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
        if (marks.pid < 0) {
            for (const auto pid : freeCadCmdPids()) {
                if (preexistingPids.contains(pid)) {
                    continue;
                }
                marks.pid = pid;
                marks.spawnNs = now;
                break;
            }
        }
        else if (marks.exitNs < 0
                 && !std::filesystem::exists("/proc/" + std::to_string(marks.pid))) {
            marks.exitNs = now;
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

TEST_F(DetachedKernelQualificationTest, preferenceReadsDuringExecuteAreNotAdmitted)
{
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

    const auto decision =
        expectRefused(*cut, AdmissionRefusal::PreferenceReadDuringExecute, controlCut);
    EXPECT_TRUE(decision.has(AdmissionRefusal::Link));
    EXPECT_TRUE(legacyIsolatedPlanDetaches(*_document, *cut));
    EXPECT_FALSE(capturePostcondition(*cut).error);
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
