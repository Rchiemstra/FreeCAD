"""Safe FCStd archive validation and reading."""

from __future__ import annotations

import io
import posixpath
import re
import struct
import zipfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import BinaryIO
from zipfile import ZipInfo

from .config import ArchiveLimits
from .errors import UnsafeArchiveError

# ZIP constants
_ZIP64_EXTRA_TAG = 1
_MAX_FILENAME_LEN = 4096


@dataclass
class ArchiveEntry:
    name: str
    compressed_size: int
    file_size: int
    is_encrypted: bool
    compress_type: int


@dataclass
class SafeArchive:
    """Validated in-memory view of a FCStd archive."""

    path: Path
    entries: dict[str, ArchiveEntry] = field(default_factory=dict)
    compressed_total: int = 0
    uncompressed_total: int = 0
    document_xml: bytes | None = None
    gui_document_xml: bytes | None = None

    def read_entry(self, name: str) -> bytes:
        if name not in self.entries:
            raise UnsafeArchiveError(f"archive entry not found: {name}")
        with zipfile.ZipFile(self.path, "r") as zf:
            return _read_zip_entry(zf, name)


def _normalize_entry_name(name: str) -> str:
    """Normalize a ZIP entry name for security checks."""
    # Convert backslashes
    normalized = name.replace("\\", "/")
    # Remove leading ./
    while normalized.startswith("./"):
        normalized = normalized[2:]
    # Collapse //
    while "//" in normalized:
        normalized = normalized.replace("//", "/")
    return normalized


def _is_unsafe_path(name: str) -> str | None:
    """Return reason if path is unsafe, else None."""
    if not name or name.strip() != name:
        return "empty or ambiguous normalized name"
    normalized = _normalize_entry_name(name)

    if not normalized:
        return "empty normalized name"

    if normalized.startswith("/"):
        return "absolute archive path"

    if re.match(r"^[A-Za-z]:", normalized):
        return "drive-qualified path"

    if normalized.startswith("//"):
        return "UNC-style path"

    # Check traversal
    parts = PurePosixPath(normalized).parts
    if ".." in parts:
        return "path traversal"

    if len(normalized) > _MAX_FILENAME_LEN:
        return "entry name too long"

    return None


def _check_zip64_extra(extra: bytes) -> tuple[int | None, int | None]:
    """Parse ZIP64 extra field for uncompressed/compressed sizes."""
    offset = 0
    uncomp = None
    comp = None
    while offset + 4 <= len(extra):
        tag, size = struct.unpack_from("<HH", extra, offset)
        offset += 4
        if offset + size > len(extra):
            break
        data = extra[offset : offset + size]
        offset += size
        if tag == _ZIP64_EXTRA_TAG:
            pos = 0
            if size >= 8:
                uncomp = struct.unpack_from("<Q", data, pos)[0]
                pos += 8
            if size >= 16:
                comp = struct.unpack_from("<Q", data, pos)[0]
    return uncomp, comp


def _read_zip_entry(zf: zipfile.ZipFile, name: str) -> bytes:
    """Read a ZIP entry, mapping low-level failures to UnsafeArchiveError."""
    try:
        return zf.read(name)
    except zipfile.BadZipFile as exc:
        raise UnsafeArchiveError(f"corrupt archive entry {name!r}: {exc}") from exc
    except RuntimeError as exc:
        raise UnsafeArchiveError(f"corrupt archive entry {name!r}: {exc}") from exc
    except NotImplementedError as exc:
        raise UnsafeArchiveError(f"unsupported compression for entry {name!r}: {exc}") from exc


def validate_and_open(path: Path, limits: ArchiveLimits) -> SafeArchive:
    """Validate a FCStd archive and return a safe handle."""
    path = path.resolve()
    if not path.is_file():
        raise UnsafeArchiveError(f"not a file: {path}")

    compressed_total = path.stat().st_size
    if compressed_total > limits.max_compressed_bytes:
        raise UnsafeArchiveError(
            f"archive exceeds max compressed size: {compressed_total} > {limits.max_compressed_bytes}"
        )

    try:
        zf = zipfile.ZipFile(path, "r")
    except zipfile.BadZipFile as exc:
        raise UnsafeArchiveError(f"not a valid ZIP archive: {exc}") from exc

    archive = SafeArchive(path=path)
    seen_normalized: dict[str, str] = {}
    entry_count = 0
    uncompressed_total = 0

    with zf:
        for info in zf.infolist():
            entry_count += 1
            if entry_count > limits.max_entries:
                raise UnsafeArchiveError(
                    f"archive exceeds max entries: {entry_count} > {limits.max_entries}"
                )

            raw_name = info.filename
            reason = _is_unsafe_path(raw_name)
            if reason:
                raise UnsafeArchiveError(f"unsafe archive entry {raw_name!r}: {reason}")

            norm_name = _normalize_entry_name(raw_name)
            if norm_name in seen_normalized and seen_normalized[norm_name] != raw_name:
                raise UnsafeArchiveError(
                    f"duplicate entry after normalization: {raw_name!r} and {seen_normalized[norm_name]!r}"
                )
            if norm_name in seen_normalized:
                raise UnsafeArchiveError(f"duplicate entry name: {raw_name!r}")
            seen_normalized[norm_name] = raw_name

            # Symlink detection via external attributes (Unix symlink = 0120000 << 16)
            external = info.external_attr
            if external:
                mode = (external >> 16) & 0xFFFF
                if (mode & 0o170000) == 0o120000:
                    raise UnsafeArchiveError(f"symlink entry not allowed: {raw_name!r}")

            is_encrypted = bool(info.flag_bits & 0x1)
            if is_encrypted:
                raise UnsafeArchiveError(f"encrypted entry not allowed: {raw_name!r}")

            file_size = info.file_size
            compressed_size = info.compress_size

            # ZIP64
            if file_size == 0xFFFFFFFF or compressed_size == 0xFFFFFFFF:
                uncomp64, comp64 = _check_zip64_extra(info.extra)
                if uncomp64 is not None:
                    file_size = uncomp64
                if comp64 is not None:
                    compressed_size = comp64

            if file_size > limits.max_xml_bytes and norm_name.endswith(".xml"):
                raise UnsafeArchiveError(
                    f"XML entry exceeds max size: {norm_name} ({file_size} bytes)"
                )

            if compressed_size > 0 and file_size / compressed_size > limits.max_compression_ratio:
                raise UnsafeArchiveError(
                    f"compression ratio too high for {norm_name!r}: "
                    f"{file_size}/{compressed_size}"
                )

            uncompressed_total += file_size
            if uncompressed_total > limits.max_uncompressed_bytes:
                raise UnsafeArchiveError(
                    f"archive exceeds max uncompressed size: "
                    f"{uncompressed_total} > {limits.max_uncompressed_bytes}"
                )

            archive.entries[norm_name] = ArchiveEntry(
                name=norm_name,
                compressed_size=compressed_size,
                file_size=file_size,
                is_encrypted=is_encrypted,
                compress_type=info.compress_type,
            )

    archive.compressed_total = compressed_total
    archive.uncompressed_total = uncompressed_total

    # Require exactly one Document.xml (at archive root only)
    doc_entries = [n for n in archive.entries if n == "Document.xml" or n.endswith("/Document.xml")]
    root_docs = [n for n in archive.entries if n == "Document.xml"]
    if len(root_docs) == 0:
        raise UnsafeArchiveError("missing Document.xml")
    if len(doc_entries) > 1:
        raise UnsafeArchiveError("multiple Document.xml entries")
    doc_name = root_docs[0]
    try:
        with zipfile.ZipFile(path, "r") as zf:
            archive.document_xml = _read_zip_entry(zf, doc_name)
    except zipfile.BadZipFile as exc:
        raise UnsafeArchiveError(f"corrupt archive while reading Document.xml: {exc}") from exc
    except RuntimeError as exc:
        raise UnsafeArchiveError(f"corrupt archive while reading Document.xml: {exc}") from exc
    except NotImplementedError as exc:
        raise UnsafeArchiveError(f"unsupported archive compression: {exc}") from exc

    if len(archive.document_xml) > limits.max_xml_bytes:
        raise UnsafeArchiveError(
            f"Document.xml exceeds max size: {len(archive.document_xml)} bytes"
        )

    # Optionally read GuiDocument.xml
    gui_entries = [n for n in archive.entries if n.lower() == "guidocument.xml"]
    if len(gui_entries) == 1:
        try:
            with zipfile.ZipFile(path, "r") as zf:
                archive.gui_document_xml = _read_zip_entry(zf, gui_entries[0])
        except UnsafeArchiveError:
            raise

    return archive


def read_entry_bytes(path: Path, entry_name: str, limits: ArchiveLimits) -> bytes:
    """Read a single entry after validation."""
    archive = validate_and_open(path, limits)
    if entry_name not in archive.entries:
        raise UnsafeArchiveError(f"entry not found: {entry_name}")
    entry = archive.entries[entry_name]
    if entry.file_size > limits.max_xml_bytes:
        raise UnsafeArchiveError(f"entry too large: {entry_name}")
    with zipfile.ZipFile(path, "r") as zf:
        return _read_zip_entry(zf, entry_name)
