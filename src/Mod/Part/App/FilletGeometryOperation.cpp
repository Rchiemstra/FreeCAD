// SPDX-License-Identifier: LGPL-2.1-or-later

#include "FilletGeometryOperation.h"
#include <App/GeometryRequestWorkspace.h>
#include <Mod/Part/App/TopoShapeOpCode.h>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS.hxx>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Standard_Failure.hxx>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryFile>

#include <cmath>
#include <limits>
#include <memory>
#include <unordered_set>

namespace Part
{

namespace
{

TopoShape materializeBase(const FrozenTopoShapeBundle& bundle)
{
    TopoShape shape = bundle.shape;
    const long tag = bundle.shapeTag != 0 ? bundle.shapeTag : (shape.Tag != 0 ? shape.Tag : 1);
    shape.Tag = tag;
    if (bundle.hasher) {
        shape.Hasher = bundle.hasher;
    }
    if (bundle.elementMap) {
        shape.resetElementMap(bundle.elementMap);
    }
    return shape;
}

bool requireString(const QJsonObject& request,
                   const char* key,
                   QString& out,
                   std::string& errorCode,
                   std::string& errorMessage)
{
    const QJsonValue v = request.value(QString::fromLatin1(key));
    if (!v.isString()) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be a JSON string";
        return false;
    }
    out = v.toString();
    return true;
}

bool requireJsonInt64(const QJsonObject& obj,
                      const char* key,
                      qint64& out,
                      std::string& errorCode,
                      std::string& errorMessage,
                      qint64 maxInclusive = std::numeric_limits<qint64>::max())
{
    if (!obj.contains(QString::fromLatin1(key))) {
        errorCode = "MissingJsonField";
        errorMessage = std::string(key) + " is required";
        return false;
    }
    const QJsonValue v = obj.value(QString::fromLatin1(key));
    if (!v.isDouble()) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be a JSON number";
        return false;
    }
    const double d = v.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be an integer";
        return false;
    }
    constexpr double kInt64ExclusiveMax = 9223372036854775808.0;
    constexpr double kInt64Min = -9223372036854775808.0;
    if (d < kInt64Min || d >= kInt64ExclusiveMax) {
        errorCode = "OutOfRangeJsonNumber";
        errorMessage = std::string(key) + " is out of int64 range";
        return false;
    }
    if (d > static_cast<double>(maxInclusive)) {
        errorCode = "OutOfRangeJsonNumber";
        errorMessage = std::string(key) + " exceeds the trusted maximum";
        return false;
    }
    out = static_cast<qint64>(d);
    if (static_cast<double>(out) != d) {
        errorCode = "OutOfRangeJsonNumber";
        errorMessage = std::string(key) + " cannot be represented exactly as int64";
        return false;
    }
    return true;
}

bool requirePositiveFiniteDouble(const QJsonObject& obj,
                                 const char* key,
                                 double& out,
                                 std::string& errorCode,
                                 std::string& errorMessage)
{
    if (!obj.contains(QString::fromLatin1(key))) {
        errorCode = "MissingJsonField";
        errorMessage = std::string(key) + " is required";
        return false;
    }
    const QJsonValue v = obj.value(QString::fromLatin1(key));
    if (!v.isDouble()) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be a JSON number";
        return false;
    }
    const double d = v.toDouble();
    if (!std::isfinite(d) || d <= 0.0) {
        errorCode = "InvalidRadius";
        errorMessage = std::string(key) + " must be a finite positive number";
        return false;
    }
    out = d;
    return true;
}

bool requireEdgeIndex(const QJsonObject& edgeObj,
                      uint32_t& out,
                      size_t edgeCount,
                      std::string& errorCode,
                      std::string& errorMessage)
{
    qint64 raw = 0;
    const qint64 maxU32 = static_cast<qint64>(std::numeric_limits<uint32_t>::max());
    if (!requireJsonInt64(edgeObj, "edgeIndex", raw, errorCode, errorMessage, maxU32)) {
        return false;
    }
    if (raw < 0) {
        errorCode = "InvalidEdgeIndex";
        errorMessage = "edgeIndex is out of range";
        return false;
    }
    const uint32_t edgeIndex = static_cast<uint32_t>(raw);
    if (static_cast<qint64>(edgeIndex) != raw) {
        errorCode = "OutOfRangeJsonNumber";
        errorMessage = "edgeIndex exceeds uint32 range";
        return false;
    }
    if (edgeIndex >= edgeCount) {
        errorCode = "InvalidEdgeIndex";
        errorMessage = "edgeIndex does not reference an edge in the base shape";
        return false;
    }
    out = edgeIndex;
    return true;
}

size_t countShapeEdges(const TopoShape& shape)
{
    size_t count = 0;
    for (TopExp_Explorer exp(shape.getShape(), TopAbs_EDGE); exp.More(); exp.Next()) {
        ++count;
    }
    return count;
}

} // namespace

FilletGeometryOperation::FilletGeometryOperation() = default;

FilletGeometryOperation::FilletGeometryOperation(const FrozenTopoShapeBundle& base, const std::vector<FilletEdgeSpec>& edges)
    : _base(base), _edges(edges)
{
}

FilletGeometryOperation::~FilletGeometryOperation() = default;

std::string FilletGeometryOperation::parameterDigest() const
{
    std::string digest = "fillet|" + TopoShapeArchive::fingerprintBundle(_base);
    for (const auto& edge : _edges) {
        digest.push_back('|');
        digest += std::to_string(edge.edgeIndex);
        digest.push_back(':');
        digest += std::to_string(edge.startRadius);
        digest.push_back(':');
        digest += std::to_string(edge.endRadius);
    }
    return digest;
}

App::GeometryOperationTraits FilletGeometryOperation::traits() const
{
    App::GeometryOperationTraits t;
    t.supportsInProcess = false; // Process-first for fillet
    t.supportsCooperativeCancel = true;
    return t;
}

App::DetachedGeometryResult FilletGeometryOperation::run(App::GeometryWorkerContext& ctx) const
{
    App::DetachedGeometryResult result;
    ctx.reportProgress(0.1, "fillet.prepare");

    if (ctx.isCancelled()) {
        result.success = false;
        result.errorCode = "Cancelled";
        result.errorMessage = "Cancelled before fillet start";
        return result;
    }

    TopoShape base = materializeBase(_base);
    if (base.isNull()) {
        result.success = false;
        result.errorCode = "NullShape";
        result.errorMessage = "Base shape for fillet is null";
        return result;
    }

    if (!base.Hasher) {
        base.Hasher = App::StringHasherRef(new App::StringHasher);
    }

    try {
        BRepFilletAPI_MakeFillet mkFillet(base.getShape());
        std::vector<TopoDS_Edge> allEdges;
        for (TopExp_Explorer exp(base.getShape(), TopAbs_EDGE); exp.More(); exp.Next()) {
            allEdges.push_back(TopoDS::Edge(exp.Current()));
        }

        size_t added = 0;
        for (const auto& spec : _edges) {
            if (spec.edgeIndex < allEdges.size()) {
                mkFillet.Add(spec.startRadius, spec.endRadius, allEdges[spec.edgeIndex]);
                ++added;
            }
        }
        if (added == 0) {
            result.success = false;
            result.errorCode = "NoEdges";
            result.errorMessage = "No valid fillet edge indices were provided";
            return result;
        }

        ctx.reportProgress(0.5, "fillet.compute");
        mkFillet.Build();

        if (ctx.isCancelled()) {
            result.success = false;
            result.errorCode = "Cancelled";
            result.errorMessage = "Cancelled during fillet compute";
            return result;
        }

        if (!mkFillet.IsDone()) {
            result.success = false;
            result.errorCode = "FilletBuildError";
            result.errorMessage = "BRepFilletAPI_MakeFillet failed to build shape";
            return result;
        }

        TopoShape outShape(0, base.Hasher);
        outShape.makeElementShape(mkFillet, base, OpCodes::Fillet);
        if (outShape.isNull()) {
            result.success = false;
            result.errorCode = "NullResult";
            result.errorMessage = "Mapped fillet produced a null shape";
            return result;
        }

        ctx.reportProgress(0.8, "fillet.archive");
        FrozenTopoShapeBundle outBundle = TopoShapeArchive::createBundle(outShape);
        if (!outBundle.valid) {
            result.success = false;
            result.errorCode = outBundle.errorCode.empty() ? "BundleInvalid" : outBundle.errorCode;
            result.errorMessage = "Failed to freeze mapped fillet result";
            return result;
        }

        std::string resultPath = ctx.tempDir() + "/result.fcg";
        if (TopoShapeArchive::writeArchive(outBundle, resultPath)) {
            result.success = true;
            result.resultArchivePath = resultPath;
        }
        else {
            result.success = false;
            result.errorCode = "ArchiveError";
            result.errorMessage = "Failed to write fillet output archive";
        }
    }
    catch (const Base::Exception& e) {
        result.success = false;
        result.errorCode = "FilletError";
        result.errorMessage = e.what();
    }
    catch (const Standard_Failure& e) {
        result.success = false;
        result.errorCode = "OCCError";
        result.errorMessage = e.GetMessageString() ? e.GetMessageString()
                                                   : "OCC Exception during fillet operation";
    }
    catch (const std::exception& e) {
        result.success = false;
        result.errorCode = "Exception";
        result.errorMessage = e.what();
    }

    ctx.reportProgress(1.0, "fillet.complete");
    return result;
}

App::GeometryArchiveWriteResult
FilletGeometryOperation::writeArchive(App::GeometryArchiveWriter& writer) const
{
    App::GeometryArchiveWriteResult out;
    auto* workspace = dynamic_cast<App::GeometryRequestWorkspace*>(&writer);
    if (!workspace) {
        out.errorCode = "UnsupportedArchiveWriter";
        out.errorMessage = "Part::Fillet requires GeometryRequestWorkspace";
        return out;
    }

    const QString baseRel = QStringLiteral("base.fcg");
    if (!workspace->clearStagedFile(baseRel)) {
        out.errorCode = workspace->failureCode().empty() ? "StaleOperandCleanupFailed"
                                                         : workspace->failureCode();
        out.errorMessage = workspace->failureMessage().empty()
            ? "Failed to remove stale Fillet base archive"
            : workspace->failureMessage();
        return out;
    }

    QTemporaryFile baseTmp;
    baseTmp.setAutoRemove(true);
    if (!baseTmp.open()) {
        out.errorCode = "TempFileFailed";
        out.errorMessage = "Failed to create temporary base archive";
        workspace->markFailed(out.errorCode, out.errorMessage);
        return out;
    }
    const QString baseTmpPath = baseTmp.fileName();
    baseTmp.close();

    if (!TopoShapeArchive::writeArchive(_base, baseTmpPath.toStdString())) {
        out.errorCode = "OperandArchiveWriteFailed";
        out.errorMessage = "Failed to serialize Fillet base archive";
        workspace->markFailed(out.errorCode, out.errorMessage);
        return out;
    }
    if (!workspace->stageFileAtomic(baseRel, baseTmpPath)) {
        out.errorCode = workspace->failureCode().empty() ? "OperandStageFailed"
                                                         : workspace->failureCode();
        out.errorMessage = workspace->failureMessage().empty()
            ? "Failed to stage Fillet base archive"
            : workspace->failureMessage();
        return out;
    }

    const QString baseAbs = QDir(workspace->workspaceDir()).filePath(baseRel);

    QJsonArray edgesArr;
    for (const auto& spec : _edges) {
        QJsonObject edgeObj;
        edgeObj.insert(QStringLiteral("edgeIndex"), static_cast<qint64>(spec.edgeIndex));
        edgeObj.insert(QStringLiteral("startRadius"), spec.startRadius);
        edgeObj.insert(QStringLiteral("endRadius"), spec.endRadius);
        edgesArr.append(edgeObj);
    }

    auto& req = workspace->requestObject();
    req.insert(QStringLiteral("operationType"), QStringLiteral("Part::Fillet"));
    req.insert(QStringLiteral("codecVersion"), static_cast<int>(codecVersion()));
    req.insert(QStringLiteral("basePath"), baseRel);
    req.insert(QStringLiteral("baseSize"), static_cast<qint64>(QFileInfo(baseAbs).size()));
    req.insert(QStringLiteral("baseSha256"),
               QString::fromStdString(TopoShapeArchive::calculateSha256File(baseAbs.toStdString())));
    req.insert(QStringLiteral("edges"), edgesArr);

    out.success = true;
    return out;
}

std::shared_ptr<FilletGeometryOperation>
FilletGeometryOperation::decodeFromRequest(const QJsonObject& request,
                                           const QString& workspaceDir,
                                           std::string& errorCode,
                                           std::string& errorMessage)
{
    errorCode.clear();
    errorMessage.clear();

    QString op;
    if (!requireString(request, "operationType", op, errorCode, errorMessage)) {
        return nullptr;
    }
    if (op != QStringLiteral("Part::Fillet")) {
        errorCode = "UnsupportedOperation";
        errorMessage = "Expected Part::Fillet";
        return nullptr;
    }

    qint64 codec64 = 0;
    const qint64 maxCodecVersion = static_cast<qint64>(std::numeric_limits<uint32_t>::max());
    if (!requireJsonInt64(request,
                          "codecVersion",
                          codec64,
                          errorCode,
                          errorMessage,
                          maxCodecVersion)) {
        return nullptr;
    }
    if (codec64 < 0) {
        errorCode = "OutOfRangeJsonNumber";
        errorMessage = "codecVersion is out of range";
        return nullptr;
    }
    if (static_cast<uint32_t>(codec64) != FilletGeometryOperation().codecVersion()) {
        errorCode = "UnsupportedCodecVersion";
        errorMessage = "Unsupported Part::Fillet codecVersion";
        return nullptr;
    }

    QString basePath;
    if (!requireString(request, "basePath", basePath, errorCode, errorMessage)) {
        return nullptr;
    }
    if (!App::GeometryRequestWorkspace::isTrustedRelativePath(basePath)) {
        errorCode = "UntrustedOperandPath";
        errorMessage = "basePath must be a trusted relative path";
        return nullptr;
    }
    const QString baseAbs = QDir(workspaceDir).filePath(basePath);
    if (!QFileInfo::exists(baseAbs) || !QFileInfo(baseAbs).isFile()) {
        errorCode = "MissingOperandArchive";
        errorMessage = "basePath archive is missing";
        return nullptr;
    }
    qint64 claimedSize = -1;
    const qint64 maxSection =
        static_cast<qint64>(App::GeometryRequestWorkspace::maxWorkspaceSectionBytes());
    if (!requireJsonInt64(request, "baseSize", claimedSize, errorCode, errorMessage, maxSection)) {
        return nullptr;
    }
    if (claimedSize < 0 || QFileInfo(baseAbs).size() != claimedSize) {
        errorCode = "OperandSizeMismatch";
        errorMessage = "baseSize does not match on-disk size";
        return nullptr;
    }
    QString claimedSha;
    if (!requireString(request, "baseSha256", claimedSha, errorCode, errorMessage)) {
        return nullptr;
    }
    claimedSha = claimedSha.toLower();
    const QString actualSha =
        QString::fromStdString(TopoShapeArchive::calculateSha256File(baseAbs.toStdString())).toLower();
    if (claimedSha.isEmpty() || claimedSha != actualSha) {
        errorCode = "OperandDigestMismatch";
        errorMessage = "baseSha256 does not match on-disk digest";
        return nullptr;
    }

    FrozenTopoShapeBundle base;
    if (!TopoShapeArchive::readArchive(baseAbs.toStdString(), base) || !base.valid) {
        errorCode = "OperandDecodeFailed";
        errorMessage = "basePath failed FCG1 decode";
        return nullptr;
    }

    const QJsonValue edgesValue = request.value(QStringLiteral("edges"));
    if (!request.contains(QStringLiteral("edges"))) {
        errorCode = "MissingJsonField";
        errorMessage = "edges is required";
        return nullptr;
    }
    if (!edgesValue.isArray()) {
        errorCode = "WrongJsonType";
        errorMessage = "edges must be a JSON array";
        return nullptr;
    }
    const QJsonArray edgesArr = edgesValue.toArray();
    if (edgesArr.isEmpty()) {
        errorCode = "EmptyEdges";
        errorMessage = "edges must be a non-empty array";
        return nullptr;
    }

    const size_t edgeCount = countShapeEdges(base.shape);
    std::unordered_set<uint32_t> seenIndices;
    std::vector<FilletEdgeSpec> specs;
    specs.reserve(static_cast<size_t>(edgesArr.size()));

    for (const QJsonValue& entry : edgesArr) {
        if (!entry.isObject()) {
            errorCode = "WrongJsonType";
            errorMessage = "each edges entry must be a JSON object";
            return nullptr;
        }
        const QJsonObject edgeObj = entry.toObject();

        uint32_t edgeIndex = 0;
        if (!requireEdgeIndex(edgeObj, edgeIndex, edgeCount, errorCode, errorMessage)) {
            return nullptr;
        }
        if (!seenIndices.insert(edgeIndex).second) {
            errorCode = "DuplicateEdgeIndex";
            errorMessage = "edgeIndex values must be unique";
            return nullptr;
        }

        double startRadius = 0.0;
        double endRadius = 0.0;
        if (!requirePositiveFiniteDouble(edgeObj, "startRadius", startRadius, errorCode, errorMessage)
            || !requirePositiveFiniteDouble(edgeObj, "endRadius", endRadius, errorCode, errorMessage)) {
            return nullptr;
        }

        specs.push_back({edgeIndex, startRadius, endRadius});
    }

    return std::make_shared<FilletGeometryOperation>(base, specs);
}

} // namespace Part
