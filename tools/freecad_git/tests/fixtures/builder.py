"""Build minimal FCStd fixtures for testing."""

from __future__ import annotations

import io
import zipfile
from pathlib import Path


XML_FIXTURES_DIR = Path(__file__).with_name("xml")


def load_xml_fixture(name: str) -> str:
    """Load a UTF-8 XML fixture by filename."""
    return (XML_FIXTURES_DIR / name).read_text(encoding="utf-8")


def load_xml_fixture_bytes(name: str) -> bytes:
    """Load an XML fixture as raw bytes."""
    return (XML_FIXTURES_DIR / name).read_bytes()


def _make_zip(entries: dict[str, str | bytes], timestamp: tuple | None = None) -> bytes:
    """Create a ZIP archive from entry dict."""
    buf = io.BytesIO()
    ts = timestamp or (2024, 1, 1, 12, 0, 0)
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for name in sorted(entries.keys()):
            data = entries[name]
            if isinstance(data, str):
                data = data.encode("utf-8")
            info = zipfile.ZipInfo(filename=name)
            info.date_time = ts
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, data)
    return buf.getvalue()


BASIC_DOCUMENT_XML = load_xml_fixture("basic_document.xml")
SPREADSHEET_DOCUMENT_XML = load_xml_fixture("spreadsheet_document.xml")
PLACEMENT_DOCUMENT_XML = load_xml_fixture("placement_document.xml")
PARTDESIGN_DOCUMENT_XML = load_xml_fixture("partdesign_document.xml")
LINK_LIST_DOCUMENT_XML = load_xml_fixture("link_list_document.xml")
EXTERNAL_LINK_DOCUMENT_XML = load_xml_fixture("external_link_document.xml")
GUI_DOCUMENT_XML = load_xml_fixture("gui_document.xml")
CUSTOM_PROPERTY_DOCUMENT_XML = load_xml_fixture("custom_property_document.xml")


def write_fixture(
    path: Path,
    document_xml: str,
    gui_xml: str | None = None,
    timestamp: tuple | None = None,
) -> None:
    """Write an FCStd fixture file."""
    entries: dict[str, str | bytes] = {"Document.xml": document_xml}
    if gui_xml:
        entries["GuiDocument.xml"] = gui_xml
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(_make_zip(entries, timestamp))


def build_all_fixtures(fixtures_dir: Path) -> None:
    """Build all standard test fixtures."""
    write_fixture(fixtures_dir / "basic.FCStd", BASIC_DOCUMENT_XML)
    write_fixture(fixtures_dir / "spreadsheet.FCStd", SPREADSHEET_DOCUMENT_XML)
    write_fixture(fixtures_dir / "placement.FCStd", PLACEMENT_DOCUMENT_XML)
    write_fixture(fixtures_dir / "partdesign.FCStd", PARTDESIGN_DOCUMENT_XML)
    write_fixture(fixtures_dir / "link_list.FCStd", LINK_LIST_DOCUMENT_XML)
    write_fixture(fixtures_dir / "external_link.FCStd", EXTERNAL_LINK_DOCUMENT_XML)
    write_fixture(fixtures_dir / "gui_only.FCStd", BASIC_DOCUMENT_XML, GUI_DOCUMENT_XML)
    write_fixture(fixtures_dir / "custom_property.FCStd", CUSTOM_PROPERTY_DOCUMENT_XML)
    write_fixture(
        fixtures_dir / "basic_timestamp_b.FCStd",
        BASIC_DOCUMENT_XML,
        timestamp=(2025, 6, 15, 8, 30, 0),
    )
    write_fixture(
        fixtures_dir / "basic_timestamp_a.FCStd",
        BASIC_DOCUMENT_XML,
        timestamp=(2024, 1, 1, 12, 0, 0),
    )
