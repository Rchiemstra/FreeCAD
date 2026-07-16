"""Experimental deterministic ZIP repacking (non-destructive, separate from sidecars)."""

from __future__ import annotations

import hashlib
import io
import struct
import time
import zipfile
from pathlib import Path

from .errors import UnsafeArchiveError


def repack_deterministic(
    source: Path,
    output: Path | None = None,
    compression: int = zipfile.ZIP_DEFLATED,
) -> bytes:
    """
    Repack a ZIP archive with deterministic metadata.

    This is an experimental utility demonstrating that identical entry payloads
    can produce byte-identical repacked archives. It does NOT stabilize semantic
    payloads and must not be used to rewrite authoritative .FCStd files in normal workflows.
    """
    source = source.resolve()
    if not source.is_file():
        raise UnsafeArchiveError(f"not a file: {source}")

    try:
        with zipfile.ZipFile(source, "r") as zf:
            entries = []
            for info in zf.infolist():
                data = zf.read(info.filename)
                entries.append((info.filename.replace("\\", "/"), data))
    except zipfile.BadZipFile as exc:
        raise UnsafeArchiveError(f"not a valid ZIP: {exc}") from exc

    # Sort entries deterministically
    entries.sort(key=lambda e: e[0])

    buf = io.BytesIO()
    fixed_time = (1980, 1, 1, 0, 0, 0)

    with zipfile.ZipFile(buf, "w", compression=compression) as out:
        for name, data in entries:
            info = zipfile.ZipInfo(filename=name)
            info.date_time = fixed_time
            info.compress_type = compression
            info.external_attr = 0o644 << 16
            out.writestr(info, data, compress_type=compression)

    result = buf.getvalue()

    if output is not None:
        output.write_bytes(result)

    return result


def repack_hash(source: Path) -> str:
    """Return SHA-256 of deterministically repacked archive."""
    data = repack_deterministic(source)
    return hashlib.sha256(data).hexdigest()
