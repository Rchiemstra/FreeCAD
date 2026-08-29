// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryArchive.h"

#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

using namespace App;

std::atomic<GeometryArchiveCodec::PrePublishTestHook>
    GeometryArchiveCodec::_prePublishTestHook {nullptr};

namespace
{

constexpr std::array<std::uint8_t, 8> ArchiveMagic {'F', 'C', 'G', 'A', 'R', 'C', 'H', '1'};
constexpr std::array<std::uint8_t, 8> HistoryMagic {'F', 'C', 'G', 'H', 'M', 'A', 'P', '1'};
constexpr std::size_t Sha256Bytes = 32;
constexpr std::size_t MaxOperationTypeBytes = 128;
constexpr std::size_t MaxBuildFingerprintBytes = 128;
constexpr std::size_t Sha256HexBytes = 64;
constexpr std::size_t MaxSectionNameBytes = 64;

void setError(GeometryArchiveError& error, std::string code, std::string message)
{
    error.code = std::move(code);
    error.message = std::move(message);
}

bool checkedAdd(std::uint64_t& total, const std::uint64_t value) noexcept
{
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

bool isPrintableAscii(const std::string& value) noexcept
{
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return character >= 0x20 && character <= 0x7e;
    });
}

bool isSha256Hex(const std::string& value) noexcept
{
    return value.size() == Sha256HexBytes
        && std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

bool isSafeSectionName(const std::string& name) noexcept
{
    if (name.empty() || name.size() > MaxSectionNameBytes
        || name.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](const unsigned char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '.'
            || character == '_' || character == '-';
    });
}

std::array<std::uint8_t, Sha256Bytes> sha256(const std::uint8_t* data,
                                             const std::size_t size)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::size_t offset = 0;
    while (offset < size) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(
            size - offset, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        hash.addData(reinterpret_cast<const char*>(data + offset), chunk);
        offset += static_cast<std::size_t>(chunk);
    }
    const QByteArray digest = hash.result();
    std::array<std::uint8_t, Sha256Bytes> result {};
    std::copy_n(reinterpret_cast<const std::uint8_t*>(digest.constData()),
                result.size(),
                result.begin());
    return result;
}

std::string toHex(const std::array<std::uint8_t, Sha256Bytes>& digest)
{
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = Hex[digest[index] >> 4U];
        result[index * 2 + 1] = Hex[digest[index] & 0x0fU];
    }
    return result;
}

class ByteWriter
{
public:
    void byte(const std::uint8_t value)
    {
        _bytes.push_back(value);
    }

    void u16(const std::uint16_t value)
    {
        byte(static_cast<std::uint8_t>(value));
        byte(static_cast<std::uint8_t>(value >> 8U));
    }

    void u32(const std::uint32_t value)
    {
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            byte(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u64(const std::uint64_t value)
    {
        for (unsigned int shift = 0; shift < 64; shift += 8) {
            byte(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void i64(const std::int64_t value)
    {
        u64(static_cast<std::uint64_t>(value));
    }

    void bytes(const std::uint8_t* data, const std::size_t size)
    {
        if (size == 0) {
            return;
        }
        _bytes.insert(_bytes.end(), data, data + size);
    }

    void bytes(const std::vector<std::uint8_t>& value)
    {
        bytes(value.data(), value.size());
    }

    void string16(const std::string& value)
    {
        if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::length_error("FCG string exceeds uint16 length");
        }
        u16(static_cast<std::uint16_t>(value.size()));
        bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept
    {
        return _bytes;
    }

    [[nodiscard]] std::vector<std::uint8_t> take()
    {
        return std::move(_bytes);
    }

private:
    std::vector<std::uint8_t> _bytes;
};

class ByteReader
{
public:
    explicit ByteReader(const std::vector<std::uint8_t>& bytes, const std::size_t limit)
        : _bytes(bytes)
        , _limit(std::min(limit, bytes.size()))
    {}

    bool byte(std::uint8_t& value)
    {
        if (!available(1)) {
            return false;
        }
        value = _bytes[_offset++];
        return true;
    }

    bool u16(std::uint16_t& value)
    {
        value = 0;
        for (unsigned int shift = 0; shift < 16; shift += 8) {
            std::uint8_t part = 0;
            if (!byte(part)) {
                return false;
            }
            value |= static_cast<std::uint16_t>(part) << shift;
        }
        return true;
    }

    bool u32(std::uint32_t& value)
    {
        value = 0;
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            std::uint8_t part = 0;
            if (!byte(part)) {
                return false;
            }
            value |= static_cast<std::uint32_t>(part) << shift;
        }
        return true;
    }

    bool u64(std::uint64_t& value)
    {
        value = 0;
        for (unsigned int shift = 0; shift < 64; shift += 8) {
            std::uint8_t part = 0;
            if (!byte(part)) {
                return false;
            }
            value |= static_cast<std::uint64_t>(part) << shift;
        }
        return true;
    }

    bool i64(std::int64_t& value)
    {
        std::uint64_t encoded = 0;
        if (!u64(encoded)) {
            return false;
        }
        value = static_cast<std::int64_t>(encoded);
        return true;
    }

    bool bytes(std::uint8_t* destination, const std::size_t size)
    {
        if (!available(size)) {
            return false;
        }
        std::copy_n(_bytes.data() + _offset, size, destination);
        _offset += size;
        return true;
    }

    bool vector(std::vector<std::uint8_t>& destination, const std::size_t size)
    {
        if (!available(size)) {
            return false;
        }
        destination.assign(_bytes.begin() + static_cast<std::ptrdiff_t>(_offset),
                           _bytes.begin() + static_cast<std::ptrdiff_t>(_offset + size));
        _offset += size;
        return true;
    }

    bool string16(std::string& value, const std::size_t maximum)
    {
        std::uint16_t size = 0;
        if (!u16(size) || size > maximum || !available(size)) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(_bytes.data() + _offset), size);
        _offset += size;
        return true;
    }

    [[nodiscard]] std::size_t offset() const noexcept
    {
        return _offset;
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return _limit - _offset;
    }

private:
    [[nodiscard]] bool available(const std::size_t size) const noexcept
    {
        return _offset <= _limit && size <= _limit - _offset;
    }

    const std::vector<std::uint8_t>& _bytes;
    const std::size_t _limit;
    std::size_t _offset {0};
};

bool validateMetadata(const GeometryArchiveMetadata& metadata, GeometryArchiveError& error)
{
    if (metadata.protocolVersion != GeometryArchiveProtocolVersion) {
        setError(error, "UnsupportedVersion", "FCG archive protocol version is unsupported");
        return false;
    }
    if (metadata.kind != GeometryArchiveKind::Request
        && metadata.kind != GeometryArchiveKind::Result) {
        setError(error, "InvalidMetadata", "FCG archive kind is invalid");
        return false;
    }
    if (metadata.jobId == 0 || metadata.policy != PreparationPolicy::IsolatedProcess
        || metadata.deadlineEpochMilliseconds <= 0) {
        setError(error, "InvalidMetadata", "FCG job identity, policy, or deadline is invalid");
        return false;
    }
    if (metadata.operationType.empty()
        || metadata.operationType.size() > MaxOperationTypeBytes
        || !isPrintableAscii(metadata.operationType)
        || metadata.buildFingerprint.empty()
        || metadata.buildFingerprint.size() > MaxBuildFingerprintBytes
        || !isPrintableAscii(metadata.buildFingerprint)
        || !isSha256Hex(metadata.inputDigest)) {
        setError(error, "InvalidMetadata", "FCG operation/build/digest metadata is invalid");
        return false;
    }
    return true;
}

bool validateExpectation(const GeometryArchiveMetadata& metadata,
                         const GeometryArchiveExpectation& expectation,
                         GeometryArchiveError& error)
{
    if (expectation.jobId == 0 || expectation.operationType.empty()
        || expectation.buildFingerprint.empty() || !isSha256Hex(expectation.inputDigest)) {
        setError(error, "InvalidExpectation", "trusted FCG expectation is incomplete");
        return false;
    }
    if (metadata.kind != expectation.kind) {
        setError(error, "KindMismatch", "FCG archive kind does not match launch expectation");
        return false;
    }
    if (metadata.jobId != expectation.jobId) {
        setError(error, "JobIdMismatch", "FCG archive job ID does not match launch expectation");
        return false;
    }
    if (metadata.operationType != expectation.operationType) {
        setError(error, "OperationMismatch", "FCG operation type does not match launch expectation");
        return false;
    }
    if (metadata.buildFingerprint != expectation.buildFingerprint) {
        setError(error,
                 "BuildFingerprintMismatch",
                 "FCG worker build fingerprint does not match launch expectation");
        return false;
    }
    if (metadata.inputDigest != expectation.inputDigest) {
        setError(error, "InputDigestMismatch", "FCG input digest does not match launch expectation");
        return false;
    }
    return true;
}

struct SectionTableEntry
{
    std::string name;
    std::uint64_t size {0};
    std::array<std::uint8_t, Sha256Bytes> digest {};
};

bool encodeArchive(const GeometryArchive& archive,
                   const GeometryArchiveLimits& limits,
                   std::vector<std::uint8_t>& encoded,
                   std::string& digestHex,
                   GeometryArchiveError& error)
{
    if (!validateMetadata(archive.metadata, error)) {
        return false;
    }
    if (archive.sections.size() > limits.maxSections) {
        setError(error, "Oversized", "FCG section count exceeds configured limit");
        return false;
    }
    std::unordered_set<std::string> names;
    std::uint64_t totalSectionBytes = 0;
    std::vector<std::array<std::uint8_t, Sha256Bytes>> digests;
    digests.reserve(archive.sections.size());
    for (const auto& section : archive.sections) {
        if (!isSafeSectionName(section.name)) {
            setError(error, "Traversal", "FCG section name is unsafe");
            return false;
        }
        if (!names.insert(section.name).second) {
            setError(error, "Malformed", "FCG section names must be unique");
            return false;
        }
        if (section.bytes.size() > limits.maxSectionBytes
            || !checkedAdd(totalSectionBytes, section.bytes.size())
            || totalSectionBytes > limits.maxTotalSectionBytes) {
            setError(error, "Oversized", "FCG section bytes exceed configured limit");
            return false;
        }
        digests.push_back(sha256(section.bytes.data(), section.bytes.size()));
    }

    try {
        ByteWriter writer;
        writer.bytes(ArchiveMagic.data(), ArchiveMagic.size());
        writer.u32(archive.metadata.protocolVersion);
        writer.byte(static_cast<std::uint8_t>(archive.metadata.kind));
        writer.byte(static_cast<std::uint8_t>(archive.metadata.policy));
        writer.u16(0);
        writer.u64(archive.metadata.jobId);
        writer.i64(archive.metadata.deadlineEpochMilliseconds);
        writer.string16(archive.metadata.operationType);
        writer.string16(archive.metadata.buildFingerprint);
        writer.string16(archive.metadata.inputDigest);
        writer.u32(static_cast<std::uint32_t>(archive.sections.size()));
        for (std::size_t index = 0; index < archive.sections.size(); ++index) {
            writer.string16(archive.sections[index].name);
            writer.u64(archive.sections[index].bytes.size());
            writer.bytes(digests[index].data(), digests[index].size());
        }
        for (const auto& section : archive.sections) {
            writer.bytes(section.bytes);
        }
        const auto archiveDigest = sha256(writer.data().data(), writer.data().size());
        digestHex = toHex(archiveDigest);
        writer.bytes(archiveDigest.data(), archiveDigest.size());
        encoded = writer.take();
    }
    catch (const std::exception& exception) {
        setError(error, "WriteFailed", exception.what());
        return false;
    }
    if (encoded.size() > limits.maxArchiveBytes) {
        encoded.clear();
        setError(error, "Oversized", "encoded FCG archive exceeds configured limit");
        return false;
    }
    return true;
}

bool validElementName(const std::string& name, const GeometryArchiveLimits& limits)
{
    return !name.empty() && name.size() <= limits.maxElementNameBytes
        && isPrintableAscii(name);
}

bool validateHistory(const GeometryElementHistory& history,
                     const GeometryArchiveLimits& limits,
                     GeometryArchiveError& error)
{
    std::uint64_t count = history.generated.size();
    if (!checkedAdd(count, history.modified.size())
        || !checkedAdd(count, history.deleted.size())
        || count > limits.maxElementMappings
        || count > std::numeric_limits<std::uint32_t>::max()) {
        setError(error, "Oversized", "element history mapping count exceeds configured limit");
        return false;
    }
    for (const auto* mappings : {&history.generated, &history.modified}) {
        std::set<std::pair<std::string, std::string>> unique;
        for (const auto& mapping : *mappings) {
            if (!validElementName(mapping.inputElement, limits)
                || !validElementName(mapping.outputElement, limits)
                || !unique.emplace(mapping.inputElement, mapping.outputElement).second) {
                setError(error, "InvalidHistory", "element history contains invalid or duplicate mapping");
                return false;
            }
        }
    }
    std::set<std::string> deleted;
    for (const auto& element : history.deleted) {
        if (!validElementName(element, limits) || !deleted.insert(element).second) {
            setError(error, "InvalidHistory", "deleted element history is invalid or duplicated");
            return false;
        }
    }
    return true;
}

}  // namespace

GeometryArchiveWriteResult GeometryArchiveCodec::writeAtomic(
    const std::filesystem::path& target,
    const GeometryArchive& archive,
    const GeometryArchiveLimits& limits)
{
    GeometryArchiveWriteResult result;
    result.artifactPath = target;
    std::vector<std::uint8_t> encoded;
    if (!encodeArchive(archive, limits, encoded, result.archiveDigest, result.error)) {
        return result;
    }
    if (target.empty() || target.filename().empty()) {
        setError(result.error, "PublishFailed", "FCG target path is empty");
        return result;
    }
    std::error_code filesystemError;
    if (std::filesystem::exists(target, filesystemError)) {
        setError(result.error, "TargetExists", "FCG target already exists");
        return result;
    }
    if (filesystemError) {
        setError(result.error, "PublishFailed", "cannot inspect FCG target path");
        return result;
    }
    const auto parent = target.has_parent_path() ? target.parent_path()
                                                  : std::filesystem::current_path(filesystemError);
    if (filesystemError || !std::filesystem::is_directory(parent, filesystemError)) {
        setError(result.error, "PublishFailed", "FCG target parent does not exist");
        return result;
    }

    static std::atomic<std::uint64_t> sequence {1};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = target;
    temporary += ".tmp." + std::to_string(stamp) + "."
        + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    struct TemporaryCleanup
    {
        std::filesystem::path path;
        ~TemporaryCleanup()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup {temporary};

    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!stream) {
            setError(result.error, "WriteFailed", "cannot create temporary FCG artifact");
            return result;
        }
        stream.write(reinterpret_cast<const char*>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size()));
        stream.flush();
        if (!stream) {
            setError(result.error, "WriteFailed", "cannot flush temporary FCG artifact");
            return result;
        }
        stream.close();
        if (const auto hook = _prePublishTestHook.load(std::memory_order_acquire)) {
            hook();
        }
    }
    catch (const std::exception& exception) {
        setError(result.error, "WriteFailed", exception.what());
        return result;
    }
    catch (...) {
        setError(result.error, "WriteFailed", "unknown failure before FCG publication");
        return result;
    }

    // A same-directory hard-link publication is atomic and create-only on the
    // supported Windows/Linux filesystems. Unlike POSIX rename, it cannot
    // silently replace a target created after the preflight check.
    std::filesystem::create_hard_link(temporary, target, filesystemError);
    if (filesystemError) {
        setError(result.error, "PublishFailed", filesystemError.message());
        return result;
    }
    result.success = true;
    return result;
}

GeometryArchiveReadResult GeometryArchiveCodec::readValidated(
    const std::filesystem::path& source,
    const GeometryArchiveExpectation& expectation,
    const GeometryArchiveLimits& limits)
{
    GeometryArchiveReadResult result;
    std::error_code filesystemError;
    const auto fileSize = std::filesystem::file_size(source, filesystemError);
    if (filesystemError) {
        setError(result.error, "ReadFailed", "cannot stat FCG archive");
        return result;
    }
    if (fileSize > limits.maxArchiveBytes) {
        setError(result.error, "Oversized", "FCG archive exceeds configured size limit");
        return result;
    }
    if (fileSize < ArchiveMagic.size() + Sha256Bytes) {
        setError(result.error, "Truncated", "FCG archive is too short");
        return result;
    }
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(fileSize));
    std::ifstream stream(source, std::ios::binary);
    if (!stream || !stream.read(reinterpret_cast<char*>(encoded.data()),
                                static_cast<std::streamsize>(encoded.size()))) {
        setError(result.error, "ReadFailed", "cannot read complete FCG archive");
        return result;
    }

    const std::size_t payloadLimit = encoded.size() - Sha256Bytes;
    const auto actualArchiveDigest = sha256(encoded.data(), payloadLimit);
    if (!std::equal(actualArchiveDigest.begin(),
                    actualArchiveDigest.end(),
                    encoded.begin() + static_cast<std::ptrdiff_t>(payloadLimit))) {
        setError(result.error, "DigestMismatch", "FCG whole-archive digest is invalid");
        return result;
    }

    ByteReader reader(encoded, payloadLimit);
    std::array<std::uint8_t, ArchiveMagic.size()> magic {};
    if (!reader.bytes(magic.data(), magic.size()) || magic != ArchiveMagic) {
        setError(result.error, "Malformed", "FCG archive magic is invalid");
        return result;
    }
    GeometryArchive archive;
    std::uint8_t kind = 0;
    std::uint8_t policy = 0;
    std::uint16_t reserved = 0;
    std::uint64_t jobId = 0;
    if (!reader.u32(archive.metadata.protocolVersion) || !reader.byte(kind)
        || !reader.byte(policy) || !reader.u16(reserved) || !reader.u64(jobId)
        || !reader.i64(archive.metadata.deadlineEpochMilliseconds)
        || !reader.string16(archive.metadata.operationType, MaxOperationTypeBytes)
        || !reader.string16(archive.metadata.buildFingerprint, MaxBuildFingerprintBytes)
        || !reader.string16(archive.metadata.inputDigest, Sha256HexBytes)) {
        setError(result.error, "Truncated", "FCG metadata is truncated or oversized");
        return result;
    }
    archive.metadata.kind = static_cast<GeometryArchiveKind>(kind);
    archive.metadata.policy = static_cast<PreparationPolicy>(policy);
    archive.metadata.jobId = jobId;
    if (reserved != 0 || !validateMetadata(archive.metadata, result.error)
        || !validateExpectation(archive.metadata, expectation, result.error)) {
        if (result.error.code.empty()) {
            setError(result.error, "Malformed", "FCG reserved metadata is nonzero");
        }
        return result;
    }

    std::uint32_t sectionCount = 0;
    if (!reader.u32(sectionCount)) {
        setError(result.error, "Truncated", "FCG section count is truncated");
        return result;
    }
    if (sectionCount > limits.maxSections) {
        setError(result.error, "Oversized", "FCG section count exceeds configured limit");
        return result;
    }
    std::vector<SectionTableEntry> table;
    table.reserve(sectionCount);
    std::unordered_set<std::string> names;
    std::uint64_t totalSectionBytes = 0;
    for (std::uint32_t index = 0; index < sectionCount; ++index) {
        SectionTableEntry entry;
        if (!reader.string16(entry.name, MaxSectionNameBytes) || !reader.u64(entry.size)
            || !reader.bytes(entry.digest.data(), entry.digest.size())) {
            setError(result.error, "Truncated", "FCG section table is truncated");
            return result;
        }
        if (!isSafeSectionName(entry.name)) {
            setError(result.error, "Traversal", "FCG section name is unsafe");
            return result;
        }
        if (!names.insert(entry.name).second) {
            setError(result.error, "Malformed", "FCG section names are duplicated");
            return result;
        }
        if (entry.size > limits.maxSectionBytes
            || !checkedAdd(totalSectionBytes, entry.size)
            || totalSectionBytes > limits.maxTotalSectionBytes) {
            setError(result.error, "Oversized", "FCG declared section bytes exceed configured limit");
            return result;
        }
        table.push_back(std::move(entry));
    }
    if (totalSectionBytes != reader.remaining()) {
        setError(result.error, "Truncated", "FCG payload size does not match section table");
        return result;
    }
    archive.sections.reserve(table.size());
    for (const auto& entry : table) {
        GeometryArchiveSection section;
        section.name = entry.name;
        if (!reader.vector(section.bytes, static_cast<std::size_t>(entry.size))) {
            setError(result.error, "Truncated", "FCG section payload is truncated");
            return result;
        }
        if (sha256(section.bytes.data(), section.bytes.size()) != entry.digest) {
            setError(result.error, "DigestMismatch", "FCG section digest is invalid");
            return result;
        }
        archive.sections.push_back(std::move(section));
    }
    if (reader.remaining() != 0) {
        setError(result.error, "Malformed", "FCG archive has trailing payload bytes");
        return result;
    }
    archive.archiveDigest = toHex(actualArchiveDigest);
    result.archive = std::move(archive);
    return result;
}

bool GeometryArchiveCodec::encodeElementHistory(
    const GeometryElementHistory& history,
    GeometryArchiveSection& section,
    GeometryArchiveError& error,
    const GeometryArchiveLimits& limits)
{
    error = {};
    if (!validateHistory(history, limits, error)) {
        return false;
    }
    try {
        ByteWriter writer;
        writer.bytes(HistoryMagic.data(), HistoryMagic.size());
        const auto writeMappings = [&writer](const auto& mappings) {
            writer.u32(static_cast<std::uint32_t>(mappings.size()));
            for (const auto& mapping : mappings) {
                writer.string16(mapping.inputElement);
                writer.string16(mapping.outputElement);
            }
        };
        writeMappings(history.generated);
        writeMappings(history.modified);
        writer.u32(static_cast<std::uint32_t>(history.deleted.size()));
        for (const auto& element : history.deleted) {
            writer.string16(element);
        }
        section.name = "element-history";
        section.bytes = writer.take();
    }
    catch (const std::exception& exception) {
        setError(error, "InvalidHistory", exception.what());
        return false;
    }
    if (section.bytes.size() > limits.maxSectionBytes) {
        section = {};
        setError(error, "Oversized", "encoded element history exceeds section limit");
        return false;
    }
    return true;
}

bool GeometryArchiveCodec::decodeElementHistory(
    const GeometryArchiveSection& section,
    GeometryElementHistory& history,
    GeometryArchiveError& error,
    const GeometryArchiveLimits& limits)
{
    error = {};
    GeometryElementHistory candidate;
    if (section.name != "element-history" || section.bytes.size() > limits.maxSectionBytes) {
        setError(error, "InvalidHistory", "element history section is missing or oversized");
        return false;
    }
    ByteReader reader(section.bytes, section.bytes.size());
    std::array<std::uint8_t, HistoryMagic.size()> magic {};
    if (!reader.bytes(magic.data(), magic.size()) || magic != HistoryMagic) {
        setError(error, "InvalidHistory", "element history magic is invalid");
        return false;
    }
    std::uint64_t totalMappings = 0;
    const auto readMappings = [&](auto& mappings) {
        std::uint32_t count = 0;
        if (!reader.u32(count) || !checkedAdd(totalMappings, count)
            || totalMappings > limits.maxElementMappings) {
            return false;
        }
        mappings.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            GeometryElementMapping mapping;
            if (!reader.string16(mapping.inputElement, limits.maxElementNameBytes)
                || !reader.string16(mapping.outputElement, limits.maxElementNameBytes)) {
                return false;
            }
            mappings.push_back(std::move(mapping));
        }
        return true;
    };
    if (!readMappings(candidate.generated) || !readMappings(candidate.modified)) {
        setError(error, "InvalidHistory", "element mapping payload is truncated or oversized");
        return false;
    }
    std::uint32_t deletedCount = 0;
    if (!reader.u32(deletedCount) || !checkedAdd(totalMappings, deletedCount)
        || totalMappings > limits.maxElementMappings) {
        setError(error, "InvalidHistory", "deleted element count is truncated or oversized");
        return false;
    }
    candidate.deleted.reserve(deletedCount);
    for (std::uint32_t index = 0; index < deletedCount; ++index) {
        std::string element;
        if (!reader.string16(element, limits.maxElementNameBytes)) {
            setError(error, "InvalidHistory", "deleted element payload is truncated");
            return false;
        }
        candidate.deleted.push_back(std::move(element));
    }
    if (reader.remaining() != 0 || !validateHistory(candidate, limits, error)) {
        if (error.code.empty()) {
            setError(error, "InvalidHistory", "element history has trailing bytes");
        }
        return false;
    }
    history = std::move(candidate);
    return true;
}
