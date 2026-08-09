// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 Joao Matos
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1            *
 *   of the License, or (at your option) any later version.                   *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful,               *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty              *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
 *   See the GNU Lesser General Public License for more details.              *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see https://www.gnu.org/licenses     *
 *                                                                            *
 ******************************************************************************/

#include <charconv>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/scope_exit.hpp>

#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>
#include <Base/Writer.h>
#include <Base/XMLTools.h>

#include "Application.h"
#include "Document.h"
#include "DocumentCollaborationService.h"
#include "RecoverySnapshot.h"

namespace
{

class ScopedSaveThumbnailPreference
{
public:
    ScopedSaveThumbnailPreference(ParameterGrp::handle params, bool enabled)
        : params(std::move(params))
        , originalValue(this->params->GetBool("SaveThumbnail", true))
    {
        this->params->SetBool("SaveThumbnail", enabled);
    }

    ScopedSaveThumbnailPreference(const ScopedSaveThumbnailPreference&) = delete;
    ScopedSaveThumbnailPreference& operator=(const ScopedSaveThumbnailPreference&) = delete;

    ~ScopedSaveThumbnailPreference()
    {
        params->SetBool("SaveThumbnail", originalValue);
    }

private:
    ParameterGrp::handle params;
    bool originalValue;
};

std::string recoveryDirectoryFor(const App::Document& doc)
{
    std::string dirName = doc.TransientDir.getValue();
    dirName += "/fc_recovery_files";
    return dirName;
}

std::string recoveryMetadataFileFor(const App::Document& doc)
{
    std::string fileName = doc.TransientDir.getValue();
    fileName += "/fc_recovery_file.xml";
    return fileName;
}

std::string compressedRecoveryFileFor(const App::Document& doc)
{
    std::string fileName = doc.TransientDir.getValue();
    fileName += "/fc_recovery_file.fcstd";
    return fileName;
}

void deleteFileIfPresent(const std::string& fileName, const char* diagnostic)
{
    Base::FileInfo fileInfo(fileName);
    if (fileInfo.exists() && !fileInfo.deleteFile()) {
        throw Base::FileException(diagnostic, fileInfo);
    }
}

void deleteDirectoryIfPresent(const std::string& directoryName, const char* diagnostic)
{
    Base::FileInfo directoryInfo(directoryName);
    if (!directoryInfo.exists()) {
        return;
    }
    if (!directoryInfo.isDir() || !directoryInfo.deleteDirectoryRecursive()) {
        throw Base::FileException(diagnostic, directoryInfo);
    }
}

void invalidateRecoveryMetadata(const App::Document& doc)
{
    deleteFileIfPresent(recoveryMetadataFileFor(doc),
                        "Failed to invalidate auto-recovery metadata file");
}

void writeRecoveryMetadataFile(const App::Document& doc,
                               const App::RecoverySnapshotMetadata& metadata)
{
    const std::string fileName = recoveryMetadataFileFor(doc);
    const std::string stagedFileName = fileName + ".tmp";
    deleteFileIfPresent(stagedFileName, "Failed to remove staged auto-recovery metadata file");

    Base::FileInfo stagedFileInfo(stagedFileName);
    Base::ofstream file(stagedFileInfo, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        throw Base::FileException("Failed to open staged auto-recovery metadata file",
                                  stagedFileInfo);
    }

    file << App::serializeRecoverySnapshotMetadata(doc.Label.getValue(),
                                                   doc.FileName.getValue(),
                                                   metadata);
    file.flush();
    if (!file) {
        file.close();
        static_cast<void>(stagedFileInfo.deleteFile());
        throw Base::FileException("Failed to write staged auto-recovery metadata file",
                                  stagedFileInfo);
    }
    file.close();
    if (!file) {
        static_cast<void>(stagedFileInfo.deleteFile());
        throw Base::FileException("Failed to close staged auto-recovery metadata file",
                                  stagedFileInfo);
    }

    Base::FileInfo targetFileInfo(fileName);
    if (targetFileInfo.exists() && !targetFileInfo.deleteFile()) {
        static_cast<void>(stagedFileInfo.deleteFile());
        throw Base::FileException("Failed to replace auto-recovery metadata file",
                                  targetFileInfo);
    }
    if (!stagedFileInfo.renameFile(fileName.c_str())) {
        static_cast<void>(stagedFileInfo.deleteFile());
        throw Base::FileException("Failed to publish auto-recovery metadata file",
                                  targetFileInfo);
    }
}

template<typename WriterT>
void writeRecoverySnapshotContents(const App::Document& doc, WriterT& writer)
{
    writer.putNextEntry("Document.xml");
    doc.Save(writer);

    // Special handling for Gui document state.
    doc.signalSaveDocument(writer);
    writer.writeFiles();

    if (writer.hasErrors()) {
        std::stringstream message;
        message << "Failed to write all data to auto-recovery output ";
        message << writer.getErrors().front();
        throw Base::FileException(message.str().c_str());
    }
}

void writeUncompressedRecoverySnapshot(const App::Document& doc, bool saveBinaryBrep)
{
    const std::string directoryName = recoveryDirectoryFor(doc);
    const std::string stagedDirectoryName = directoryName + ".tmp";
    deleteDirectoryIfPresent(stagedDirectoryName,
                             "Failed to remove staged auto-recovery directory");

    Base::FileInfo stagedDirectory(stagedDirectoryName);
    if (!stagedDirectory.createDirectory()) {
        throw Base::FileException("Failed to create staged auto-recovery directory",
                                  stagedDirectory);
    }

    try {
        Base::FileWriter writer(stagedDirectoryName.c_str());
        if (saveBinaryBrep) {
            writer.setMode("BinaryBrep");
        }

        writeRecoverySnapshotContents(doc, writer);
    }
    catch (...) {
        static_cast<void>(stagedDirectory.deleteDirectoryRecursive());
        throw;
    }

    deleteDirectoryIfPresent(directoryName,
                             "Failed to replace auto-recovery directory");
    if (!stagedDirectory.renameFile(directoryName.c_str())) {
        static_cast<void>(stagedDirectory.deleteDirectoryRecursive());
        throw Base::FileException("Failed to publish auto-recovery directory",
                                  Base::FileInfo(directoryName));
    }
}

void writeCompressedRecoverySnapshot(const App::Document& doc, bool saveBinaryBrep)
{
    const std::string fileName = compressedRecoveryFileFor(doc);
    const std::string stagedFileName = fileName + ".tmp";
    deleteFileIfPresent(stagedFileName, "Failed to remove staged auto-recovery archive");

    Base::FileInfo stagedFileInfo(stagedFileName);
    Base::ofstream file(stagedFileInfo, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        throw Base::FileException("Failed to open staged auto-recovery archive", stagedFileInfo);
    }

    try {
        {
            Base::ZipWriter writer(file);
            if (saveBinaryBrep) {
                writer.setMode("BinaryBrep");
            }

            writer.setComment("AutoRecovery file");
            writer.setLevel(1);  // Prefer lower latency over compression ratio for autosave.
            writeRecoverySnapshotContents(doc, writer);
        }
        file.flush();
        if (!file) {
            throw Base::FileException("Failed to finalize staged auto-recovery archive",
                                      stagedFileInfo);
        }
        file.close();
        if (!file) {
            throw Base::FileException("Failed to close staged auto-recovery archive",
                                      stagedFileInfo);
        }
    }
    catch (...) {
        file.close();
        static_cast<void>(stagedFileInfo.deleteFile());
        throw;
    }

    Base::FileInfo targetFileInfo(fileName);
    if (targetFileInfo.exists() && !targetFileInfo.deleteFile()) {
        static_cast<void>(stagedFileInfo.deleteFile());
        throw Base::FileException("Failed to replace auto-recovery archive", targetFileInfo);
    }
    if (!stagedFileInfo.renameFile(fileName.c_str())) {
        static_cast<void>(stagedFileInfo.deleteFile());
        throw Base::FileException("Failed to publish auto-recovery archive", targetFileInfo);
    }
}

template<typename Unsigned>
std::optional<Unsigned> parseExactDecimal(std::string_view text) noexcept
{
    static_assert(std::is_unsigned_v<Unsigned>);
    if (text.empty()) {
        return std::nullopt;
    }

    Unsigned value {};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> singleTextChild(const boost::property_tree::ptree& parent,
                                           std::string_view childName)
{
    std::optional<std::string> value;
    for (const auto& child : parent) {
        if (child.first != childName) {
            continue;
        }
        if (value || !child.second.empty()) {
            return std::nullopt;
        }
        value = child.second.get_value<std::string>();
    }
    return value;
}

}  // namespace

namespace App
{

bool RecoverySnapshotMetadata::valid() const noexcept
{
    return schemaVersion == CurrentSchemaVersion && sourceDocumentInstanceId != 0
        && sourceLifecycleEpoch != 0;
}

RecoverySnapshotMetadata captureRecoverySnapshotMetadata(const Document& doc)
{
    const auto sourceIdentity = doc.collaborationIdentity();
    const auto revisionIdentity = doc.collaborationRevisions().documentIdentity();
    if (sourceIdentity.state != DocumentLifecycleState::Live || !revisionIdentity
        || revisionIdentity->documentInstanceId != sourceIdentity.instanceId
        || revisionIdentity->lifecycleEpoch != sourceIdentity.lifecycleEpoch) {
        throw Base::RuntimeError(
            "Cannot capture recovery provenance from an unbound or changing revision stream"
        );
    }

    const auto publications = doc.collaborationRevisions().pollPublications(
        {sourceIdentity.instanceId, sourceIdentity.lifecycleEpoch, 0},
        0
    );
    if (publications.status != DocumentRevisionCursorStatus::Valid
        || publications.currentIdentity != *revisionIdentity) {
        throw Base::RuntimeError(
            "Cannot capture recovery provenance across a lifecycle boundary"
        );
    }

    return {RecoverySnapshotMetadata::CurrentSchemaVersion,
            sourceIdentity.instanceId,
            sourceIdentity.lifecycleEpoch,
            publications.latestSequence};
}

std::string serializeRecoverySnapshotMetadata(
    std::string_view label,
    std::string_view fileName,
    const std::optional<RecoverySnapshotMetadata>& collaborationMetadata)
{
    if (collaborationMetadata && !collaborationMetadata->valid()) {
        throw std::invalid_argument("invalid collaboration recovery metadata");
    }

    const auto escapedLabel = XMLTools::escapeXml(std::string(label));
    const auto escapedFileName = XMLTools::escapeXml(std::string(fileName));
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "<?xml version='1.0' encoding='utf-8'?>\n"
           << "<AutoRecovery SchemaVersion=\"1\">\n"
           << "  <Status>Created</Status>\n"
           << "  <Label>" << escapedLabel << "</Label>\n"
           << "  <FileName>" << escapedFileName << "</FileName>\n";
    if (collaborationMetadata) {
        output << "  <CollaborationProvenance SchemaVersion=\""
               << collaborationMetadata->schemaVersion << "\">\n"
               << "    <SourceDocumentInstanceId>"
               << collaborationMetadata->sourceDocumentInstanceId
               << "</SourceDocumentInstanceId>\n"
               << "    <SourceLifecycleEpoch>"
               << collaborationMetadata->sourceLifecycleEpoch
               << "</SourceLifecycleEpoch>\n"
               << "    <LatestPublicationSequence>"
               << collaborationMetadata->latestPublicationSequence
               << "</LatestPublicationSequence>\n"
               << "  </CollaborationProvenance>\n";
    }
    output << "</AutoRecovery>\n";
    return output.str();
}

std::optional<RecoverySnapshotMetadata>
parseRecoverySnapshotMetadata(std::string_view metadataXml) noexcept
{
    try {
        std::istringstream input {std::string(metadataXml)};
        boost::property_tree::ptree tree;
        boost::property_tree::read_xml(
            input,
            tree,
            boost::property_tree::xml_parser::trim_whitespace
                | boost::property_tree::xml_parser::no_comments
        );

        if (tree.size() != 1 || tree.begin()->first != "AutoRecovery") {
            return std::nullopt;
        }

        const auto& root = tree.begin()->second;
        const boost::property_tree::ptree* provenance = nullptr;
        for (const auto& child : root) {
            if (child.first == "CollaborationProvenance") {
                if (provenance) {
                    return std::nullopt;
                }
                provenance = &child.second;
            }
        }
        if (!provenance) {
            return std::nullopt;
        }
        if (!provenance->data().empty()) {
            return std::nullopt;
        }
        for (const auto& child : *provenance) {
            if (child.first != "<xmlattr>" && child.first != "SourceDocumentInstanceId"
                && child.first != "SourceLifecycleEpoch"
                && child.first != "LatestPublicationSequence") {
                return std::nullopt;
            }
        }

        const auto attributes = provenance->get_child_optional("<xmlattr>");
        if (!attributes || attributes->size() != 1
            || attributes->begin()->first != "SchemaVersion") {
            return std::nullopt;
        }
        const auto schemaText = provenance->get_optional<std::string>(
            "<xmlattr>.SchemaVersion"
        );
        const auto instanceText = singleTextChild(*provenance,
                                                  "SourceDocumentInstanceId");
        const auto epochText = singleTextChild(*provenance, "SourceLifecycleEpoch");
        const auto sequenceText = singleTextChild(*provenance,
                                                  "LatestPublicationSequence");
        if (!schemaText || !instanceText || !epochText || !sequenceText) {
            return std::nullopt;
        }

        const auto schema = parseExactDecimal<std::uint32_t>(*schemaText);
        const auto instance = parseExactDecimal<DocumentInstanceId>(*instanceText);
        const auto epoch = parseExactDecimal<DocumentLifecycleEpoch>(*epochText);
        const auto sequence = parseExactDecimal<DocumentPublicationSequence>(*sequenceText);
        if (!schema || !instance || !epoch || !sequence) {
            return std::nullopt;
        }

        RecoverySnapshotMetadata metadata {*schema, *instance, *epoch, *sequence};
        return metadata.valid() ? std::optional {metadata} : std::nullopt;
    }
    catch (...) {
        return std::nullopt;
    }
}

bool writeRecoverySnapshotToTransientDir(const Document& doc,
                                         const RecoverySnapshotSaveOptions& options)
{
    auto& mutableDocument = const_cast<Document&>(doc);
    auto lifecyclePin = mutableDocument.collaborationService().pinDocumentAccess();
    if (!lifecyclePin) {
        throw Base::RuntimeError(
            "Recovery snapshots cannot start while document close is sealed");
    }
    if (!doc.isCollaborationOwnerThread()) {
        throw Base::RuntimeError("Recovery snapshots must be written on the document owner thread");
    }

    std::unique_lock<std::recursive_mutex> commitLock(
        mutableDocument.collaborationCommitMutex()
    );
    const auto identity = doc.collaborationIdentity();
    if (!doc.canWriteRecoverySnapshot() || doc.collaborationStableReadBlocked()
        || doc.collaborationNotificationsReplaying()
        || doc.collaborationLifecycleMutationBlocked()
        || identity.state != DocumentLifecycleState::Live) {
        std::stringstream message;
        message << "Document '" << doc.Label.getValue()
                << "' is not in a stable App state for recovery write";
        throw Base::RuntimeError(message.str().c_str());
    }

    // Keep lifecycle teardown and administrative epoch changes out for the
    // full serialization interval. The mutex is recursive on the owner thread,
    // so re-entrant close must also observe this explicit admission pin.
    mutableDocument.beginCollaborationStableReadCapture();
    BOOST_SCOPE_EXIT_ALL(&) {
        mutableDocument.finishCollaborationStableReadCapture();
    };

    const auto metadata = captureRecoverySnapshotMetadata(doc);

    auto params = GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document"
    );
    ScopedSaveThumbnailPreference saveThumbnailPreference(params, options.saveThumbnail);

    // A pre-existing Created marker must not survive a failed replacement.
    // The archive becomes authoritative first; metadata is published last.
    invalidateRecoveryMetadata(doc);
    try {
        if (!options.compressed) {
            writeUncompressedRecoverySnapshot(doc, options.saveBinaryBrep);
            deleteFileIfPresent(compressedRecoveryFileFor(doc),
                                "Failed to retire compressed auto-recovery archive");
        }
        else {
            writeCompressedRecoverySnapshot(doc, options.saveBinaryBrep);
            deleteDirectoryIfPresent(recoveryDirectoryFor(doc),
                                     "Failed to retire uncompressed auto-recovery directory");
        }

        const auto identityAfterSerialization = doc.collaborationIdentity();
        if (!doc.canWriteRecoverySnapshot() || doc.collaborationStableReadBlocked()
            || doc.collaborationNotificationsReplaying()
            || identityAfterSerialization.state != DocumentLifecycleState::Live
            || captureRecoverySnapshotMetadata(doc) != metadata) {
            throw Base::RuntimeError(
                "Document changed or became unstable during recovery serialization"
            );
        }
        writeRecoveryMetadataFile(doc, metadata);
    }
    catch (...) {
        try {
            invalidateRecoveryMetadata(doc);
        }
        catch (...) {
            // Preserve the original serialization error. The metadata writer
            // stages and renames, so no new Created marker was published.
        }
        throw;
    }

    return true;
}

}  // namespace App
