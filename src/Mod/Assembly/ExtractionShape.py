# SPDX-License-Identifier: LGPL-2.1-or-later
"""Shape-valued expressions need a real shape property in the assembly frame."""

import FreeCAD as App
import Part


class InstanceShape:
    def execute(self, obj):
        shape = Part.getShape(obj.Source)
        shape.Placement = obj.Placement
        obj.Shape = shape

    def supportsDocumentThreadExecution(self, obj):
        return True

    def dumps(self):
        return None

    def loads(self, state):
        pass


def create(document, definition, instance):
    obj = document.addObject("Part::FeaturePython", "ExtractedInstanceShape")
    obj.Label = App.Qt.translate("Assembly", "Component shape")
    group = App.Qt.translate("Assembly", "Extracted component")
    obj.addProperty("App::PropertyXLink", "Source", group)
    obj.addProperty("App::PropertyLinkGlobal", "Instance", group)
    obj.Source = definition
    obj.Instance = instance
    obj.setExpression("Placement", f"{instance.Name}.Placement")
    obj.Proxy = InstanceShape()
    obj.Visibility = False
    return obj


class ReferenceFrame:
    def supportsDocumentThreadExecution(self, obj):
        return True

    def execute(self, obj):
        obj.Result = (
            _globalPlacement(obj.Instance)
            * _globalPlacement(obj.DefinitionRoot).inverse()
            * _globalPlacement(obj.Source)
        )

    def dumps(self):
        return None

    def loads(self, state):
        pass


def _frames(obj):
    while obj is not None:
        yield obj
        obj = obj.getParentGeoFeatureGroup()


def _globalPlacement(obj):
    placement = App.Placement()
    for frame in reversed(list(_frames(obj))):
        if "Placement" in frame.PropertiesList:
            placement = placement * frame.Placement
    return placement


def createFrame(document, definition, source, instance, parent):
    obj = document.addObject("App::FeaturePython", "ExtractedReferenceFrame")
    obj.Label = App.Qt.translate("Assembly", "Component reference frame")
    group = App.Qt.translate("Assembly", "Extracted component")
    obj.addProperty("App::PropertyXLink", "DefinitionRoot", group)
    obj.addProperty("App::PropertyXLink", "Source", group)
    obj.addProperty("App::PropertyLinkGlobal", "Instance", group)
    obj.addProperty("App::PropertyPlacement", "Result", group)
    obj.setPropertyStatus("Result", "Output")
    obj.DefinitionRoot, obj.Source, obj.Instance = definition, source, instance
    watched = set()
    # Parent containers own consumers too. Hidden references subscribe to their
    # placements without adding a dependency cycle through the container Group.
    for start, hidden in ((instance, False), (parent, True), (source, False)):
        for frame in _frames(start):
            if frame in watched or "Placement" not in frame.PropertiesList:
                continue
            watched.add(frame)
            prop = f"Frame{len(watched)}"
            obj.addProperty("App::PropertyPlacement", prop, group)
            expression = f"{frame.Document.Name}#{frame.Name}.Placement"
            obj.setExpression(
                prop, f"hiddenref({expression})" if hidden else expression
            )
            obj.setEditorMode(prop, 1)
    obj.Proxy = ReferenceFrame()
    return obj
