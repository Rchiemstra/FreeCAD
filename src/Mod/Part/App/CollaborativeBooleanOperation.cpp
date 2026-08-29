// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborativeBooleanOperation.h"

#include "FuzzyHelper.h"
#include "PartFeature.h"
#include "TopoShapeOpCode.h"

#include <Standard_Version.hxx>

#include <App/CollaborativeOperation.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/GeometryWorkerOperationRegistry.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>

#include <BRep_Builder.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepTools_ShapeSet.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <gp_Trsf.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#if OCC_VERSION_HEX >= 0x070600
#include <TopTools_FormatVersion.hxx>
#endif
#include <TopTools_ListOfShape.hxx>

#include <QCryptographicHash>
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
#include <QByteArrayView>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <stop_token>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

enum class BooleanKind
{
    Cut,
    Fuse,
    Common
};

struct ObjectReference
{
    std::string name;
    std::string identity;
};

class StopTokenProgressIndicator final: public Message_ProgressIndicator
{
public:
    StopTokenProgressIndicator(
        std::stop_token stopToken,
        Part::Internal::CollaborativeBooleanTaskProbe* probe = nullptr)
        : _stopToken(std::move(stopToken))
        , _probe(probe)
    {}

    void Show(const Message_ProgressScope&, Standard_Boolean) override
    {}

    Standard_Boolean UserBreak() override
    {
        if (_probe) {
            _probe->progressCallbacks.fetch_add(1, std::memory_order_relaxed);
        }
        const bool cancelled = _stopToken.stop_requested();
        if (cancelled && _probe) {
            _probe->cancellationObserved.store(true, std::memory_order_relaxed);
        }
        return cancelled ? Standard_True : Standard_False;
    }

private:
    const std::stop_token _stopToken;
    Part::Internal::CollaborativeBooleanTaskProbe* const _probe;
};

using ShapeDigest = Part::Internal::CollaborativeBooleanDigest;

class DigestStreamBuffer final: public std::streambuf
{
public:
    void append(const char* data, std::size_t size)
    {
        while (size > 0) {
            const std::size_t available = _digestBuffer.size() - _digestBufferSize;
            const std::size_t count = std::min(available, size);
            std::copy_n(data, count, _digestBuffer.data() + _digestBufferSize);
            _digestBufferSize += count;
            _bytesWritten += count;
            data += count;
            size -= count;
            if (_digestBufferSize == _digestBuffer.size()) {
                flushDigestBuffer();
            }
        }
    }

    template<std::size_t Size>
    void append(const char (&text)[Size])
    {
        append(text, Size - 1);
    }

    [[nodiscard]] ShapeDigest digest()
    {
        flushDigestBuffer();
        const QByteArray value = _hash.result();
        if (value.size()
            != static_cast<decltype(value.size())>(ShapeDigest {}.size())) {
            throw std::runtime_error("Part Boolean result digest failed");
        }
        ShapeDigest result {};
        std::transform(value.cbegin(), value.cend(), result.begin(), [](char byte) {
            return static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
        });
        return result;
    }

    [[nodiscard]] std::size_t bytesWritten() const noexcept
    {
        return _bytesWritten;
    }

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override
    {
        if (size > 0) {
            append(data, static_cast<std::size_t>(size));
        }
        return size;
    }

    int_type overflow(int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        const char byte = traits_type::to_char_type(character);
        append(&byte, 1);
        return character;
    }

private:
    void flushDigestBuffer()
    {
        if (_digestBufferSize == 0) {
            return;
        }
#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
        _hash.addData(_digestBuffer.data(), static_cast<int>(_digestBufferSize));
#else
        _hash.addData(QByteArrayView(_digestBuffer.data(),
                                    static_cast<qsizetype>(_digestBufferSize)));
#endif
        _digestBufferSize = 0;
    }

    QCryptographicHash _hash {QCryptographicHash::Sha256};
    std::array<char, 4096> _digestBuffer {};
    std::size_t _digestBufferSize {0};
    std::size_t _bytesWritten {0};
};

bool appendInteger(DigestStreamBuffer& digest, Standard_Integer value)
{
    std::array<char, 32> encoded {};
    const auto result = std::to_chars(encoded.data(), encoded.data() + encoded.size(), value);
    if (result.ec != std::errc {}) {
        return false;
    }
    digest.append(encoded.data(), static_cast<std::size_t>(result.ptr - encoded.data()));
    return true;
}

bool appendReal(DigestStreamBuffer& digest, Standard_Real value)
{
    if (!std::isfinite(value)) {
        return false;
    }
    // Signed zero has one geometric meaning and must not reintroduce a
    // representation distinction after a location chain is collapsed.
    const Standard_Real normalized = value == 0.0 ? 0.0 : value;
    std::array<char, 64> encoded {};
    const auto result = std::to_chars(encoded.data(),
                                      encoded.data() + encoded.size(),
                                      normalized,
                                      std::chars_format::general,
                                      std::numeric_limits<Standard_Real>::max_digits10);
    if (result.ec != std::errc {}) {
        return false;
    }
    digest.append(encoded.data(), static_cast<std::size_t>(result.ptr - encoded.data()));
    return true;
}

bool appendCanonicalLocation(DigestStreamBuffer& digest,
                             const TopTools_LocationSet& locations,
                             Standard_Integer index)
{
    if (index < 0) {
        return false;
    }
    const TopLoc_Location& location = locations.Location(index);
    if (locations.Index(location) != index) {
        return false;
    }
    const gp_Trsf transform = location.Transformation();
    digest.append("{");
    for (Standard_Integer row = 1; row <= 3; ++row) {
        for (Standard_Integer column = 1; column <= 4; ++column) {
            if (row != 1 || column != 1) {
                digest.append(",");
            }
            if (!appendReal(digest, transform.Value(row, column))) {
                return false;
            }
        }
    }
    digest.append("}");
    return true;
}

struct TokenSpan
{
    std::size_t begin {0};
    std::size_t end {0};
};

bool parseLocationIndex(const char* line,
                        TokenSpan token,
                        Standard_Integer& result)
{
    if (token.begin == token.end) {
        return false;
    }
    Standard_Integer parsed = 0;
    for (std::size_t index = token.begin; index < token.end; ++index) {
        if (line[index] < '0' || line[index] > '9') {
            return false;
        }
        const Standard_Integer digit = line[index] - '0';
        if (parsed > (std::numeric_limits<Standard_Integer>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    result = parsed;
    return true;
}

class GeometryStreamBuffer final: public std::streambuf
{
public:
    GeometryStreamBuffer(DigestStreamBuffer& digest,
                         const BRepTools_ShapeSet& shapeSet,
                         TopAbs_ShapeEnum type)
        : _digest(digest)
        , _shapeSet(shapeSet)
        , _type(type)
    {}

    [[nodiscard]] bool finish() const noexcept
    {
        if (_failed || _lineSize != 0) {
            return false;
        }
        switch (_type) {
            case TopAbs_VERTEX:
                return _lineNumber >= 3 && _ended;
            case TopAbs_EDGE:
                return _lineNumber >= 2 && _ended;
            case TopAbs_FACE:
                return _lineNumber == 1;
            default:
                return _lineNumber == 0;
        }
    }

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override
    {
        for (std::streamsize index = 0; index < size; ++index) {
            if (!consume(data[index])) {
                return index;
            }
        }
        return size;
    }

    int_type overflow(int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        return consume(traits_type::to_char_type(character))
            ? character
            : traits_type::eof();
    }

private:
    bool consume(char byte)
    {
        if (_failed) {
            return false;
        }
        if (byte == '\n') {
            if (!processLine()) {
                _failed = true;
                return false;
            }
            _lineSize = 0;
            ++_lineNumber;
            return true;
        }
        if (_lineSize == _line.size()) {
            _failed = true;
            return false;
        }
        _line[_lineSize++] = byte;
        return true;
    }

    bool tokenize(std::array<TokenSpan, 12>& tokens, std::size_t& count) const
    {
        count = 0;
        std::size_t index = 0;
        while (index < _lineSize) {
            while (index < _lineSize
                   && (_line[index] == ' ' || _line[index] == '\t')) {
                ++index;
            }
            if (index == _lineSize) {
                break;
            }
            if (count == tokens.size()) {
                return false;
            }
            const std::size_t begin = index;
            while (index < _lineSize
                   && _line[index] != ' ' && _line[index] != '\t') {
                ++index;
            }
            tokens[count++] = {begin, index};
        }
        return true;
    }

    bool splitAttachedContinuity(std::array<TokenSpan, 12>& tokens,
                                 std::size_t& count) const
    {
        if (count != 7 || tokens[2].end - tokens[2].begin < 3
            || count == tokens.size()) {
            return false;
        }
        const std::size_t continuity = tokens[2].end - 2;
        for (std::size_t index = count; index > 3; --index) {
            tokens[index] = tokens[index - 1];
        }
        tokens[3] = {continuity, tokens[2].end};
        tokens[2].end = continuity;
        ++count;
        return tokens[2].begin != tokens[2].end && isContinuity(tokens[3]);
    }

    bool isContinuity(TokenSpan token) const
    {
        if (token.end - token.begin != 2) {
            return false;
        }
        const char family = _line[token.begin];
        const char degree = _line[token.begin + 1];
        return (family == 'C'
                && ((degree >= '0' && degree <= '3') || degree == 'N'))
            || (family == 'G' && (degree == '1' || degree == '2'));
    }

    bool appendRawLine()
    {
        _digest.append(_line.data(), _lineSize);
        _digest.append("\n");
        return true;
    }

    bool appendLineReplacing(const std::array<TokenSpan, 12>& tokens,
                             const std::array<std::size_t, 2>& locations,
                             std::size_t locationCount)
    {
        std::size_t cursor = 0;
        for (std::size_t index = 0; index < locationCount; ++index) {
            const TokenSpan token = tokens[locations[index]];
            Standard_Integer locationIndex = 0;
            if (token.begin < cursor
                || !parseLocationIndex(_line.data(), token, locationIndex)) {
                return false;
            }
            _digest.append(_line.data() + cursor, token.begin - cursor);
            if (!appendCanonicalLocation(_digest,
                                         _shapeSet.Locations(),
                                         locationIndex)) {
                return false;
            }
            cursor = token.end;
        }
        _digest.append(_line.data() + cursor, _lineSize - cursor);
        _digest.append("\n");
        return true;
    }

    bool processVertexLine(const std::array<TokenSpan, 12>& tokens,
                           std::size_t count)
    {
        if (_ended) {
            return false;
        }
        if (_lineNumber < 2) {
            return appendRawLine();
        }
        Standard_Integer record = -1;
        if (count == 2
            && parseLocationIndex(_line.data(), tokens[0], record)
            && record == 0) {
            Standard_Integer terminator = -1;
            if (!parseLocationIndex(_line.data(), tokens[1], terminator)
                || terminator != 0) {
                return false;
            }
            _ended = true;
            return appendRawLine();
        }
        if (count < 2
            || !parseLocationIndex(_line.data(), tokens[1], record)) {
            return false;
        }
        const std::size_t expected = record == 1 ? 4 : record == 2 ? 5 : record == 3 ? 5 : 0;
        return expected != 0 && count == expected
            && appendLineReplacing(tokens, {count - 1, 0}, 1);
    }

    bool processEdgeLine(std::array<TokenSpan, 12> tokens, std::size_t count)
    {
        if (_ended) {
            return false;
        }
        if (_lineNumber == 0) {
            return appendRawLine();
        }
        Standard_Integer record = -1;
        if (count == 1
            && parseLocationIndex(_line.data(), tokens[0], record)
            && record == 0) {
            _ended = true;
            return appendRawLine();
        }
        if (count == 0
            || !parseLocationIndex(_line.data(), tokens[0], record)) {
            return false;
        }
        switch (record) {
            case 1:
                return count == 5
                    && appendLineReplacing(tokens, {2, 0}, 1);
            case 2:
                return count == 6
                    && appendLineReplacing(tokens, {3, 0}, 1);
            case 3:
                if (count == 7 && !splitAttachedContinuity(tokens, count)) {
                    return false;
                }
                return count == 8 && isContinuity(tokens[3])
                    && appendLineReplacing(tokens, {5, 0}, 1);
            case 4:
                return count == 6 && isContinuity(tokens[1])
                    && appendLineReplacing(tokens, {3, 5}, 2);
            default:
                // Triangle-backed records are impossible for a no-mesh
                // ShapeSet; accepting them would silently apply the wrong
                // location-token grammar.
                return false;
        }
    }

    bool processFaceLine(const std::array<TokenSpan, 12>& tokens,
                         std::size_t count)
    {
        return _lineNumber == 0 && count == 4
            && appendLineReplacing(tokens, {3, 0}, 1);
    }

    bool processLine()
    {
        std::array<TokenSpan, 12> tokens {};
        std::size_t count = 0;
        if (!tokenize(tokens, count)) {
            return false;
        }
        switch (_type) {
            case TopAbs_VERTEX:
                return processVertexLine(tokens, count);
            case TopAbs_EDGE:
                return processEdgeLine(tokens, count);
            case TopAbs_FACE:
                return processFaceLine(tokens, count);
            default:
                return false;
        }
    }

    DigestStreamBuffer& _digest;
    const BRepTools_ShapeSet& _shapeSet;
    const TopAbs_ShapeEnum _type;
    std::array<char, 1024> _line {};
    std::size_t _lineSize {0};
    std::size_t _lineNumber {0};
    bool _ended {false};
    bool _failed {false};
};

const char* shapeTypeToken(TopAbs_ShapeEnum type)
{
    switch (type) {
        case TopAbs_VERTEX:
            return "Ve";
        case TopAbs_EDGE:
            return "Ed";
        case TopAbs_WIRE:
            return "Wi";
        case TopAbs_FACE:
            return "Fa";
        case TopAbs_SHELL:
            return "Sh";
        case TopAbs_SOLID:
            return "So";
        case TopAbs_COMPSOLID:
            return "CS";
        case TopAbs_COMPOUND:
            return "Co";
        case TopAbs_SHAPE:
            return "Sp";
    }
    return nullptr;
}

char orientationToken(TopAbs_Orientation orientation)
{
    switch (orientation) {
        case TopAbs_FORWARD:
            return '+';
        case TopAbs_REVERSED:
            return '-';
        case TopAbs_INTERNAL:
            return 'i';
        case TopAbs_EXTERNAL:
            return 'e';
    }
    return '\0';
}

bool appendShapeReference(DigestStreamBuffer& digest,
                          const BRepTools_ShapeSet& shapeSet,
                          const TopoDS_Shape& shape)
{
    if (shape.IsNull()) {
        digest.append("*");
        return true;
    }
    const char orientation = orientationToken(shape.Orientation());
    TopoDS_Shape unlocated = shape;
    unlocated.Location(TopLoc_Location {});
    const Standard_Integer storedIndex = shapeSet.Index(unlocated);
    const Standard_Integer locationIndex = shapeSet.Locations().Index(shape.Location());
    if (orientation == '\0' || storedIndex <= 0
        || (!shape.Location().IsIdentity() && locationIndex <= 0)) {
        return false;
    }
    digest.append(&orientation, 1);
    if (!appendInteger(digest, shapeSet.NbShapes() - storedIndex + 1)) {
        return false;
    }
    digest.append(" ");
    if (!appendCanonicalLocation(digest, shapeSet.Locations(), locationIndex)) {
        return false;
    }
    digest.append(" ");
    return true;
}

ShapeDigest canonicalShapeDigest(const TopoDS_Shape& shape,
                                 std::size_t* bytesWritten = nullptr)
{
    if (shape.IsNull()) {
        throw std::runtime_error("Part Boolean result canonicalization received a null shape");
    }

    // ShapeSet fixes the V3/no-mesh grammar and exposes every indexed
    // location used by both topology and BRep geometry. The semantic stream
    // deliberately omits ShapeSet's representation-specific Locations table:
    // every location reference is replaced in place with its effective gp_Trsf.
    BRepTools_ShapeSet shapeSet(Standard_False, Standard_False);
#if OCC_VERSION_HEX >= 0x070600
    shapeSet.SetFormatNb(TopTools_FormatVersion_VERSION_3);
#else
    // TopTools_FormatVersion was made public in OCCT 7.6. The ASCII V3
    // format number itself is supported by the legacy ShapeSet API.
    shapeSet.SetFormatNb(3);
#endif
    if (shapeSet.Add(shape) <= 0 || shapeSet.NbShapes() <= 0) {
        throw std::runtime_error("Part Boolean result ShapeSet is empty");
    }

    DigestStreamBuffer digest;
    digest.append("BRepSemanticV3\nGeometry\n");
    {
        std::ostream stream(&digest);
        stream.imbue(std::locale::classic());
        stream.precision(15);
        shapeSet.WriteGeometry(stream);
        if (!stream) {
            throw std::runtime_error("Part Boolean global geometry serialization failed");
        }
    }
    digest.append("\nTShapes ");
    if (!appendInteger(digest, shapeSet.NbShapes())) {
        throw std::runtime_error("Part Boolean TShape count serialization failed");
    }
    digest.append("\n");

    for (Standard_Integer index = 1; index <= shapeSet.NbShapes(); ++index) {
        const TopoDS_Shape& stored = shapeSet.Shape(index);
        const char* type = shapeTypeToken(stored.ShapeType());
        if (!type) {
            throw std::runtime_error("Part Boolean TShape type serialization failed");
        }
        digest.append(type, 2);
        digest.append("\n");

        if (stored.ShapeType() == TopAbs_VERTEX
            || stored.ShapeType() == TopAbs_EDGE
            || stored.ShapeType() == TopAbs_FACE) {
            GeometryStreamBuffer geometry(digest, shapeSet, stored.ShapeType());
            std::ostream stream(&geometry);
            stream.imbue(std::locale::classic());
            stream.precision(15);
            shapeSet.WriteGeometry(stored, stream);
            if (!stream || !geometry.finish()) {
                throw std::runtime_error(
                    "Part Boolean per-TShape geometry serialization failed");
            }
        }

        digest.append("\n000");
        const std::array<bool, 4> semanticFlags {
            stored.Orientable(), stored.Closed(), stored.Infinite(), stored.Convex()};
        for (bool flag : semanticFlags) {
            digest.append(flag ? "1" : "0");
        }
        digest.append("\n");

        Standard_Integer referencesOnLine = 0;
        for (TopoDS_Iterator iterator(stored, Standard_False, Standard_False);
             iterator.More();
             iterator.Next()) {
            if (!appendShapeReference(digest, shapeSet, iterator.Value())) {
                throw std::runtime_error(
                    "Part Boolean child topology serialization failed");
            }
            if (++referencesOnLine == 10) {
                digest.append("\n");
                referencesOnLine = 0;
            }
        }
        digest.append("*\n");
    }

    digest.append("\nRoot\n");
    if (!appendShapeReference(digest, shapeSet, shape)) {
        throw std::runtime_error("Part Boolean root topology serialization failed");
    }
    digest.append("\n");

    if (bytesWritten) {
        *bytesWritten = digest.bytesWritten();
    }
    return digest.digest();
}

ShapeDigest expectedShapeDigest(
    const TopoDS_Shape& computed,
    Part::Internal::CollaborativeBooleanTaskProbe* probe)
{
    // BRepBuilderAPI_Copy is the required isolation boundary for every live
    // application attempt. Normalize the detached worker result through that
    // same boundary once, off the document thread, so the immutable trust root
    // describes the representation that can actually be installed. Validation
    // remains a bounded, copy-free stream over the live result, and retries
    // continue to take fresh copies from the pristine worker result.
    BRepBuilderAPI_Copy expectedCopy(computed, Standard_True, Standard_False);
    if (!expectedCopy.IsDone() || expectedCopy.Shape().IsNull()) {
        throw std::runtime_error("Part Boolean expected result copy failed");
    }
    std::size_t bytesWritten = 0;
    ShapeDigest digest = canonicalShapeDigest(expectedCopy.Shape(), &bytesWritten);
    if (probe) {
        probe->expectedCanonicalBytes.store(bytesWritten, std::memory_order_relaxed);
    }
    return digest;
}

std::vector<App::DocumentRevisionKey> canonicalKeys(
    std::vector<App::DocumentRevisionKey> keys)
{
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

bool isValidShape(const TopoDS_Shape& shape);

BooleanKind parseKind(const std::string& value)
{
    if (value == "cut") {
        return BooleanKind::Cut;
    }
    if (value == "fuse") {
        return BooleanKind::Fuse;
    }
    if (value == "common") {
        return BooleanKind::Common;
    }
    throw std::invalid_argument("Part Boolean kind must be cut, fuse, or common");
}

std::vector<std::uint8_t> encodeShape(const TopoDS_Shape& shape)
{
    if (!isValidShape(shape)) {
        throw std::invalid_argument("isolated Part geometry input is null or invalid");
    }
    std::ostringstream stream(std::ios::out | std::ios::binary);
    stream.imbue(std::locale::classic());
    BRepTools::Write(shape, stream);
    if (!stream) {
        throw std::runtime_error("isolated Part geometry BREP encoding failed");
    }
    const std::string encoded = stream.str();
    return {encoded.begin(), encoded.end()};
}

TopoDS_Shape decodeShape(const App::GeometryArchiveSection& section)
{
    try {
        if (section.bytes.empty()) {
            throw std::invalid_argument("isolated Part geometry BREP payload is empty");
        }
        const std::string encoded(section.bytes.begin(), section.bytes.end());
        std::istringstream stream(encoded, std::ios::in | std::ios::binary);
        stream.imbue(std::locale::classic());
        BRep_Builder builder;
        TopoDS_Shape shape;
        BRepTools::Read(shape, stream, builder);
        if (!stream || !isValidShape(shape)) {
            throw std::invalid_argument("isolated Part geometry BREP payload is invalid");
        }
        stream >> std::ws;
        if (!stream.eof()) {
            throw std::invalid_argument(
                "isolated Part geometry BREP payload has trailing data");
        }
        return shape;
    }
    catch (const Standard_Failure& failure) {
        const char* detail = failure.GetMessageString();
        throw std::invalid_argument(
            detail && *detail
                ? std::string("isolated Part geometry BREP payload failed: ") + detail
                : "isolated Part geometry BREP payload failed");
    }
}

void appendDouble(std::vector<std::uint8_t>& target, const double value)
{
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        target.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xffU));
    }
}

double readDouble(const std::vector<std::uint8_t>& source, const std::size_t offset)
{
    if (offset > source.size() || source.size() - offset < sizeof(double)) {
        throw std::invalid_argument("isolated Part Boolean parameters are truncated");
    }
    std::uint64_t bits = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        bits |= static_cast<std::uint64_t>(source[offset + shift / 8]) << shift;
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) {
        throw std::invalid_argument("isolated Part Boolean parameter is not finite");
    }
    return value;
}

const App::GeometryArchiveSection& requireSection(
    const App::GeometryArchive& archive,
    const std::string& name)
{
    const auto found = std::ranges::find_if(archive.sections, [&](const auto& section) {
        return section.name == name;
    });
    if (found == archive.sections.end()) {
        throw std::invalid_argument("isolated Part geometry section is missing: " + name);
    }
    return *found;
}

std::string sectionDigest(const std::vector<App::GeometryArchiveSection>& sections)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& section : sections) {
        const std::uint64_t nameSize = section.name.size();
        const std::uint64_t byteSize = section.bytes.size();
        hash.addData(reinterpret_cast<const char*>(&nameSize), sizeof(nameSize));
        hash.addData(section.name.data(), static_cast<qsizetype>(section.name.size()));
        hash.addData(reinterpret_cast<const char*>(&byteSize), sizeof(byteSize));
        if (!section.bytes.empty()) {
            hash.addData(reinterpret_cast<const char*>(section.bytes.data()),
                         static_cast<qsizetype>(section.bytes.size()));
        }
    }
    return hash.result().toHex().toStdString();
}

bool isValidShape(const TopoDS_Shape& shape)
{
    if (shape.IsNull()) {
        return false;
    }
    if (shape.ShapeType() == TopAbs_COMPOUND && !TopoDS_Iterator(shape).More()) {
        return false;
    }
    try {
        return BRepCheck_Analyzer(shape).IsValid();
    }
    catch (const Standard_Failure&) {
        return false;
    }
}

Part::TopoShape inputShape(const Part::Feature& feature)
{
    return Part::Feature::getTopoShape(
               &feature,
               Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform);
}

const Part::Feature& requireFeature(const App::Document& document,
                                    const std::string& name,
                                    const char* role)
{
    const auto* object = document.getObject(name.c_str());
    const auto* feature = dynamic_cast<const Part::Feature*>(object);
    if (!feature) {
        throw std::invalid_argument(std::string("Part Boolean ") + role
                                    + " must identify an attached Part::Feature");
    }
    return *feature;
}

const Part::Feature& resolveFeature(const App::Document& document,
                                    const ObjectReference& reference,
                                    const char* role)
{
    const auto* object = document.getObject(reference.name.c_str());
    const auto* feature = dynamic_cast<const Part::Feature*>(object);
    if (!feature
        || document.collaborationObjectIdentity(*feature) != reference.identity) {
        throw std::runtime_error(std::string("Part Boolean ") + role
                                 + " object identity is stale");
    }
    return *feature;
}

Part::Feature& resolveFeature(App::Document& document,
                              const ObjectReference& reference,
                              const char* role)
{
    return const_cast<Part::Feature&>(resolveFeature(
        static_cast<const App::Document&>(document), reference, role));
}

bool hasHazardousInputDependency(const Part::Feature& result,
                                 const Part::Feature& base,
                                 const Part::Feature& tool)
{
    if (&result == &base || &result == &tool) {
        return true;
    }
    const auto dependents = result.getInListRecursive();
    return std::ranges::any_of(dependents, [&](const auto* dependent) {
        return dependent == static_cast<const App::DocumentObject*>(&base)
            || dependent == static_cast<const App::DocumentObject*>(&tool);
    });
}

std::map<std::string, std::string> freezeRecomputeClosure(
    const App::Document& document,
    const Part::Feature& result)
{
    std::map<std::string, std::string> affected;
    affected.emplace(result.getNameInDocument(),
                     document.collaborationObjectIdentity(result));
    for (const auto* dependent : result.getInListRecursive()) {
        if (!dependent || dependent->getDocument() != &document) {
            continue;
        }
        const char* name = dependent->getNameInDocument();
        if (!name || *name == '\0') {
            throw std::invalid_argument(
                "Part Boolean dependent object has no document name");
        }
        const std::string identity =
            document.collaborationObjectIdentity(*dependent);
        const auto [position, inserted] = affected.emplace(name, identity);
        if (!inserted && position->second != identity) {
            throw std::invalid_argument(
                "Part Boolean dependency graph has ambiguous object identity");
        }
    }
    return affected;
}

const char* operationCode(BooleanKind kind)
{
    switch (kind) {
        case BooleanKind::Cut:
            return Part::OpCodes::Cut;
        case BooleanKind::Fuse:
            return Part::OpCodes::Fuse;
        case BooleanKind::Common:
            return Part::OpCodes::Common;
    }
    throw std::logic_error("Part Boolean kind is invalid");
}

std::unique_ptr<BRepAlgoAPI_BooleanOperation> makeRawBoolean(BooleanKind kind)
{
    switch (kind) {
        case BooleanKind::Cut:
            return std::make_unique<BRepAlgoAPI_Cut>();
        case BooleanKind::Fuse:
            return std::make_unique<BRepAlgoAPI_Fuse>();
        case BooleanKind::Common:
            return std::make_unique<BRepAlgoAPI_Common>();
    }
    throw std::logic_error("Part Boolean kind is invalid");
}

Part::TopoShape runRawBoolean(
    BooleanKind kind,
    const std::vector<Part::TopoShape>& arguments,
    const std::vector<Part::TopoShape>& tools,
    std::stop_token stopToken,
    double fuzzyTolerance,
    const Handle(StopTokenProgressIndicator)& progress,
    Part::Internal::CollaborativeBooleanTaskProbe* probe,
    bool allowEmptyResult = false)
{
    if (arguments.empty() || tools.empty()) {
        throw std::runtime_error("Part Boolean calculation requires arguments and tools");
    }
    if (stopToken.stop_requested()) {
        throw std::runtime_error("Part Boolean detached preparation was cancelled");
    }

    auto operation = makeRawBoolean(kind);
    TopTools_ListOfShape argumentShapes;
    TopTools_ListOfShape toolShapes;
    for (const auto& argument : arguments) {
        argumentShapes.Append(argument.getShape());
    }
    for (const auto& tool : tools) {
        toolShapes.Append(tool.getShape());
    }
    operation->SetArguments(argumentShapes);
    operation->SetTools(toolShapes);
    operation->SetRunParallel(Standard_True);
    operation->SetNonDestructive(Standard_True);
    operation->SetFuzzyValue(fuzzyTolerance);

    if (probe) {
        probe->buildEntered.store(true, std::memory_order_relaxed);
    }
#if OCC_VERSION_HEX >= 0x070600
    operation->Build(progress->Start());
#else
    operation->SetProgressIndicator(progress);
    operation->Build();
#endif
    if (stopToken.stop_requested()) {
        throw std::runtime_error("Part Boolean detached preparation was cancelled");
    }
    if (!operation->IsDone()) {
        throw std::runtime_error("Part Boolean calculation did not complete");
    }
    const bool emptyCompound = !operation->Shape().IsNull()
        && operation->Shape().ShapeType() == TopAbs_COMPOUND
        && !TopoDS_Iterator(operation->Shape()).More();
    if (!allowEmptyResult && !isValidShape(operation->Shape())) {
        throw std::runtime_error("Part Boolean produced a null or invalid shape");
    }
    if (allowEmptyResult && operation->Shape().IsNull()) {
        throw std::runtime_error("Part Boolean produced a null shape");
    }
    if (emptyCompound && allowEmptyResult) {
        return Part::TopoShape(operation->Shape());
    }

    std::vector<Part::TopoShape> inputs = arguments;
    inputs.insert(inputs.end(), tools.begin(), tools.end());
    Part::TopoShape result(0);
    result.makeElementShape(*operation, inputs, operationCode(kind));
    if (!isValidShape(result.getShape())) {
        throw std::runtime_error("Part Boolean produced a null or invalid mapped shape");
    }
    return result;
}

void flattenCompound(const Part::TopoShape& shape,
                     std::vector<Part::TopoShape>& flattened)
{
    for (TopoDS_Iterator iterator(shape.getShape()); iterator.More(); iterator.Next()) {
        Part::TopoShape child(iterator.Value());
        if (child.getShape().ShapeType() == TopAbs_COMPOUND) {
            flattenCompound(child, flattened);
        }
        else {
            flattened.push_back(std::move(child));
        }
    }
}

Part::TopoShape cutCompoundArgument(
    const Part::TopoShape& argument,
    const std::vector<Part::TopoShape>& tools,
    std::stop_token stopToken,
    double fuzzyTolerance,
    const Handle(StopTokenProgressIndicator)& progress,
    Part::Internal::CollaborativeBooleanTaskProbe* probe,
    bool allowEmptyResult = false)
{
    if (argument.getShape().ShapeType() != TopAbs_COMPOUND) {
        return runRawBoolean(BooleanKind::Cut,
                             {argument},
                             tools,
                             stopToken,
                             fuzzyTolerance,
                             progress,
                             probe,
                             allowEmptyResult);
    }

    std::vector<Part::TopoShape> cutChildren;
    for (TopoDS_Iterator iterator(argument.getShape()); iterator.More(); iterator.Next()) {
        cutChildren.push_back(cutCompoundArgument(Part::TopoShape(iterator.Value()),
                                                   tools,
                                                   stopToken,
                                                   fuzzyTolerance,
                                                   progress,
                                                   probe,
                                                   true));
    }
    if (cutChildren.empty()) {
        throw std::runtime_error("Part Boolean compound argument is empty");
    }
    Part::TopoShape result(0);
    result.makeElementCompound(
        cutChildren,
        Part::OpCodes::Cut,
        Part::TopoShape::SingleShapeCompoundCreationPolicy::forceCompound);
    if (!isValidShape(result.getShape())) {
        throw std::runtime_error("Part Boolean produced an invalid compound cut");
    }
    return result;
}

Part::TopoShape computeBoolean(BooleanKind kind,
                               const Part::TopoShape& base,
                               const Part::TopoShape& tool,
                               std::stop_token stopToken,
                               double fuzzySetting,
                               double confusion,
                               Part::Internal::CollaborativeBooleanTaskProbe* probe)
{
    try {
        Handle(StopTokenProgressIndicator) progress =
            new StopTokenProgressIndicator(stopToken, probe);
        Bnd_Box originalBounds;
        BRepBndLib::Add(base.getShape(), originalBounds);
        BRepBndLib::Add(tool.getShape(), originalBounds);
        const double fuzzyTolerance = fuzzySetting
            * std::sqrt(originalBounds.SquareExtent()) * confusion;
        Part::TopoShape result;
        if (kind != BooleanKind::Cut) {
            result = runRawBoolean(kind,
                                   {base},
                                   {tool},
                                   stopToken,
                                   fuzzyTolerance,
                                   progress,
                                   probe);
        }
        else {
            std::vector<Part::TopoShape> cutTools;
            if (tool.getShape().ShapeType() == TopAbs_COMPOUND) {
                flattenCompound(tool, cutTools);
                if (cutTools.size() >= 2) {
                    const Part::TopoShape firstTool = cutTools.front();
                    std::vector<Part::TopoShape> remainingTools(cutTools.begin() + 1,
                                                               cutTools.end());
                    Part::TopoShape fusedTool = runRawBoolean(BooleanKind::Fuse,
                                                              {firstTool},
                                                              remainingTools,
                                                              stopToken,
                                                              fuzzyTolerance,
                                                              progress,
                                                              probe);
                    cutTools.clear();
                    // Match FCBRepAlgoAPI's RecursiveAddTools: inspect the
                    // fused result's children even when its root is a solid.
                    flattenCompound(fusedTool, cutTools);
                }
            }
            else {
                cutTools.push_back(tool);
            }
            if (cutTools.empty()) {
                throw std::runtime_error("Part Boolean compound tool is empty");
            }
            result = cutCompoundArgument(base,
                                         cutTools,
                                         stopToken,
                                         fuzzyTolerance,
                                         progress,
                                         probe);
        }
        if (probe
            && !probe->cancellationObserved.load(std::memory_order_relaxed)) {
            probe->buildCompletedNaturally.store(true, std::memory_order_relaxed);
        }
        return result;
    }
    catch (const Standard_Failure& failure) {
        if (stopToken.stop_requested()) {
            if (probe) {
                probe->cancellationObserved.store(true, std::memory_order_relaxed);
            }
            throw std::runtime_error("Part Boolean detached preparation was cancelled");
        }
        const char* detail = failure.GetMessageString();
        throw std::runtime_error(detail && *detail
                                     ? std::string("Part Boolean calculation failed: ") + detail
                                     : "Part Boolean calculation failed");
    }
}

Part::TopoShape captureShape(const Part::TopoShape& source,
                             const char* role,
                             std::atomic<bool>* independent = nullptr)
{
    try {
        BRepBuilderAPI_Copy copy(source.getShape(), Standard_True, Standard_False);
        if (!copy.IsDone() || copy.Shape().IsNull()) {
            throw std::invalid_argument(std::string("Part Boolean ") + role
                                        + " shape could not be captured");
        }
        bool sharesSourceTShape = copy.Shape().IsPartner(source.getShape());
        TopTools_IndexedMapOfShape sourceTShapes;
        TopTools_IndexedMapOfShape sourceShapes;
        TopExp::MapShapes(source.getShape(), sourceShapes);
        for (Standard_Integer index = 1; index <= sourceShapes.Extent(); ++index) {
            TopoDS_Shape normalized = sourceShapes(index);
            normalized.Location(TopLoc_Location {});
            normalized.Orientation(TopAbs_FORWARD);
            sourceTShapes.Add(normalized);
        }
        TopTools_IndexedMapOfShape snapshotShapes;
        TopExp::MapShapes(copy.Shape(), snapshotShapes);
        for (Standard_Integer index = 1;
             !sharesSourceTShape && index <= snapshotShapes.Extent();
             ++index) {
            TopoDS_Shape normalized = snapshotShapes(index);
            normalized.Location(TopLoc_Location {});
            normalized.Orientation(TopAbs_FORWARD);
            if (sourceTShapes.Contains(normalized)) {
                sharesSourceTShape = true;
                break;
            }
        }
        if (sharesSourceTShape) {
            if (independent) {
                independent->store(false, std::memory_order_relaxed);
            }
            throw std::runtime_error(std::string("Part Boolean ") + role
                                     + " snapshot shares a source TShape");
        }
        if (independent) {
            independent->store(true, std::memory_order_relaxed);
        }
        return Part::TopoShape(copy.Shape());
    }
    catch (const Standard_Failure& failure) {
        const char* detail = failure.GetMessageString();
        throw std::invalid_argument(
            detail && *detail
                ? std::string("Part Boolean ") + role
                    + " shape capture failed: " + detail
                : std::string("Part Boolean ") + role + " shape capture failed");
    }
}

class CollaborativeBooleanOperation final: public App::CollaborativeOperation
{
public:
    CollaborativeBooleanOperation(ObjectReference base,
                                  ObjectReference tool,
                                  ObjectReference result,
                                  Part::TopoShape computedResult,
                                  Part::Internal::CollaborativeBooleanTaskProbe* probe)
        : _base(std::move(base))
        , _tool(std::move(tool))
        , _result(std::move(result))
        , _computedResult(std::move(computedResult))
        , _computedDigest(expectedShapeDigest(_computedResult.getShape(), probe))
    {}

    std::string_view typeId() const noexcept override
    {
        return Part::CollaborativeBooleanOperationType;
    }

    void apply(App::Document& document) const override
    {
        const auto& base = resolveFeature(document, _base, "base");
        const auto& tool = resolveFeature(document, _tool, "tool");
        auto& result = resolveFeature(document, _result, "result");
        if (result.getTypeId() != Part::Feature::getClassTypeId()) {
            throw std::runtime_error("Part Boolean result type changed");
        }
        if (hasHazardousInputDependency(result, base, tool)) {
            throw std::runtime_error(
                "Part Boolean result dependency closure contains an input");
        }

        _applied = false;
        BRepBuilderAPI_Copy attemptCopy(_computedResult.getShape(),
                                        Standard_True,
                                        Standard_False);
        if (!attemptCopy.IsDone() || attemptCopy.Shape().IsNull()) {
            throw std::runtime_error("Part Boolean result application copy failed");
        }
        result.Shape.setValue(Part::TopoShape(attemptCopy.Shape()));
        _applied = true;
    }

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document& document) const override
    {
        try {
            const auto& base = resolveFeature(document, _base, "base");
            const auto& tool = resolveFeature(document, _tool, "tool");
            const auto& result = resolveFeature(document, _result, "result");
            if (result.getTypeId() != Part::Feature::getClassTypeId()) {
                return {false, "Part Boolean result is not an exact Part::Feature"};
            }
            if (hasHazardousInputDependency(result, base, tool)) {
                return {false,
                        "Part Boolean result dependency closure contains an input"};
            }
            if (!isValidShape(result.Shape.getValue())) {
                return {false, "Part Boolean result shape is null, empty, or invalid"};
            }
            if (!_applied
                || canonicalShapeDigest(result.Shape.getValue()) != _computedDigest) {
                return {false, "Part Boolean result does not match the prepared shape"};
            }
        }
        catch (...) {
            return {false, "Part Boolean object identity or topology is stale"};
        }
        return {true, {}};
    }

private:
    const ObjectReference _base;
    const ObjectReference _tool;
    const ObjectReference _result;
    const Part::TopoShape _computedResult;
    const ShapeDigest _computedDigest;
    mutable bool _applied {false};
};

std::uint8_t encodeBooleanKind(const BooleanKind kind)
{
    switch (kind) {
        case BooleanKind::Cut:
            return 1;
        case BooleanKind::Fuse:
            return 2;
        case BooleanKind::Common:
            return 3;
    }
    throw std::invalid_argument("unsupported isolated Part Boolean kind");
}

BooleanKind decodeBooleanKind(const std::uint8_t kind)
{
    switch (kind) {
        case 1:
            return BooleanKind::Cut;
        case 2:
            return BooleanKind::Fuse;
        case 3:
            return BooleanKind::Common;
        default:
            throw std::invalid_argument("isolated Part Boolean kind is invalid");
    }
}

App::GeometryArchive executeBooleanArchive(const App::GeometryArchive& input,
                                           const std::stop_token stopToken)
{
    if (input.metadata.operationType != Part::CollaborativeBooleanOperationType
        || input.sections.size() != 3) {
        throw std::invalid_argument("isolated Part Boolean request contract is invalid");
    }
    const auto& parameters = requireSection(input, "parameters").bytes;
    if (parameters.size() != 17) {
        throw std::invalid_argument("isolated Part Boolean parameters are invalid");
    }
    const BooleanKind kind = decodeBooleanKind(parameters.front());
    const double fuzzySetting = readDouble(parameters, 1);
    const double confusion = readDouble(parameters, 9);
    if (fuzzySetting < 0.0 || confusion <= 0.0) {
        throw std::invalid_argument("isolated Part Boolean tolerances are invalid");
    }
    const Part::TopoShape base(decodeShape(requireSection(input, "base.brep")));
    const Part::TopoShape tool(decodeShape(requireSection(input, "tool.brep")));
    Part::TopoShape result = computeBoolean(kind,
                                            base,
                                            tool,
                                            stopToken,
                                            fuzzySetting,
                                            confusion,
                                            nullptr);
    if (stopToken.stop_requested()) {
        throw std::runtime_error("isolated Part Boolean operation was cancelled");
    }
    App::GeometryArchive output;
    output.sections.push_back({"result.brep", encodeShape(result.getShape())});
    App::GeometryArchiveSection history;
    App::GeometryArchiveError historyError;
    history.name = "element-history";
    if (!App::GeometryArchiveCodec::encodeElementHistory({}, history, historyError)) {
        throw std::runtime_error(historyError.code + ": " + historyError.message);
    }
    output.sections.push_back(std::move(history));
    return output;
}

std::unique_ptr<const App::CollaborativeOperation> decodeBooleanResult(
    const App::GeometryArchive& output,
    ObjectReference base,
    ObjectReference tool,
    ObjectReference result)
{
    if (output.metadata.operationType != Part::CollaborativeBooleanOperationType
        || output.sections.size() != 2) {
        throw std::invalid_argument("isolated Part Boolean result contract is invalid");
    }
    App::GeometryElementHistory history;
    App::GeometryArchiveError historyError;
    if (!App::GeometryArchiveCodec::decodeElementHistory(
            requireSection(output, "element-history"), history, historyError)) {
        throw std::invalid_argument(historyError.code + ": " + historyError.message);
    }
    if (!history.generated.empty() || !history.modified.empty()
        || !history.deleted.empty()) {
        throw std::invalid_argument("isolated Part Boolean returned unexpected history");
    }
    return std::make_unique<const CollaborativeBooleanOperation>(
        std::move(base),
        std::move(tool),
        std::move(result),
        Part::TopoShape(decodeShape(requireSection(output, "result.brep"))),
        nullptr);
}

App::CollaborativeOperationPreparation prepareBooleanImpl(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent,
    std::shared_ptr<Part::Internal::CollaborativeBooleanTaskProbe> probe)
{
    static constexpr std::string_view requiredArguments[] = {"base", "tool", "result", "kind"};
    if (intent.arguments.size() != std::size(requiredArguments)
        || std::ranges::any_of(requiredArguments, [&](std::string_view name) {
               return !intent.arguments.contains(std::string(name));
           })) {
        throw std::invalid_argument(
            "Part Boolean intent requires exactly base, tool, result, and kind");
    }

    const std::string baseName = intent.arguments.at("base");
    const std::string toolName = intent.arguments.at("tool");
    const std::string resultName = intent.arguments.at("result");
    if (baseName.empty() || toolName.empty() || resultName.empty()) {
        throw std::invalid_argument("Part Boolean object names must be nonempty");
    }

    const auto& base = requireFeature(document, baseName, "base");
    const auto& tool = requireFeature(document, toolName, "tool");
    const auto& result = requireFeature(document, resultName, "result");
    if (result.getTypeId() != Part::Feature::getClassTypeId()) {
        throw std::invalid_argument(
            "Part Boolean result must be an exact Part::Feature");
    }
    if (hasHazardousInputDependency(result, base, tool)) {
        throw std::invalid_argument(
            "Part Boolean result dependency closure contains an input");
    }
    const Part::TopoShape liveBaseShape = inputShape(base);
    const Part::TopoShape liveToolShape = inputShape(tool);
    if (!isValidShape(liveBaseShape.getShape())) {
        throw std::invalid_argument("Part Boolean base shape is null or invalid");
    }
    if (!isValidShape(liveToolShape.getShape())) {
        throw std::invalid_argument("Part Boolean tool shape is null or invalid");
    }
    const BooleanKind kind = parseKind(intent.arguments.at("kind"));
    ObjectReference baseReference {baseName, document.collaborationObjectIdentity(base)};
    ObjectReference toolReference {toolName, document.collaborationObjectIdentity(tool)};
    ObjectReference resultReference {resultName, document.collaborationObjectIdentity(result)};
    if (baseReference.identity.empty() || toolReference.identity.empty()
        || resultReference.identity.empty()) {
        throw std::invalid_argument("Part Boolean object identity must be nonempty");
    }

    const auto affected = freezeRecomputeClosure(document, result);
    std::vector<App::DocumentRevisionKey> reads {
        App::DocumentRevisionKey::objectExistence(baseName),
        App::DocumentRevisionKey::objectModel(baseName),
        App::DocumentRevisionKey::objectStructure(baseName),
        App::DocumentRevisionKey::objectExistence(toolName),
        App::DocumentRevisionKey::objectModel(toolName),
        App::DocumentRevisionKey::objectStructure(toolName),
        App::DocumentRevisionKey::documentStructure(),
        App::DocumentRevisionKey::unknownModelMutation()};
    std::vector<App::DocumentRevisionKey> writes;
    std::vector<App::DocumentRevisionPublicationRequest> effects;
    reads.reserve(reads.size() + affected.size() * 3);
    writes.reserve(affected.size() + 1);
    effects.reserve(affected.size() + 1);
    for (const auto& [affectedName, affectedIdentity] : affected) {
        reads.push_back(App::DocumentRevisionKey::objectExistence(affectedName));
        reads.push_back(App::DocumentRevisionKey::objectModel(affectedName));
        reads.push_back(App::DocumentRevisionKey::objectStructure(affectedName));
        const auto model = App::DocumentRevisionKey::objectModel(affectedName);
        writes.push_back(model);
        effects.push_back({model, affectedIdentity});
    }
    const auto resultStructure =
        App::DocumentRevisionKey::objectStructure(resultName);
    writes.push_back(resultStructure);
    effects.push_back({resultStructure, resultReference.identity});
    reads = canonicalKeys(std::move(reads));
    writes = canonicalKeys(std::move(writes));
    std::sort(effects.begin(), effects.end(), [](const auto& left, const auto& right) {
        return left.key < right.key;
    });

    // Copy only after every document-bound validation and metadata derivation
    // has succeeded. These raw TopoShapes deliberately carry no document
    // hasher or source element-map ownership into the detached task.
    Part::TopoShape baseSnapshot = captureShape(
        liveBaseShape,
        "base",
        probe ? &probe->baseSnapshotIndependent : nullptr);
    Part::TopoShape toolSnapshot = captureShape(
        liveToolShape,
        "tool",
        probe ? &probe->toolSnapshotIndependent : nullptr);
    const double fuzzySetting = Part::FuzzyHelper::getBooleanFuzzy();
    const double confusion = Precision::Confusion();
    App::CollaborativeOperationPreparation::DetachedTask task;
    if (probe) {
        task = [baseReference = std::move(baseReference),
                toolReference = std::move(toolReference),
                resultReference = std::move(resultReference),
                kind,
                fuzzySetting,
                confusion,
                baseSnapshot = std::move(baseSnapshot),
                toolSnapshot = std::move(toolSnapshot),
                probe = std::move(probe)](std::stop_token stopToken) {
            if (stopToken.stop_requested()) {
                probe->cancellationObserved.store(true, std::memory_order_relaxed);
                throw std::runtime_error("Part Boolean detached preparation was cancelled");
            }
            Part::TopoShape computedResult = computeBoolean(kind,
                                                            baseSnapshot,
                                                            toolSnapshot,
                                                            stopToken,
                                                            fuzzySetting,
                                                            confusion,
                                                            probe.get());
            if (stopToken.stop_requested()) {
                probe->cancellationObserved.store(true, std::memory_order_relaxed);
                throw std::runtime_error("Part Boolean detached preparation was cancelled");
            }
            return std::make_unique<const CollaborativeBooleanOperation>(
                baseReference,
                toolReference,
                resultReference,
                std::move(computedResult),
                probe.get());
        };
    }
    else {
        App::GeometryArchive input;
        input.sections = {{"base.brep", encodeShape(baseSnapshot.getShape())},
                          {"tool.brep", encodeShape(toolSnapshot.getShape())}};
        std::vector<std::uint8_t> parameters {encodeBooleanKind(kind)};
        appendDouble(parameters, fuzzySetting);
        appendDouble(parameters, confusion);
        input.sections.push_back({"parameters", std::move(parameters)});

        App::GeometryJobRequest request;
        request.operationType = std::string(Part::CollaborativeBooleanOperationType);
        request.policy = App::PreparationPolicy::IsolatedProcess;
        request.coalescingKey = resultReference.identity;
        request.inputDigest = sectionDigest(input.sections);
        request.coalescing = App::GeometryJobCoalescing::LatestWins;
        request.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);

        App::CollaborativeOperationPreparation::IsolatedTask isolated {
            std::move(request),
            std::move(input),
            [baseReference = std::move(baseReference),
             toolReference = std::move(toolReference),
             resultReference = std::move(resultReference)](
                const App::GeometryArchive& output) mutable {
                return decodeBooleanResult(output,
                                           std::move(baseReference),
                                           std::move(toolReference),
                                           std::move(resultReference));
            }};
        return {std::move(reads),
                std::move(writes),
                std::move(effects),
                std::move(isolated)};
    }

    return {std::move(reads),
            std::move(writes),
            std::move(effects),
            std::move(task)};
}

App::CollaborativeOperationPreparation prepareBoolean(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent)
{
    return prepareBooleanImpl(document, intent, {});
}

}  // namespace

void Part::ensureCollaborativeBooleanOperationRegistered()
{
    static std::once_flag registered;
    std::call_once(registered, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(CollaborativeBooleanOperationType),
            prepareBoolean));
        App::Internal::GeometryWorkerOperationRegistry::instance().registerOperation(
            std::string(CollaborativeBooleanOperationType),
            executeBooleanArchive);
    });
}

App::CollaborativeOperationPreparation
Part::Internal::prepareCollaborativeBooleanForTests(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent,
    std::shared_ptr<CollaborativeBooleanTaskProbe> probe)
{
    if (!probe) {
        throw std::invalid_argument("Part Boolean test probe is required");
    }
    return prepareBooleanImpl(document, intent, std::move(probe));
}

Part::Internal::CollaborativeBooleanDigest
Part::Internal::collaborativeBooleanShapeDigestForTests(const TopoDS_Shape& shape)
{
    return collaborativeGeometryShapeDigest(shape);
}

Part::Internal::CollaborativeBooleanDigest
Part::Internal::collaborativeGeometryShapeDigest(const TopoDS_Shape& shape)
{
    return canonicalShapeDigest(shape);
}
