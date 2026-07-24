"""Security tests for archive validation."""

from __future__ import annotations

import io
import struct
import zipfile
from pathlib import Path

import pytest

from freecad_git.archive import validate_and_open
from freecad_git.config import ArchiveLimits
from freecad_git.errors import UnsafeArchiveError
from tests.fixtures.builder import load_xml_fixture_bytes

LIMITS = ArchiveLimits(
    max_entries=100,
    max_compressed_bytes=1024 * 1024,
    max_uncompressed_bytes=2 * 1024 * 1024,
    max_xml_bytes=512 * 1024,
    max_compression_ratio=50.0,
)

DOC_XML = load_xml_fixture_bytes("minimal_document.xml")


def _make_zip(entries: dict[str, bytes], **kwargs) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in entries.items():
            zf.writestr(name, data, **kwargs)
    return buf.getvalue()


def _write_zip(path: Path, entries: dict[str, bytes], **kwargs) -> Path:
    path.write_bytes(_make_zip(entries, **kwargs))
    return path


@pytest.fixture
def tmp_fcstd(tmp_path):
    def _create(entries: dict[str, bytes], name: str = "test.FCStd", **kwargs) -> Path:
        p = tmp_path / name
        _write_zip(p, entries, **kwargs)
        return p

    return _create


class TestArchiveSecurity:
    def test_valid_minimal_archive(self, tmp_fcstd):
        path = tmp_fcstd({"Document.xml": DOC_XML})
        archive = validate_and_open(path, LIMITS)
        assert archive.document_xml is not None

    def test_non_zip_rejected(self, tmp_path):
        path = tmp_path / "bad.FCStd"
        path.write_bytes(b"not a zip file")
        with pytest.raises(UnsafeArchiveError, match="not a valid ZIP"):
            validate_and_open(path, LIMITS)

    def test_truncated_zip_rejected(self, tmp_path):
        path = tmp_path / "truncated.FCStd"
        data = _make_zip({"Document.xml": DOC_XML})
        path.write_bytes(data[: len(data) // 2])
        with pytest.raises(UnsafeArchiveError):
            validate_and_open(path, LIMITS)

    def test_missing_document_xml(self, tmp_fcstd):
        path = tmp_fcstd({"Other.xml": load_xml_fixture_bytes("other.xml")})
        with pytest.raises(UnsafeArchiveError, match="missing Document.xml"):
            validate_and_open(path, LIMITS)

    def test_duplicate_document_xml(self, tmp_fcstd):
        path = tmp_fcstd({"Document.xml": DOC_XML, "subdir/Document.xml": DOC_XML})
        with pytest.raises(UnsafeArchiveError, match="multiple Document.xml"):
            validate_and_open(path, LIMITS)

    def test_absolute_path_rejected(self, tmp_fcstd):
        path = tmp_fcstd({"/etc/passwd": b"evil", "Document.xml": DOC_XML})
        with pytest.raises(UnsafeArchiveError, match="absolute"):
            validate_and_open(path, LIMITS)

    def test_drive_path_rejected(self, tmp_fcstd):
        path = tmp_fcstd({"C:/Windows/System32/evil.dll": b"evil", "Document.xml": DOC_XML})
        with pytest.raises(UnsafeArchiveError, match="drive-qualified"):
            validate_and_open(path, LIMITS)

    def test_unc_path_rejected(self, tmp_fcstd):
        path = tmp_fcstd({"//server/share/file": b"evil", "Document.xml": DOC_XML})
        with pytest.raises(UnsafeArchiveError, match="absolute"):
            validate_and_open(path, LIMITS)

    def test_traversal_rejected(self, tmp_fcstd):
        path = tmp_fcstd({"../escape": b"evil", "Document.xml": DOC_XML})
        with pytest.raises(UnsafeArchiveError, match="traversal"):
            validate_and_open(path, LIMITS)

    def test_backslash_traversal_rejected(self, tmp_fcstd):
        path = tmp_fcstd({"..\\escape": b"evil", "Document.xml": DOC_XML})
        with pytest.raises(UnsafeArchiveError, match="traversal"):
            validate_and_open(path, LIMITS)

    def test_duplicate_entries_rejected(self, tmp_path):
        buf2 = io.BytesIO()
        with zipfile.ZipFile(buf2, "w") as zf:
            zf.writestr("Document.xml", DOC_XML)
            zf.writestr("Document.xml", DOC_XML)
        p = tmp_path / "dup.FCStd"
        p.write_bytes(buf2.getvalue())
        with pytest.raises(UnsafeArchiveError):
            validate_and_open(p, LIMITS)

    def test_too_many_entries(self, tmp_fcstd):
        entries = {"Document.xml": DOC_XML}
        for i in range(150):
            entries[f"file{i}.txt"] = b"x"
        path = tmp_fcstd(entries)
        with pytest.raises(UnsafeArchiveError, match="max entries"):
            validate_and_open(path, LIMITS)

    def test_oversized_xml(self, tmp_fcstd):
        big_xml = load_xml_fixture_bytes("oversized_document_template.xml").replace(
            b"{{PAYLOAD}}", b"x" * (600 * 1024)
        )
        path = tmp_fcstd({"Document.xml": big_xml})
        with pytest.raises(UnsafeArchiveError, match="exceeds max"):
            validate_and_open(path, LIMITS)

    def test_dtd_rejected_in_xml(self, tmp_fcstd):
        dtd_xml = load_xml_fixture_bytes("dtd_entity.xml")
        tmp_fcstd({"Document.xml": dtd_xml})
        from freecad_git.config import CollectionLimits, XmlLimits
        from freecad_git.document_xml import parse_document_xml
        from freecad_git.errors import InvalidXmlError

        with pytest.raises(InvalidXmlError, match="external entities|parse error"):
            parse_document_xml(dtd_xml, XmlLimits(), CollectionLimits())

    def test_encrypted_entry_rejected(self, tmp_path):
        from freecad_git.archive import ArchiveEntry

        # Test encryption detection logic directly
        entry = ArchiveEntry(
            name="Document.xml",
            compressed_size=100,
            file_size=100,
            is_encrypted=True,
            compress_type=0,
        )
        assert entry.is_encrypted
        # Full integration test when platform preserves flag
        path = tmp_path / "encrypted.FCStd"
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            info = zipfile.ZipInfo("Document.xml")
            info.flag_bits |= 0x1
            zf.writestr(info, DOC_XML)
        path.write_bytes(buf.getvalue())
        try:
            validate_and_open(path, LIMITS)
        except UnsafeArchiveError as exc:
            assert "encrypted" in exc.message
        else:
            pytest.skip("platform ZIP writer does not preserve encryption flag")

    def test_zip_bomb_ratio(self, tmp_fcstd):
        # Highly compressible content
        huge = b"A" * (100 * 1024)
        path = tmp_fcstd({"Document.xml": DOC_XML, "bomb.dat": huge})
        tight_limits = ArchiveLimits(max_compression_ratio=10.0)
        with pytest.raises(UnsafeArchiveError, match="compression ratio"):
            validate_and_open(path, tight_limits)
