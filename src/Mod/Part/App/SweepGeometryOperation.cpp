// SPDX-License-Identifier: LGPL-2.1-or-later

#include "SweepGeometryOperation.h"
#include <App/GeometryRequestWorkspace.h>
#include <Mod/Part/App/TopoShapeOpCode.h>
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

bool requireBool(const QJsonObject& request,
                 const char* key,
                 bool& out,
                 std::string& errorCode,
                 std::string& errorMessage)
{
    if (!request.contains(QString::fromLatin1(key))) {
        errorCode = "MissingJsonField";
        errorMessage = std::string(key) + " is required";
        return false;
    }
    const QJsonValue v = request.value(QString::fromLatin1(key));
    if (!v.isBool()) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be a JSON boolean";
        return false;
    }
    out = v.toBool();
    return true;
}

QString normalizeCanonicalOperandPath(const QString& relativePath)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(relativePath));
}

bool materializeSweepOperandsOntoSharedHasher(const FrozenTopoShapeBundle& spineIn,
                                              const std::vector<FrozenTopoShapeBundle>& profilesIn,
                                              App::StringHasherRef workerHasher,
                                              FrozenTopoShapeBundle& spineOut,
                                              std::vector<FrozenTopoShapeBundle>& profilesOut,
                                              std::string& errorCode,
                                              std::string& errorMessage)
{
    errorCode.clear();
    errorMessage.clear();
    if (!workerHasher) {
        errorCode = "NullHasher";
        errorMessage = "Shared worker hasher is null";
        return false;
    }
    if (!spineIn.valid) {
        errorCode = "InvalidOperand";
        errorMessage = "Sweep spine operand bundle is invalid";
        return false;
    }
    if (profilesIn.empty()) {
        errorCode = "NoProfiles";
        errorMessage = "Sweep requires at least one profile section";
        return false;
    }
    for (const auto& profile : profilesIn) {
        if (!profile.valid) {
            errorCode = "InvalidOperand";
            errorMessage = "Sweep profile operand bundle is invalid";
            return false;
        }
    }

    auto matSpine = workerHasher->materializeExactClosure(spineIn.hasherSnapshot);
    if (!matSpine.success) {
        errorCode = matSpine.errorCode.empty() ? "SpineHasherMaterializeFailed" : matSpine.errorCode;
        errorMessage = matSpine.errorMessage.empty()
            ? "Failed to materialize spine hasher closure"
            : matSpine.errorMessage;
        return false;
    }
    for (size_t i = 0; i < profilesIn.size(); ++i) {
        auto mergeProfile = workerHasher->mergeExactClosure(profilesIn[i].hasherSnapshot,
                                                              /*expectedRevision=*/0);
        if (!mergeProfile.success) {
            errorCode = mergeProfile.errorCode.empty() ? "ProfileHasherMergeFailed" : mergeProfile.errorCode;
            errorMessage = mergeProfile.errorMessage.empty()
                ? "Failed to merge profile hasher closure onto shared worker hasher"
                : mergeProfile.errorMessage;
            return false;
        }
    }

    spineOut = spineIn;
    profilesOut = profilesIn;
    if (!TopoShapeArchive::rebindBundleToHasher(spineOut, workerHasher)) {
        errorCode = "SpineRebindFailed";
        errorMessage = "Failed to rebind spine ElementMap onto shared worker hasher";
        return false;
    }
    for (auto& profile : profilesOut) {
        if (!TopoShapeArchive::rebindBundleToHasher(profile, workerHasher)) {
            errorCode = "ProfileRebindFailed";
            errorMessage = "Failed to rebind profile ElementMap onto shared worker hasher";
            return false;
        }
    }
    return true;
}

} // namespace

SweepGeometryOperation::SweepGeometryOperation() = default;

SweepGeometryOperation::SweepGeometryOperation(const FrozenTopoShapeBundle& spine,
                                               const std::vector<FrozenTopoShapeBundle>& profiles,
                                               bool isSolid)
    : _spine(spine)
    , _profiles(profiles)
    , _isSolid(isSolid)
{
}

SweepGeometryOperation::~SweepGeometryOperation() = default;

std::string SweepGeometryOperation::parameterDigest() const
{
    std::string digest = std::string("sweep:solid=")
        + (_isSolid ? '1' : '0') + '|'
        + TopoShapeArchive::fingerprintBundle(_spine);
    for (const auto& profile : _profiles) {
        digest.push_back('|');
        digest += TopoShapeArchive::fingerprintBundle(profile);
    }
    return digest;
}

App::GeometryOperationTraits SweepGeometryOperation::traits() const
{
    App::GeometryOperationTraits t;
    t.supportsInProcess = false; // Process-first for sweep
    t.supportsCooperativeCancel = true;
    return t;
}

App::DetachedGeometryResult SweepGeometryOperation::run(App::GeometryWorkerContext& ctx) const
{
    App::DetachedGeometryResult result;
    ctx.reportProgress(0.1, "sweep.prepare");

    if (ctx.isCancelled()) {
        result.success = false;
        result.errorCode = "Cancelled";
        result.errorMessage = "Cancelled before sweep start";
        return result;
    }

    if (_profiles.empty()) {
        result.success = false;
        result.errorCode = "NoProfiles";
        result.errorMessage = "Sweep requires at least one profile section";
        return result;
    }

    TopoShape spine = materializeOperand(_spine, /*fallbackTag=*/1);
    if (spine.isNull()) {
        result.success = false;
        result.errorCode = "InvalidSpine";
        result.errorMessage = "Spine shape is null";
        return result;
    }

    // makeElementPipeShell accepts a spine that can form a single wire (edge/wire/face outline).
    App::StringHasherRef hasher = spine.Hasher;
    std::vector<TopoShape> shapes;
    shapes.push_back(spine);

    long nextTag = spine.Tag == 0 ? 2 : spine.Tag + 1;
    for (const auto& profBundle : _profiles) {
        TopoShape profile = materializeOperand(profBundle, nextTag);
        if (profile.Tag == spine.Tag || profile.Tag == 0) {
            profile.Tag = nextTag;
        }
        ++nextTag;
        if (profile.isNull()) {
            result.success = false;
            result.errorCode = "InvalidProfile";
            result.errorMessage = "A sweep profile shape is null";
            return result;
        }
        if (!hasher && profile.Hasher) {
            hasher = profile.Hasher;
        }
        shapes.push_back(std::move(profile));
    }

    if (!hasher) {
        hasher = App::StringHasherRef(new App::StringHasher);
    }
    for (auto& shape : shapes) {
        if (!shape.Hasher) {
            shape.Hasher = hasher;
        }
    }

    ctx.reportProgress(0.5, "sweep.compute");

    try {
        if (ctx.isCancelled()) {
            result.success = false;
            result.errorCode = "Cancelled";
            result.errorMessage = "Cancelled during sweep compute";
            return result;
        }

        TopoShape outShape(0, hasher);
        outShape.makeElementPipeShell(
            shapes,
            _isSolid ? MakeSolid::makeSolid : MakeSolid::noSolid,
            /*isFrenet=*/Standard_False,
            TransitionMode::Transformed,
            OpCodes::Sweep);

        if (outShape.isNull()) {
            result.success = false;
            result.errorCode = "NullResult";
            result.errorMessage = "Mapped sweep produced a null shape";
            return result;
        }

        ctx.reportProgress(0.8, "sweep.archive");
        FrozenTopoShapeBundle outBundle = TopoShapeArchive::createBundle(outShape);
        if (!outBundle.valid) {
            result.success = false;
            result.errorCode = outBundle.errorCode.empty() ? "BundleInvalid" : outBundle.errorCode;
            result.errorMessage = "Failed to freeze mapped sweep result";
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
            result.errorMessage = "Failed to write sweep output archive";
        }
    }
    catch (const Base::Exception& e) {
        result.success = false;
        result.errorCode = "SweepError";
        result.errorMessage = e.what();
    }
    catch (const Standard_Failure& e) {
        result.success = false;
        result.errorCode = "OCCError";
        result.errorMessage = e.GetMessageString() ? e.GetMessageString()
                                                   : "OCC Exception during sweep";
    }
    catch (const std::exception& e) {
        result.success = false;
        result.errorCode = "Exception";
        result.errorMessage = e.what();
    }

    ctx.reportProgress(1.0, "sweep.complete");
    return result;
}

App::GeometryArchiveWriteResult
SweepGeometryOperation::writeArchive(App::GeometryArchiveWriter& writer) const
{
    App::GeometryArchiveWriteResult out;
    auto* workspace = dynamic_cast<App::GeometryRequestWorkspace*>(&writer);
    if (!workspace) {
        out.errorCode = "UnsupportedArchiveWriter";
        out.errorMessage = "Part::Sweep requires GeometryRequestWorkspace";
        return out;
    }

    if (_profiles.empty()) {
        out.success = false;
        out.errorCode = "NoProfiles";
        out.errorMessage = "Sweep requires at least one profile section";
        workspace->markFailed(out.errorCode, out.errorMessage);
        return out;
    }

    const QString spineRel = QStringLiteral("spine.fcg");
    if (!workspace->clearStagedFile(spineRel)) {
        out.errorCode = workspace->failureCode().empty() ? "StaleOperandCleanupFailed"
                                                         : workspace->failureCode();
        out.errorMessage = workspace->failureMessage().empty()
            ? "Failed to remove stale Sweep spine archive"
            : workspace->failureMessage();
        return out;
    }
    for (size_t i = 0; i < _profiles.size(); ++i) {
        const QString profileRel =
            QStringLiteral("profile-%1.fcg").arg(static_cast<qulonglong>(i));
        if (!workspace->clearStagedFile(profileRel)) {
            out.errorCode = workspace->failureCode().empty() ? "StaleOperandCleanupFailed"
                                                             : workspace->failureCode();
            out.errorMessage = workspace->failureMessage().empty()
                ? "Failed to remove stale Sweep profile archives"
                : workspace->failureMessage();
            return out;
        }
    }

    std::vector<std::unique_ptr<QTemporaryFile>> operandTmps;
    operandTmps.reserve(1 + _profiles.size());

    auto openOperandTmp = [&](const char* context) -> bool {
        auto tmp = std::make_unique<QTemporaryFile>();
        tmp->setAutoRemove(true);
        if (!tmp->open()) {
            out.errorCode = "TempFileFailed";
            out.errorMessage = context;
            workspace->markFailed(out.errorCode, out.errorMessage);
            return false;
        }
        tmp->close();
        operandTmps.push_back(std::move(tmp));
        return true;
    };

    if (!openOperandTmp("Failed to create temporary spine archive")) {
        return out;
    }
    for (size_t i = 0; i < _profiles.size(); ++i) {
        (void)i;
        if (!openOperandTmp("Failed to create temporary profile archives")) {
            return out;
        }
    }

    if (!TopoShapeArchive::writeArchive(_spine, operandTmps[0]->fileName().toStdString())) {
        out.errorCode = "OperandArchiveWriteFailed";
        out.errorMessage = "Failed to serialize Sweep spine archive";
        workspace->markFailed(out.errorCode, out.errorMessage);
        return out;
    }
    for (size_t i = 0; i < _profiles.size(); ++i) {
        if (!TopoShapeArchive::writeArchive(_profiles[i],
                                            operandTmps[1 + i]->fileName().toStdString())) {
            out.errorCode = "OperandArchiveWriteFailed";
            out.errorMessage = "Failed to serialize Sweep profile archives";
            workspace->markFailed(out.errorCode, out.errorMessage);
            return out;
        }
    }

    if (!workspace->stageFileAtomic(spineRel, operandTmps[0]->fileName())) {
        out.errorCode = workspace->failureCode().empty() ? "OperandStageFailed"
                                                         : workspace->failureCode();
        out.errorMessage = workspace->failureMessage().empty()
            ? "Failed to stage Sweep spine archive"
            : workspace->failureMessage();
        return out;
    }
    for (size_t i = 0; i < _profiles.size(); ++i) {
        const QString profileRel =
            QStringLiteral("profile-%1.fcg").arg(static_cast<qulonglong>(i));
        if (!workspace->stageFileAtomic(profileRel, operandTmps[1 + i]->fileName())) {
            out.errorCode = workspace->failureCode().empty() ? "OperandStageFailed"
                                                             : workspace->failureCode();
            out.errorMessage = workspace->failureMessage().empty()
                ? "Failed to stage Sweep profile archives"
                : workspace->failureMessage();
            return out;
        }
    }

    const QString spineAbs = QDir(workspace->workspaceDir()).filePath(spineRel);

    QJsonArray profilesArr;
    for (size_t i = 0; i < _profiles.size(); ++i) {
        const QString profileRel =
            QStringLiteral("profile-%1.fcg").arg(static_cast<qulonglong>(i));
        const QString profileAbs = QDir(workspace->workspaceDir()).filePath(profileRel);
        QJsonObject profileObj;
        profileObj.insert(QStringLiteral("path"), profileRel);
        profileObj.insert(QStringLiteral("size"), static_cast<qint64>(QFileInfo(profileAbs).size()));
        profileObj.insert(QStringLiteral("sha256"),
                          QString::fromStdString(
                              TopoShapeArchive::calculateSha256File(profileAbs.toStdString())));
        profilesArr.append(profileObj);
    }

    auto& req = workspace->requestObject();
    req.insert(QStringLiteral("operationType"), QStringLiteral("Part::Sweep"));
    req.insert(QStringLiteral("codecVersion"), static_cast<int>(codecVersion()));
    req.insert(QStringLiteral("isSolid"), _isSolid);
    req.insert(QStringLiteral("spinePath"), spineRel);
    req.insert(QStringLiteral("spineSize"), static_cast<qint64>(QFileInfo(spineAbs).size()));
    req.insert(QStringLiteral("spineSha256"),
               QString::fromStdString(TopoShapeArchive::calculateSha256File(spineAbs.toStdString())));
    req.insert(QStringLiteral("profiles"), profilesArr);

    out.success = true;
    return out;
}

App::DetachedGeometryResult
SweepGeometryOperation::decodeResultArchive(const std::string& absolutePath) const
{
    App::DetachedGeometryResult result;
    result.resultArchivePath = absolutePath;

    Part::FrozenTopoShapeBundle candidate;
    candidate.valid = false;
    if (!TopoShapeArchive::readArchive(absolutePath, candidate) || !candidate.valid) {
        result.success = false;
        result.errorCode = candidate.errorCode.empty() ? "ResultDecodeFailed" : candidate.errorCode;
        result.errorMessage = "FCG1 Sweep result failed structural decode";
        return result;
    }
    if (candidate.shape.isNull()) {
        result.success = false;
        result.errorCode = "NullResultShape";
        result.errorMessage = "Decoded Sweep result shape is null";
        return result;
    }
    if (!candidate.hasher) {
        result.success = false;
        result.errorCode = "MissingResultHasher";
        result.errorMessage = "Mapped Sweep result requires a hasher";
        return result;
    }
    if (!candidate.elementMap || candidate.elementMap->size() == 0) {
        result.success = false;
        result.errorCode = "MissingResultElementMap";
        result.errorMessage = "Mapped Sweep result requires a non-empty ElementMap";
        return result;
    }
    if (candidate.hasherSnapshot.entries.empty()) {
        result.success = false;
        result.errorCode = "MissingResultHasherClosure";
        result.errorMessage = "Mapped Sweep result requires a hasher closure";
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

std::shared_ptr<SweepGeometryOperation>
SweepGeometryOperation::decodeFromRequest(const QJsonObject& request,
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
    if (op != QStringLiteral("Part::Sweep")) {
        errorCode = "UnsupportedOperation";
        errorMessage = "Expected Part::Sweep";
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
    if (static_cast<uint32_t>(codec64) != SweepGeometryOperation().codecVersion()) {
        errorCode = "UnsupportedCodecVersion";
        errorMessage = "Unsupported Part::Sweep codecVersion";
        return nullptr;
    }

    bool isSolid = false;
    if (!requireBool(request, "isSolid", isSolid, errorCode, errorMessage)) {
        return nullptr;
    }

    const qint64 maxSection =
        static_cast<qint64>(App::GeometryRequestWorkspace::maxWorkspaceSectionBytes());

    auto loadOperand = [&](const QJsonObject& obj,
                           const char* pathKey,
                           const char* sizeKey,
                           const char* shaKey,
                           FrozenTopoShapeBundle& out) -> bool {
        QString rel;
        if (!requireString(obj, pathKey, rel, errorCode, errorMessage)) {
            return false;
        }
        if (!App::GeometryRequestWorkspace::isTrustedRelativePath(rel)) {
            errorCode = "UntrustedOperandPath";
            errorMessage = std::string(pathKey) + " must be a trusted relative path";
            return false;
        }
        const QString abs = QDir(workspaceDir).filePath(rel);
        if (!QFileInfo::exists(abs) || !QFileInfo(abs).isFile()) {
            errorCode = "MissingOperandArchive";
            errorMessage = std::string(pathKey) + " archive is missing";
            return false;
        }
        qint64 claimedSize = -1;
        if (!requireJsonInt64(obj, sizeKey, claimedSize, errorCode, errorMessage, maxSection)) {
            return false;
        }
        if (claimedSize < 0 || QFileInfo(abs).size() != claimedSize) {
            errorCode = "OperandSizeMismatch";
            errorMessage = std::string(sizeKey) + " does not match on-disk size";
            return false;
        }
        QString claimedSha;
        if (!requireString(obj, shaKey, claimedSha, errorCode, errorMessage)) {
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
        try {
            if (!TopoShapeArchive::readArchive(abs.toStdString(), out) || !out.valid) {
                errorCode = "OperandDecodeFailed";
                errorMessage = std::string(pathKey) + " failed FCG1 decode";
                return false;
            }
        }
        catch (const Standard_Failure& e) {
            errorCode = "OperandDecodeFailed";
            errorMessage = e.GetMessageString() ? e.GetMessageString()
                                                : std::string(pathKey) + " failed FCG1 decode";
            return false;
        }
        catch (const Base::Exception& e) {
            errorCode = "OperandDecodeFailed";
            errorMessage = e.what();
            return false;
        }
        catch (...) {
            errorCode = "OperandDecodeFailed";
            errorMessage = std::string(pathKey) + " failed FCG1 decode";
            return false;
        }
        return true;
    };

    FrozenTopoShapeBundle spine;
    if (!loadOperand(request, "spinePath", "spineSize", "spineSha256", spine)) {
        return nullptr;
    }

    const QJsonValue profilesValue = request.value(QStringLiteral("profiles"));
    if (!request.contains(QStringLiteral("profiles"))) {
        errorCode = "MissingJsonField";
        errorMessage = "profiles is required";
        return nullptr;
    }
    if (!profilesValue.isArray()) {
        errorCode = "WrongJsonType";
        errorMessage = "profiles must be a JSON array";
        return nullptr;
    }
    const QJsonArray profilesArr = profilesValue.toArray();
    if (profilesArr.isEmpty()) {
        errorCode = "EmptyProfiles";
        errorMessage = "profiles must be a non-empty array";
        return nullptr;
    }

    std::unordered_set<QString> seenPaths;
    seenPaths.insert(
        normalizeCanonicalOperandPath(request.value(QStringLiteral("spinePath")).toString()));

    std::vector<FrozenTopoShapeBundle> profiles;
    profiles.reserve(static_cast<size_t>(profilesArr.size()));

    for (const QJsonValue& entry : profilesArr) {
        if (!entry.isObject()) {
            errorCode = "WrongJsonType";
            errorMessage = "each profiles entry must be a JSON object";
            return nullptr;
        }
        const QJsonObject profileObj = entry.toObject();
        FrozenTopoShapeBundle profile;
        if (!loadOperand(profileObj, "path", "size", "sha256", profile)) {
            return nullptr;
        }
        const QString normalized =
            normalizeCanonicalOperandPath(profileObj.value(QStringLiteral("path")).toString());
        if (!seenPaths.insert(normalized).second) {
            errorCode = "DuplicateOperandPath";
            errorMessage = "operand paths must be distinct after normalization";
            return nullptr;
        }
        profiles.push_back(std::move(profile));
    }

    App::StringHasherRef worker(new App::StringHasher);
    FrozenTopoShapeBundle spineBound;
    std::vector<FrozenTopoShapeBundle> profilesBound;
    try {
        if (!materializeSweepOperandsOntoSharedHasher(spine,
                                                      profiles,
                                                      worker,
                                                      spineBound,
                                                      profilesBound,
                                                      errorCode,
                                                      errorMessage)) {
            return nullptr;
        }
    }
    catch (const Standard_Failure& e) {
        errorCode = "OperandDecodeFailed";
        errorMessage = e.GetMessageString() ? e.GetMessageString() : "OCC exception during operand bind";
        return nullptr;
    }
    catch (...) {
        errorCode = "OperandDecodeFailed";
        errorMessage = "Unexpected failure during operand bind";
        return nullptr;
    }

    return std::make_shared<SweepGeometryOperation>(spineBound, profilesBound, isSolid);
}

} // namespace Part
