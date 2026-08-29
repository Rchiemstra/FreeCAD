// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/GeometryArchive.h>

#include <QCryptographicHash>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace App;

namespace App::Internal
{

class GeometryArchiveTestAccess
{
public:
    static void setPrePublishHook(void (*hook)())
    {
        GeometryArchiveCodec::_prePublishTestHook.store(hook, std::memory_order_release);
    }
};

}  // namespace App::Internal

namespace
{

constexpr std::size_t DigestBytes = 32;

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        static std::atomic<std::uint64_t> sequence {1};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("freecad-fcg-test-" + std::to_string(stamp) + "-"
               + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

GeometryArchive archive(GeometryArchiveKind kind = GeometryArchiveKind::Request)
{
    GeometryArchive value;
    value.metadata.kind = kind;
    value.metadata.jobId = 17;
    value.metadata.deadlineEpochMilliseconds = 4'000'000'000'000;
    value.metadata.operationType = "Part.Boolean";
    value.metadata.buildFingerprint = "freecad-test-build";
    value.metadata.inputDigest.assign(64, 'a');
    value.sections = {
        {"brep", {0x01, 0x02, 0x03, 0x04}},
        {"parameters", {'l', 'e', 'f', 't', '+', 'r', 'i', 'g', 'h', 't'}},
    };
    return value;
}

GeometryArchiveExpectation expectation(GeometryArchiveKind kind = GeometryArchiveKind::Request)
{
    GeometryArchiveExpectation value;
    value.kind = kind;
    value.jobId = 17;
    value.operationType = "Part.Boolean";
    value.buildFingerprint = "freecad-test-build";
    value.inputDigest.assign(64, 'a');
    return value;
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.good());
    EXPECT_TRUE(stream.read(reinterpret_cast<char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size())));
    return bytes;
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.good());
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    ASSERT_TRUE(stream.good());
}

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes.at(offset))
        | static_cast<std::uint16_t>(bytes.at(offset + 1)) << 8U;
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes.at(offset++)) << shift;
    }
    return value;
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        bytes.at(offset++) = static_cast<std::uint8_t>(value >> shift);
    }
}

void writeU64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value)
{
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        bytes.at(offset++) = static_cast<std::uint8_t>(value >> shift);
    }
}

struct FirstSectionOffsets
{
    std::size_t name {0};
    std::size_t size {0};
    std::size_t digest {0};
};

FirstSectionOffsets firstSectionOffsets(const std::vector<std::uint8_t>& bytes)
{
    std::size_t offset = 32;
    for (int field = 0; field < 3; ++field) {
        const auto size = readU16(bytes, offset);
        offset += 2 + size;
    }
    const auto sectionCount = readU32(bytes, offset);
    EXPECT_GE(sectionCount, 1U);
    offset += 4;
    const auto nameSize = readU16(bytes, offset);
    EXPECT_EQ(nameSize, 4U);
    offset += 2;
    FirstSectionOffsets result;
    result.name = offset;
    result.size = offset + nameSize;
    result.digest = result.size + 8;
    return result;
}

void rewriteWholeDigest(std::vector<std::uint8_t>& bytes)
{
    ASSERT_GE(bytes.size(), DigestBytes);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<int>(bytes.size() - DigestBytes));
    const auto digest = hash.result();
    ASSERT_EQ(digest.size(), static_cast<int>(DigestBytes));
    std::copy_n(reinterpret_cast<const std::uint8_t*>(digest.constData()),
                DigestBytes,
                bytes.end() - static_cast<std::ptrdiff_t>(DigestBytes));
}

void throwBeforePublish()
{
    throw std::runtime_error("simulated interruption before atomic publication");
}

class HookReset
{
public:
    ~HookReset()
    {
        Internal::GeometryArchiveTestAccess::setPrePublishHook(nullptr);
    }
};

}  // namespace

TEST(GeometryArchiveTest, atomicallyRoundTripsRequestAndResultArchives)
{
    for (const auto kind : {GeometryArchiveKind::Request, GeometryArchiveKind::Result}) {
        TemporaryDirectory directory;
        const auto target = directory.path / "job.fcg";
        const auto original = archive(kind);
        const auto written = GeometryArchiveCodec::writeAtomic(target, original);
        ASSERT_TRUE(written.success) << written.error.code << ": " << written.error.message;
        EXPECT_TRUE(std::filesystem::is_regular_file(target));
        EXPECT_EQ(written.archiveDigest.size(), 64U);

        const auto read = GeometryArchiveCodec::readValidated(target, expectation(kind));
        ASSERT_TRUE(read.success()) << read.error.code << ": " << read.error.message;
        EXPECT_EQ(read.archive->metadata.protocolVersion, GeometryArchiveProtocolVersion);
        EXPECT_EQ(read.archive->metadata.kind, kind);
        EXPECT_EQ(read.archive->metadata.jobId, original.metadata.jobId);
        EXPECT_EQ(read.archive->metadata.operationType, original.metadata.operationType);
        EXPECT_EQ(read.archive->metadata.buildFingerprint, original.metadata.buildFingerprint);
        EXPECT_EQ(read.archive->metadata.inputDigest, original.metadata.inputDigest);
        EXPECT_EQ(read.archive->sections, original.sections);
        EXPECT_EQ(read.archive->archiveDigest, written.archiveDigest);

        const auto duplicate = GeometryArchiveCodec::writeAtomic(target, original);
        EXPECT_FALSE(duplicate.success);
        EXPECT_EQ(duplicate.error.code, "TargetExists");
        EXPECT_EQ(readBytes(target).size(), std::filesystem::file_size(target));
        for (const auto& entry : std::filesystem::directory_iterator(directory.path)) {
            EXPECT_EQ(entry.path().filename(), "job.fcg");
        }
    }
}

TEST(GeometryArchiveTest, rejectsWrongTrustedExpectations)
{
    TemporaryDirectory directory;
    const auto target = directory.path / "job.fcg";
    ASSERT_TRUE(GeometryArchiveCodec::writeAtomic(target, archive()).success);

    auto wrong = expectation();
    wrong.jobId = 18;
    EXPECT_EQ(GeometryArchiveCodec::readValidated(target, wrong).error.code, "JobIdMismatch");
    wrong = expectation();
    wrong.kind = GeometryArchiveKind::Result;
    EXPECT_EQ(GeometryArchiveCodec::readValidated(target, wrong).error.code, "KindMismatch");
    wrong = expectation();
    wrong.operationType = "Part.Fillet";
    EXPECT_EQ(GeometryArchiveCodec::readValidated(target, wrong).error.code,
              "OperationMismatch");
    wrong = expectation();
    wrong.buildFingerprint = "different-build";
    EXPECT_EQ(GeometryArchiveCodec::readValidated(target, wrong).error.code,
              "BuildFingerprintMismatch");
    wrong = expectation();
    wrong.inputDigest.assign(64, 'b');
    EXPECT_EQ(GeometryArchiveCodec::readValidated(target, wrong).error.code,
              "InputDigestMismatch");
}

TEST(GeometryArchiveTest, rejectsCorruptionTruncationAndUnsupportedVersion)
{
    TemporaryDirectory directory;
    const auto target = directory.path / "job.fcg";
    ASSERT_TRUE(GeometryArchiveCodec::writeAtomic(target, archive()).success);
    const auto original = readBytes(target);

    auto corrupted = original;
    corrupted.at(corrupted.size() - DigestBytes - 1) ^= 0x01;
    const auto corruptedPath = directory.path / "corrupted.fcg";
    writeBytes(corruptedPath, corrupted);
    EXPECT_EQ(GeometryArchiveCodec::readValidated(corruptedPath, expectation()).error.code,
              "DigestMismatch");

    auto truncated = original;
    truncated.resize(12);
    const auto truncatedPath = directory.path / "truncated.fcg";
    writeBytes(truncatedPath, truncated);
    EXPECT_EQ(GeometryArchiveCodec::readValidated(truncatedPath, expectation()).error.code,
              "Truncated");

    auto unsupported = original;
    writeU32(unsupported, 8, GeometryArchiveProtocolVersion + 1);
    rewriteWholeDigest(unsupported);
    const auto unsupportedPath = directory.path / "unsupported.fcg";
    writeBytes(unsupportedPath, unsupported);
    EXPECT_EQ(GeometryArchiveCodec::readValidated(unsupportedPath, expectation()).error.code,
              "UnsupportedVersion");
}

TEST(GeometryArchiveTest, validatesSectionTableBeforePayloadUse)
{
    TemporaryDirectory directory;
    const auto target = directory.path / "job.fcg";
    auto singleSection = archive();
    singleSection.sections.resize(1);
    ASSERT_TRUE(GeometryArchiveCodec::writeAtomic(target, singleSection).success);
    const auto original = readBytes(target);
    const auto offsets = firstSectionOffsets(original);

    auto traversal = original;
    const std::string unsafe = "../x";
    std::copy(unsafe.begin(), unsafe.end(), traversal.begin() + offsets.name);
    rewriteWholeDigest(traversal);
    const auto traversalPath = directory.path / "traversal.fcg";
    writeBytes(traversalPath, traversal);
    EXPECT_EQ(GeometryArchiveCodec::readValidated(traversalPath, expectation()).error.code,
              "Traversal");

    auto oversized = original;
    writeU64(oversized, offsets.size, 1024);
    rewriteWholeDigest(oversized);
    const auto oversizedPath = directory.path / "oversized.fcg";
    writeBytes(oversizedPath, oversized);
    GeometryArchiveLimits limits;
    limits.maxSectionBytes = 16;
    EXPECT_EQ(GeometryArchiveCodec::readValidated(oversizedPath, expectation(), limits).error.code,
              "Oversized");

    auto wrongSectionDigest = original;
    wrongSectionDigest.at(offsets.digest) ^= 0x01;
    rewriteWholeDigest(wrongSectionDigest);
    const auto digestPath = directory.path / "section-digest.fcg";
    writeBytes(digestPath, wrongSectionDigest);
    EXPECT_EQ(GeometryArchiveCodec::readValidated(digestPath, expectation()).error.code,
              "DigestMismatch");
}

TEST(GeometryArchiveTest, enforcesWriteAndReadSizeLimits)
{
    TemporaryDirectory directory;
    GeometryArchiveLimits limits;
    limits.maxSectionBytes = 3;
    const auto rejectedTarget = directory.path / "rejected.fcg";
    const auto rejected = GeometryArchiveCodec::writeAtomic(rejectedTarget, archive(), limits);
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.error.code, "Oversized");
    EXPECT_FALSE(std::filesystem::exists(rejectedTarget));

    const auto target = directory.path / "job.fcg";
    ASSERT_TRUE(GeometryArchiveCodec::writeAtomic(target, archive()).success);
    limits = {};
    limits.maxArchiveBytes = std::filesystem::file_size(target) - 1;
    EXPECT_EQ(GeometryArchiveCodec::readValidated(target, expectation(), limits).error.code,
              "Oversized");
}

TEST(GeometryArchiveTest, interruptionLeavesNoPublishedOrTemporaryArtifact)
{
    TemporaryDirectory directory;
    const auto target = directory.path / "job.fcg";
    HookReset reset;
    Internal::GeometryArchiveTestAccess::setPrePublishHook(&throwBeforePublish);
    const auto result = GeometryArchiveCodec::writeAtomic(target, archive());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error.code, "WriteFailed");
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_TRUE(std::filesystem::is_empty(directory.path));
}

TEST(GeometryArchiveTest, exactElementHistoryRoundTripsAndRejectsMalformedData)
{
    GeometryElementHistory original;
    original.generated = {{"Edge1", "Edge7"}, {"Face2", "Face8"}};
    original.modified = {{"Face1", "Face3"}};
    original.deleted = {"Edge4", "Vertex2"};
    GeometryArchiveSection section;
    GeometryArchiveError error;
    ASSERT_TRUE(GeometryArchiveCodec::encodeElementHistory(original, section, error))
        << error.code << ": " << error.message;
    EXPECT_EQ(section.name, "element-history");

    GeometryElementHistory decoded;
    ASSERT_TRUE(GeometryArchiveCodec::decodeElementHistory(section, decoded, error))
        << error.code << ": " << error.message;
    EXPECT_EQ(decoded, original);

    auto duplicate = original;
    duplicate.generated.push_back(duplicate.generated.front());
    EXPECT_FALSE(GeometryArchiveCodec::encodeElementHistory(duplicate, section, error));
    EXPECT_EQ(error.code, "InvalidHistory");

    ASSERT_TRUE(GeometryArchiveCodec::encodeElementHistory(original, section, error));
    section.bytes.pop_back();
    const GeometryElementHistory sentinel = decoded;
    EXPECT_FALSE(GeometryArchiveCodec::decodeElementHistory(section, decoded, error));
    EXPECT_EQ(error.code, "InvalidHistory");
    EXPECT_EQ(decoded, sentinel) << "decode must publish only a fully validated candidate";
}
