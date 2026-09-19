// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionStorage.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace Inspection::Photo
{
namespace
{

QString native(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

}  // namespace

ValidationResult writePhotoInspectionFileAtomically(
    const std::string& target,
    const std::string& content,
    const AtomicWriteOptions& options
)
{
    if (target.empty() || options.allowedRoot.empty() || options.maximumBytes == 0
        || content.size() > options.maximumBytes) {
        return ValidationResult::failure(
            content.size() > options.maximumBytes ? DiagnosticCode::ResourceLimit
                                                  : DiagnosticCode::InvalidSchema,
            "atomic-write target, root, or content limit is invalid"
        );
    }
    const QFileInfo rootInfo(native(options.allowedRoot));
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || !rootInfo.isDir()) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidSchema,
            "allowed output root does not exist"
        );
    }

    const QFileInfo targetInfo(native(target));
    if (!targetInfo.isAbsolute() || targetInfo.fileName().isEmpty()) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidSchema,
            "output target must be an absolute file path"
        );
    }
    const QString canonicalParent = QFileInfo(targetInfo.absolutePath()).canonicalFilePath();
    const QString rootPrefix = QDir::cleanPath(canonicalRoot) + QDir::separator();
    const QString cleanParent = QDir::cleanPath(canonicalParent);
    if (canonicalParent.isEmpty()
        || (cleanParent != QDir::cleanPath(canonicalRoot) && !cleanParent.startsWith(rootPrefix))) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidSchema,
            "output target escapes the allowed root"
        );
    }
    if (targetInfo.exists() && targetInfo.isSymLink()) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidSchema,
            "symbolic-link output targets are not allowed"
        );
    }
    if (targetInfo.exists() && !options.replaceExisting) {
        return ValidationResult::failure(DiagnosticCode::InvalidSchema, "output target already exists");
    }

    QSaveFile file(targetInfo.absoluteFilePath());
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return ValidationResult::failure(DiagnosticCode::NumericalFailure, "cannot open atomic output");
    }
    const QByteArray bytes(content.data(), static_cast<qsizetype>(content.size()));
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return ValidationResult::failure(
            DiagnosticCode::NumericalFailure,
            "cannot write complete atomic output"
        );
    }
    if (!file.commit()) {
        return ValidationResult::failure(DiagnosticCode::NumericalFailure, "cannot commit atomic output");
    }
    return ValidationResult::success();
}

}  // namespace Inspection::Photo
