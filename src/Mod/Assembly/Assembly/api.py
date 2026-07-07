# SPDX-License-Identifier: LGPL-2.1-or-later

import FreeCAD as App


class JointCreationError(ValueError):
    """Raised when a scripted Assembly joint request is invalid."""


def createAssembly(doc=None, name="Assembly", *, createJointGroup=True, recompute=False):
    """Create an Assembly object in a document."""
    doc = _require_document(doc)

    assembly = doc.addObject("Assembly::AssemblyObject", name)
    assembly.Type = "Assembly"

    if createJointGroup:
        import UtilsAssembly

        UtilsAssembly.getJointGroup(assembly)

    if recompute:
        doc.recompute()

    return assembly


def makeJointReference(component, element="", vertex=None):
    """Create a component-rooted joint reference."""
    component = _require_component(component)
    element = _normalize_subname(element, "element")
    vertex = element if vertex is None else _normalize_subname(vertex, "vertex")

    return [component, [element, vertex]]


def referenceFromSelection(assembly, rootObj, subName, vertexSubName=None):
    """Convert a GUI-style selection path into a component-rooted joint reference."""
    assembly = _require_assembly(assembly)
    rootObj = _require_component(rootObj, assembly)
    element_component, element = _component_reference_from_selection(
        assembly, rootObj, subName, "subName"
    )

    if vertexSubName is None:
        vertex_component = element_component
        vertex = element
    else:
        vertex_component, vertex = _component_reference_from_selection(
            assembly, rootObj, vertexSubName, "vertexSubName"
        )

    if vertex_component != element_component:
        raise JointCreationError("Selection element and vertex resolve to different components")

    return makeJointReference(element_component, element, vertex)


def createJoint(
    assembly,
    jointType,
    ref1,
    ref2,
    *,
    label=None,
    solve=True,
    presolve=True,
    recompute=True,
    **properties,
):
    """Create a joint from explicit component-rooted references."""
    import JointObject
    import UtilsAssembly

    assembly = _require_assembly(assembly)
    ref1 = _normalize_reference(ref1, assembly, "ref1")
    ref2 = _normalize_reference(ref2, assembly, "ref2")
    type_index = _joint_type_index(jointType, JointObject.JointTypes)

    if recompute:
        assembly.Document.recompute()

    if getattr(assembly, "Type", None) == "Assembly":
        assembly.ensureIdentityPlacements()

    joint_group = UtilsAssembly.getJointGroup(assembly)
    joint = joint_group.newObject("App::FeaturePython", "Joint")
    JointObject.Joint(joint, type_index)
    joint.Label = JointObject.JointTypes[type_index] if label is None else label
    _attach_joint_view_provider(joint, grounded=False)

    _apply_joint_properties(joint, properties)
    joint.Proxy.setJointConnectors(joint, [ref1, ref2], solve=bool(solve), presolve=bool(presolve))

    if recompute:
        assembly.Document.recompute()

    return joint


def createGroundedJoint(assembly, component, *, label=None, recompute=True):
    """Create a grounded joint for a component in an assembly."""
    import JointObject
    import UtilsAssembly

    assembly = _require_assembly(assembly)
    component = _require_component(component, assembly)

    joint_group = UtilsAssembly.getJointGroup(assembly)
    joint = joint_group.newObject("App::FeaturePython", "GroundedJoint")
    JointObject.GroundedJoint(joint, component)
    if label is not None:
        joint.Label = label
    _attach_joint_view_provider(joint, grounded=True)

    if recompute:
        assembly.Document.recompute()

    return joint


def _require_document(doc):
    if doc is None:
        doc = App.ActiveDocument

    if doc is None:
        raise JointCreationError("No active document; pass a document explicitly")

    if not hasattr(doc, "addObject"):
        raise JointCreationError("doc must be a FreeCAD document")

    return doc


def _require_assembly(assembly):
    if assembly is None or not hasattr(assembly, "isDerivedFrom"):
        raise JointCreationError("assembly must be an Assembly::AssemblyObject")

    if not assembly.isDerivedFrom("Assembly::AssemblyObject"):
        raise JointCreationError("assembly must be an Assembly::AssemblyObject")

    if getattr(assembly, "Document", None) is None:
        raise JointCreationError("assembly must belong to a document")

    return assembly


def _require_component(component, assembly=None):
    if component is None or not hasattr(component, "Name"):
        raise JointCreationError("component must be a FreeCAD document object")

    if getattr(component, "Document", None) is None:
        raise JointCreationError("component must belong to a document")

    if assembly is not None and component.Document != assembly.Document:
        raise JointCreationError("component must belong to the same document as the assembly")

    return component


def _normalize_subname(subname, field):
    if subname is None:
        subname = ""

    if not isinstance(subname, str):
        raise JointCreationError(f"{field} must be a string")

    if "?" in subname:
        raise JointCreationError(f"{field} contains an unresolved subelement name")

    return subname


def _normalize_reference(ref, assembly, field):
    if not isinstance(ref, (list, tuple)) or len(ref) != 2:
        raise JointCreationError(f"{field} must be [component, [element, vertex]]")

    component = _require_component(ref[0], assembly)
    subnames = ref[1]
    if not isinstance(subnames, (list, tuple)) or len(subnames) != 2:
        raise JointCreationError(f"{field} must contain exactly two subelement names")

    element = _normalize_subname(subnames[0], f"{field} element")
    vertex = _normalize_subname(subnames[1], f"{field} vertex")

    return [component, [element, vertex]]


def _component_reference_from_selection(assembly, rootObj, subName, field):
    import UtilsAssembly

    subName = _normalize_subname(subName, field)
    component, relative_sub = UtilsAssembly.getComponentReference(assembly, rootObj, subName)

    if component is None:
        component = rootObj
        relative_sub = subName

    component = _require_component(component, assembly)
    relative_sub = _normalize_subname(relative_sub, field)

    return component, relative_sub


def _joint_type_index(jointType, jointTypes):
    if isinstance(jointType, int):
        if jointType < 0 or jointType >= len(jointTypes):
            raise JointCreationError("jointType index is out of range")
        return jointType

    if not isinstance(jointType, str):
        raise JointCreationError("jointType must be a string or index")

    if jointType not in jointTypes:
        expected = ", ".join(jointTypes)
        raise JointCreationError(f"Unsupported joint type '{jointType}'. Expected one of: {expected}")

    return jointTypes.index(jointType)


def _apply_joint_properties(joint, properties):
    property_names = set(getattr(joint, "PropertiesList", []))

    for name, value in properties.items():
        if name == "JointType":
            raise JointCreationError("JointType is controlled by the jointType argument")

        if name not in property_names:
            raise JointCreationError(f"Unknown joint property '{name}'")

        try:
            setattr(joint, name, value)
        except Exception as exc:
            raise JointCreationError(f"Unable to set joint property '{name}': {exc}") from exc


def _attach_joint_view_provider(joint, grounded):
    if not App.GuiUp:
        return

    view_object = getattr(joint, "ViewObject", None)
    if view_object is None:
        return

    import JointObject

    if grounded:
        JointObject.ViewProviderGroundedJoint(view_object)
    else:
        JointObject.ViewProviderJoint(view_object)


__all__ = [
    "JointCreationError",
    "createAssembly",
    "createGroundedJoint",
    "createJoint",
    "makeJointReference",
    "referenceFromSelection",
]
