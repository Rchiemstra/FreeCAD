// SPDX-License-Identifier: LGPL-2.1-or-later

#include "BooleanGeometryOperation.h"
#include <App/GeometryRequestWorkspace.h>
#include <Mod/Part/App/TopoShapeOpCode.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Standard_Failure.hxx>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryFile>

#include <memory>

namespace Part
{

namespace
{

TopoShape materializeOperand(const FrozenTopoShapeBundle& bundle, long fallbackTag)
{
    TopoShape shape = bundle.shape;
    const long tag = bundle.shapeTag != 0 ? bundle.shapeTag : (shape.Tag != 0 ? shape.Tag : fallbackTag);
    shape.Tag = tag;
    if (bundle.hasher) {
        shape.Hasher = bundle.hasher;
    }
    if (bundle.elementMap) {
        shape.resetElementMap(bundle.elementMap);
    }
    return shape;
}

const char* makerForType(BooleanType type)
{
    switch (type) {
        case BooleanType::Cut:
            return OpCodes::Cut;
        case BooleanType::Common:
            return OpCodes::Common;
        case BooleanType::Section:
            return OpCodes::Section;
        case BooleanType::Fuse:
        default:
            return OpCodes::Fuse;
    }
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

bool requireInt(const QJsonObject& request,
                const char* key,
                int& out,
                std::string& errorCode,
                std::string& errorMessage)
{
    const QJsonValue v = request.value(QString::fromLatin1(key));
    if (!v.isDouble()) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be a JSON number";
        return false;
    }
    const double d = v.toDouble();
    if (d != static_cast<double>(static_cast<qint64>(d))) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be an integer";
        return false;
    }
    out = static_cast<int>(d);
    return true;
}

bool requireInt64(const QJsonObject& request,
                  const char* key,
                  qint64& out,
                  std::string& errorCode,
                  std::string& errorMessage)
{
    const QJsonValue v = request.value(QString::fromLatin1(key));
    if (!v.isDouble()) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be a JSON number";
        return false;
    }
    const double d = v.toDouble();
    if (d != static_cast<double>(static_cast<qint64>(d))) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be an integer";
        return false;
    }
    out = static_cast<qint64>(d);
    return true;
}

} // namespace

BooleanGeometryOperation::BooleanGeometryOperation() = default;

BooleanGeometryOperation::BooleanGeometryOperation(BooleanType type, const FrozenTopoShapeBundle& base, const FrozenTopoShapeBundle& tool)
    : _type(type), _base(base), _tool(tool)
{
}

BooleanGeometryOperation::~BooleanGeometryOperation() = default;

std::string BooleanGeometryOperation::parameterDigest() const
{
    return std::string("bool:")
        + std::to_string(static_cast<int>(_type)) + '|'
        + TopoShapeArchive::fingerprintBundle(_base) + '|'
        + TopoShapeArchive::fingerprintBundle(_tool);
}

App::GeometryOperationTraits BooleanGeometryOperation::traits() const
{
    App::GeometryOperationTraits t;
    t.supportsInProcess = true;
    t.supportsCooperativeCancel = true;
    return t;
}

App::DetachedGeometryResult BooleanGeometryOperation::run(App::GeometryWorkerContext& ctx) const
{
    App::DetachedGeometryResult result;
    ctx.reportProgress(0.1, "boolean.prepare");

    if (ctx.isCancelled()) {
        result.success = false;
        result.errorCode = "Cancelled";
        result.errorMessage = "Cancelled before boolean start";
        return result;
    }

    // Always restore both operands against one hasher before OCC.
    App::StringHasherRef shared(new App::StringHasher);
    FrozenTopoShapeBundle baseBundle;
    FrozenTopoShapeBundle toolBundle;
    std::string bindError;
    std::string bindMessage;
    if (!TopoShapeArchive::materializeOperandsOntoSharedHasher(_base,
                                                               _tool,
                                                               shared,
                                                               baseBundle,
                                                               toolBundle,
                                                               bindError,
                                                               bindMessage)) {
        result.success = false;
        result.errorCode = bindError;
        result.errorMessage = bindMessage;
        return result;
    }

    TopoShape base = materializeOperand(baseBundle, /*fallbackTag=*/1);
    TopoShape tool = materializeOperand(toolBundle, /*fallbackTag=*/2);
    if (base.Tag == tool.Tag) {
        tool.Tag = base.Tag == 0 ? 2 : base.Tag + 1;
    }
    base.Hasher = shared;
    tool.Hasher = shared;

    if (base.isNull() || tool.isNull()) {
        result.success = false;
        result.errorCode = "NullShape";
        result.errorMessage = "One of the boolean operand shapes is null";
        return result;
    }

    ctx.reportProgress(0.4, "boolean.compute");

    try {
        TopoShape outShape(0, shared);
        outShape.makeElementBoolean(makerForType(_type), {base, tool});

        if (ctx.isCancelled()) {
            result.success = false;
            result.errorCode = "Cancelled";
            result.errorMessage = "Cancelled during boolean compute";
            return result;
        }

        if (outShape.isNull()) {
            result.success = false;
            result.errorCode = "NullResult";
            result.errorMessage = "Boolean operation produced a null shape";
            return result;
        }

        ctx.reportProgress(0.8, "boolean.archive");
        FrozenTopoShapeBundle outBundle = TopoShapeArchive::createBundle(outShape);
        if (!outBundle.valid) {
            result.success = false;
            result.errorCode = outBundle.errorCode.empty() ? "BundleInvalid" : outBundle.errorCode;
            result.errorMessage = "Failed to freeze mapped boolean result";
            return result;
        }

        std::string resultPath = ctx.tempDir() + "/result.fcg";
        if (TopoShapeArchive::writeArchive(outBundle, resultPath)) {
            result.success = true;
            result.resultArchivePath = resultPath;
        }
        else {
            result.success = false;
            result.errorCode = "ArchiveWriteError";
            result.errorMessage = "Failed to write output result archive";
        }
    }
    catch (const Base::Exception& e) {
        result.success = false;
        result.errorCode = "BooleanError";
        result.errorMessage = e.what();
    }
    catch (const Standard_Failure& e) {
        result.success = false;
        result.errorCode = "OCCError";
        result.errorMessage = e.GetMessageString() ? e.GetMessageString()
                                                   : "OCC Exception during boolean operation";
    }
    catch (const std::exception& e) {
        result.success = false;
        result.errorCode = "Exception";
        result.errorMessage = e.what();
    }

    ctx.reportProgress(1.0, "boolean.complete");
    return result;
}

App::GeometryArchiveWriteResult
BooleanGeometryOperation::writeArchive(App::GeometryArchiveWriter& writer) const
{
    App::GeometryArchiveWriteResult out;
    auto* workspace = dynamic_cast<App::GeometryRequestWorkspace*>(&writer);
    if (!workspace) {
        out.errorCode = "UnsupportedArchiveWriter";
        out.errorMessage = "Part::Boolean requires GeometryRequestWorkspace";
        return out;
    }

    // Drop stale operand archives from a reused workspace before staging.
    const QString baseRel = QStringLiteral("base.fcg");
    const QString toolRel = QStringLiteral("tool.fcg");
    if (!workspace->clearStagedFile(baseRel) || !workspace->clearStagedFile(toolRel)) {
        out.errorCode = workspace->failureCode().empty() ? "StaleOperandCleanupFailed"
                                                         : workspace->failureCode();
        out.errorMessage = workspace->failureMessage().empty()
            ? "Failed to remove stale Boolean operand archives"
            : workspace->failureMessage();
        return out;
    }

    QTemporaryFile baseTmp;
    QTemporaryFile toolTmp;
    baseTmp.setAutoRemove(true);
    toolTmp.setAutoRemove(true);
    if (!baseTmp.open() || !toolTmp.open()) {
        out.errorCode = "TempFileFailed";
        out.errorMessage = "Failed to create temporary operand archives";
        workspace->markFailed(out.errorCode, out.errorMessage);
        return out;
    }
    const QString baseTmpPath = baseTmp.fileName();
    const QString toolTmpPath = toolTmp.fileName();
    baseTmp.close();
    toolTmp.close();

    if (!TopoShapeArchive::writeArchive(_base, baseTmpPath.toStdString())
        || !TopoShapeArchive::writeArchive(_tool, toolTmpPath.toStdString())) {
        out.errorCode = "OperandArchiveWriteFailed";
        out.errorMessage = "Failed to serialize Boolean operand archives";
        workspace->markFailed(out.errorCode, out.errorMessage);
        return out;
    }
    if (!workspace->stageFileAtomic(baseRel, baseTmpPath)
        || !workspace->stageFileAtomic(toolRel, toolTmpPath)) {
        out.errorCode = workspace->failureCode().empty() ? "OperandStageFailed"
                                                         : workspace->failureCode();
        out.errorMessage = workspace->failureMessage().empty()
            ? "Failed to stage Boolean operand archives"
            : workspace->failureMessage();
        return out;
    }

    const QString baseAbs = QDir(workspace->workspaceDir()).filePath(baseRel);
    const QString toolAbs = QDir(workspace->workspaceDir()).filePath(toolRel);

    auto& req = workspace->requestObject();
    req.insert(QStringLiteral("operationType"), QStringLiteral("Part::Boolean"));
    req.insert(QStringLiteral("codecVersion"), static_cast<int>(codecVersion()));
    const char* typeName = "Fuse";
    switch (_type) {
        case BooleanType::Cut:
            typeName = "Cut";
            break;
        case BooleanType::Common:
            typeName = "Common";
            break;
        case BooleanType::Section:
            typeName = "Section";
            break;
        case BooleanType::Fuse:
        default:
            typeName = "Fuse";
            break;
    }
    req.insert(QStringLiteral("booleanType"), QString::fromLatin1(typeName));
    req.insert(QStringLiteral("basePath"), baseRel);
    req.insert(QStringLiteral("baseSize"), static_cast<qint64>(QFileInfo(baseAbs).size()));
    req.insert(QStringLiteral("baseSha256"),
               QString::fromStdString(TopoShapeArchive::calculateSha256File(baseAbs.toStdString())));
    req.insert(QStringLiteral("toolPath"), toolRel);
    req.insert(QStringLiteral("toolSize"), static_cast<qint64>(QFileInfo(toolAbs).size()));
    req.insert(QStringLiteral("toolSha256"),
               QString::fromStdString(TopoShapeArchive::calculateSha256File(toolAbs.toStdString())));

    out.success = true;
    return out;
}

App::DetachedGeometryResult
BooleanGeometryOperation::decodeResultArchive(const std::string& absolutePath) const
{
    App::DetachedGeometryResult result;
    result.resultArchivePath = absolutePath;

    Part::FrozenTopoShapeBundle candidate;
    candidate.valid = false;
    if (!TopoShapeArchive::readArchive(absolutePath, candidate) || !candidate.valid) {
        result.success = false;
        result.errorCode = candidate.errorCode.empty() ? "ResultDecodeFailed" : candidate.errorCode;
        result.errorMessage = "FCG1 Boolean result failed structural decode";
        return result;
    }
    if (candidate.shape.isNull()) {
        result.success = false;
        result.errorCode = "NullResultShape";
        result.errorMessage = "Decoded Boolean result shape is null";
        return result;
    }
    if (!candidate.hasher) {
        result.success = false;
        result.errorCode = "MissingResultHasher";
        result.errorMessage = "Mapped Boolean result requires a hasher";
        return result;
    }
    if (!candidate.elementMap || candidate.elementMap->size() == 0) {
        result.success = false;
        result.errorCode = "MissingResultElementMap";
        result.errorMessage = "Mapped Boolean result requires a non-empty ElementMap";
        return result;
    }
    if (candidate.hasherSnapshot.entries.empty()) {
        result.success = false;
        result.errorCode = "MissingResultHasherClosure";
        result.errorMessage = "Mapped Boolean result requires a hasher closure";
        return result;
    }
    if (static_cast<App::StringHasher*>(candidate.shape.Hasher)
        != static_cast<App::StringHasher*>(candidate.hasher)) {
        result.success = false;
        result.errorCode = "ResultHasherMismatch";
        result.errorMessage = "Decoded shape hasher does not match bundle hasher";
        return result;
    }

    result.success = true;
    return result;
}

std::shared_ptr<BooleanGeometryOperation>
BooleanGeometryOperation::decodeFromRequest(const QJsonObject& request,
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
    if (op != QStringLiteral("Part::Boolean")) {
        errorCode = "UnsupportedOperation";
        errorMessage = "Expected Part::Boolean";
        return nullptr;
    }

    int codec = -1;
    if (!requireInt(request, "codecVersion", codec, errorCode, errorMessage)) {
        return nullptr;
    }
    if (codec != 1) {
        errorCode = "UnsupportedCodecVersion";
        errorMessage = "Unsupported Part::Boolean codecVersion";
        return nullptr;
    }

    QString typeName;
    if (!requireString(request, "booleanType", typeName, errorCode, errorMessage)) {
        return nullptr;
    }
    BooleanType type = BooleanType::Fuse;
    if (typeName == QStringLiteral("Cut")) {
        type = BooleanType::Cut;
    }
    else if (typeName == QStringLiteral("Common")) {
        type = BooleanType::Common;
    }
    else if (typeName == QStringLiteral("Section")) {
        type = BooleanType::Section;
    }
    else if (typeName == QStringLiteral("Fuse")) {
        type = BooleanType::Fuse;
    }
    else {
        errorCode = "UnknownBooleanType";
        errorMessage = "booleanType is missing or unsupported";
        return nullptr;
    }

    QString basePath;
    QString toolPath;
    if (!requireString(request, "basePath", basePath, errorCode, errorMessage)
        || !requireString(request, "toolPath", toolPath, errorCode, errorMessage)) {
        return nullptr;
    }
    if (QDir::fromNativeSeparators(basePath) == QDir::fromNativeSeparators(toolPath)) {
        errorCode = "DuplicateOperandPath";
        errorMessage = "basePath and toolPath must be distinct";
        return nullptr;
    }

    auto loadOperand = [&](const char* pathKey,
                           const char* sizeKey,
                           const char* shaKey,
                           FrozenTopoShapeBundle& out) -> bool {
        QString rel;
        if (!requireString(request, pathKey, rel, errorCode, errorMessage)) {
            return false;
        }
        if (!App::GeometryRequestWorkspace::isTrustedRelativePath(rel)) {
            errorCode = "UntrustedOperandPath";
            errorMessage = std::string(pathKey) + " must be a trusted relative path";
            return false;
        }
        const QString abs = QDir(workspaceDir).filePath(rel);
        if (!QFileInfo::exists(abs)) {
            errorCode = "MissingOperandArchive";
            errorMessage = std::string(pathKey) + " archive is missing";
            return false;
        }
        qint64 claimedSize = -1;
        if (!requireInt64(request, sizeKey, claimedSize, errorCode, errorMessage)) {
            return false;
        }
        if (claimedSize < 0 || QFileInfo(abs).size() != claimedSize) {
            errorCode = "OperandSizeMismatch";
            errorMessage = std::string(sizeKey) + " does not match on-disk size";
            return false;
        }
        QString claimedSha;
        if (!requireString(request, shaKey, claimedSha, errorCode, errorMessage)) {
            return false;
        }
        claimedSha = claimedSha.toLower();
        const QString actualSha =
            QString::fromStdString(TopoShapeArchive::calculateSha256File(abs.toStdString())).toLower();
        if (claimedSha.isEmpty() || claimedSha != actualSha) {
            errorCode = "OperandDigestMismatch";
            errorMessage = std::string(shaKey) + " does not match on-disk digest";
            return false;
        }
        if (!TopoShapeArchive::readArchive(abs.toStdString(), out) || !out.valid) {
            errorCode = "OperandDecodeFailed";
            errorMessage = std::string(pathKey) + " failed FCG1 decode";
            return false;
        }
        return true;
    };

    FrozenTopoShapeBundle base;
    FrozenTopoShapeBundle tool;
    if (!loadOperand("basePath", "baseSize", "baseSha256", base)
        || !loadOperand("toolPath", "toolSize", "toolSha256", tool)) {
        return nullptr;
    }

    App::StringHasherRef worker(new App::StringHasher);
    FrozenTopoShapeBundle baseBound;
    FrozenTopoShapeBundle toolBound;
    if (!TopoShapeArchive::materializeOperandsOntoSharedHasher(base,
                                                               tool,
                                                               worker,
                                                               baseBound,
                                                               toolBound,
                                                               errorCode,
                                                               errorMessage)) {
        return nullptr;
    }

    return std::make_shared<BooleanGeometryOperation>(type, baseBound, toolBound);
}

} // namespace Part
