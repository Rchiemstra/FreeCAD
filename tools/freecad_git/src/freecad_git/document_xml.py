"""Hardened XML parsing for Document.xml and GuiDocument.xml."""

from __future__ import annotations

import xml.sax
from typing import Any
from xml.sax.handler import ContentHandler
from xml.sax.xmlreader import AttributesImpl

from defusedxml import sax as defused_sax
from defusedxml.common import EntitiesForbidden

from .config import CollectionLimits, XmlLimits
from .errors import InvalidXmlError


class _DepthTrackingHandler(ContentHandler):
    """Base SAX handler tracking depth and size limits."""

    def __init__(self, xml_limits: XmlLimits, collection_limits: CollectionLimits) -> None:
        super().__init__()
        self.xml_limits = xml_limits
        self.collection_limits = collection_limits
        self.depth = 0
        self.text_size = 0
        self.property_count = 0
        self.object_count = 0

    def startElement(self, name: str, attrs: AttributesImpl) -> None:
        self.depth += 1
        if self.depth > self.xml_limits.max_depth:
            raise InvalidXmlError(f"XML nesting depth exceeds limit: {self.depth}")
        if len(attrs) > self.xml_limits.max_attributes:
            raise InvalidXmlError(f"too many attributes on <{name}>")

    def endElement(self, name: str) -> None:
        self.depth -= 1

    def characters(self, content: str) -> None:
        self.text_size += len(content.encode("utf-8"))
        if self.text_size > self.xml_limits.max_text_bytes:
            raise InvalidXmlError("XML text content exceeds size limit")


class DocumentXmlParser(_DepthTrackingHandler):
    """Parse Document.xml into structured element trees."""

    def __init__(self, xml_limits: XmlLimits, collection_limits: CollectionLimits) -> None:
        super().__init__(xml_limits, collection_limits)
        self.document_attrs: dict[str, str] = {}
        self.doc_properties: list[dict[str, Any]] = []
        self.object_types: dict[str, str] = {}
        self.object_data: dict[str, dict[str, Any]] = {}
        self._current_object: str | None = None
        self._current_property: dict[str, Any] | None = None
        self._element_stack: list[dict[str, Any]] = []
        self._text_buffer: list[str] = []
        self._in_doc_properties = False
        self._in_objects = False
        self._in_object_data = False
        self._in_properties = False
        self._depth_extensions = 0

    def startElement(self, name: str, attrs: AttributesImpl) -> None:
        super().startElement(name, attrs)
        attr_dict = {k: v for k, v in attrs.items()}

        if name == "Document":
            self.document_attrs = attr_dict
        elif name == "Properties":
            if self._in_object_data and self._current_object:
                self._in_properties = True
                self.property_count = 0
            elif not self._in_object_data:
                self._in_doc_properties = True
        elif name == "Objects":
            self._in_objects = True
        elif name == "Object" and self._in_objects and not self._in_object_data:
            obj_name = attr_dict.get("name", "")
            obj_type = attr_dict.get("type", "")
            if obj_name:
                self.object_types[obj_name] = obj_type
                self.object_count += 1
                if self.object_count > self.collection_limits.max_objects:
                    raise InvalidXmlError("object count exceeds limit")
        elif name == "ObjectData":
            self._in_object_data = True
        elif name == "Object" and self._in_object_data:
            obj_name = attr_dict.get("name", "")
            if obj_name:
                self._current_object = obj_name
                self.object_data.setdefault(obj_name, {"properties": []})
        elif name in ("Extensions", "Extension") and self._in_object_data:
            self._depth_extensions += 1
        elif name == "Property" and self._in_properties:
            self.property_count += 1
            if self.property_count > self.xml_limits.max_properties_per_object:
                raise InvalidXmlError("property count exceeds limit per object")
            self._current_property = {
                "name": attr_dict.get("name", ""),
                "type": attr_dict.get("type", ""),
                "status": attr_dict.get("status"),
                "elements": [],
            }
            self._element_stack = []
            self._text_buffer = []
        elif self._current_property is not None and self._depth_extensions == 0:
            self._text_buffer = []
            elem: dict[str, Any] = {"tag": name, "attrs": attr_dict, "children": [], "text": ""}
            if self._element_stack:
                self._element_stack[-1]["children"].append(elem)
            else:
                self._current_property["elements"].append(elem)
            self._element_stack.append(elem)

    def characters(self, content: str) -> None:
        super().characters(content)
        if self._element_stack:
            self._text_buffer.append(content)

    def endElement(self, name: str) -> None:
        if name in ("Extensions", "Extension") and self._depth_extensions > 0:
            self._depth_extensions -= 1
        elif self._element_stack and self._element_stack[-1]["tag"] == name:
            elem = self._element_stack.pop()
            elem["text"] = "".join(self._text_buffer)
            self._text_buffer = []
        elif name == "Property" and self._current_property is not None:
            if self._current_object:
                self.object_data[self._current_object]["properties"].append(self._current_property)
            else:
                self.doc_properties.append(self._current_property)
            self._current_property = None
            self._element_stack = []
            self._text_buffer = []
        elif name == "Object" and self._in_object_data:
            self._current_object = None
            self._in_properties = False
        elif name == "Properties":
            self._in_properties = False
            self._in_doc_properties = False
        elif name == "Objects":
            self._in_objects = False
        elif name == "ObjectData":
            self._in_object_data = False
        super().endElement(name)


class GuiDocumentParser(_DepthTrackingHandler):
    """Parse GuiDocument.xml for presentation profile data."""

    def __init__(self, xml_limits: XmlLimits, collection_limits: CollectionLimits) -> None:
        super().__init__(xml_limits, collection_limits)
        self.object_visibility: dict[str, bool] = {}
        self._current_object: str | None = None
        self._in_object_data = False
        self._in_view_provider_data = False
        self._in_properties = False
        self._current_property_name: str | None = None

    def startElement(self, name: str, attrs: AttributesImpl) -> None:
        super().startElement(name, attrs)
        attr_dict = {k: v for k, v in attrs.items()}
        if name == "ObjectData":
            self._in_object_data = True
        elif name == "ViewProviderData":
            self._in_view_provider_data = True
        elif name == "Object" and self._in_object_data:
            self._current_object = attr_dict.get("name")
        elif name == "ViewProvider" and self._in_view_provider_data:
            self._current_object = attr_dict.get("name")
        elif name == "Properties":
            self._in_properties = True
        elif name == "Property" and self._in_properties:
            self._current_property_name = attr_dict.get("name")
        elif name == "Bool" and self._current_property_name == "Visibility":
            if self._current_object:
                value = attr_dict.get("value", "true").strip().lower()
                self.object_visibility[self._current_object] = value == "true"

    def endElement(self, name: str) -> None:
        if name == "Property":
            self._current_property_name = None
        elif name == "Properties":
            self._in_properties = False
        elif name in ("Object", "ViewProvider"):
            self._current_object = None
        elif name == "ObjectData":
            self._in_object_data = False
        elif name == "ViewProviderData":
            self._in_view_provider_data = False
        super().endElement(name)


def parse_document_xml(
    data: bytes,
    xml_limits: XmlLimits,
    collection_limits: CollectionLimits,
) -> DocumentXmlParser:
    """Parse Document.xml bytes with hardened SAX parser."""
    parser = defused_sax.make_parser()
    handler = DocumentXmlParser(xml_limits, collection_limits)
    parser.setContentHandler(handler)
    try:
        parser.feed(data.decode("utf-8"))
        parser.close()
    except EntitiesForbidden as exc:
        raise InvalidXmlError(f"XML external entities are not allowed: {exc}") from exc
    except xml.sax.SAXParseException as exc:
        raise InvalidXmlError(f"XML parse error: {exc}") from exc
    except UnicodeDecodeError as exc:
        raise InvalidXmlError(f"Document.xml is not valid UTF-8: {exc}") from exc
    return handler


def parse_gui_document_xml(
    data: bytes,
    xml_limits: XmlLimits,
    collection_limits: CollectionLimits,
) -> GuiDocumentParser:
    """Parse GuiDocument.xml bytes."""
    parser = defused_sax.make_parser()
    handler = GuiDocumentParser(xml_limits, collection_limits)
    parser.setContentHandler(handler)
    try:
        parser.feed(data.decode("utf-8"))
        parser.close()
    except EntitiesForbidden as exc:
        raise InvalidXmlError(f"XML external entities are not allowed: {exc}") from exc
    except xml.sax.SAXParseException as exc:
        raise InvalidXmlError(f"GuiDocument.xml parse error: {exc}") from exc
    return handler
