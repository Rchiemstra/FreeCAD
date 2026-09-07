# SPDX-License-Identifier: LGPL-2.1-or-later
"""Closed enumeration of the propertyStatus mutations that escape Restricted.

``ensurePropertySchemaMutationAllowed`` classifies every propertyStatus change
made across a collaboration stable boundary.  ``Restricted`` is refused; the
``Object`` kind is grantable.  Each predicate that promotes a mutation out of
``Restricted`` widens what an agent commit may do to an object it did not
create, so the set is enumerated here: adding one has to be a deliberate edit
to this list, not a quiet addition to a boolean chain.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RECORDER_SOURCE = "src/App/private/CollaborationStructuralMutationRecorder.cpp"
JOINT_SOURCE = "src/Mod/Assembly/JointObject.py"

#: Every predicate allowed to promote a propertyStatus mutation on a live,
#: pre-existing object out of ``Restricted``.
GRANTED_LIVE_OBJECT_PREDICATES = (
    "removalOwnedStatus",
    "liveSketchAttachmentStatus",
    "groundedJointPlacementLock",
)


def _read(path: str) -> str:
    return (REPO_ROOT / path).read_text(encoding="utf-8", errors="surrogateescape")


def _slice(source: str, start_marker: str, end_marker: str) -> str:
    """Text from ``start_marker`` up to the following ``end_marker``."""
    start = source.index(start_marker)
    return source[start : source.index(end_marker, start + len(start_marker))]


def _compact(text: str) -> str:
    return re.sub(r"\s+", "", text)


def test_only_the_enumerated_predicates_escape_restricted() -> None:
    recorder = _read(RECORDER_SOURCE)
    selection = _compact(
        _slice(
            recorder,
            "const bool removalOwnedStatus",
            'std::string mutation = "propertyStatus on ";',
        )
    )
    granted = "||".join(GRANTED_LIVE_OBJECT_PREDICATES)
    assert f"elseif({granted})" in selection, selection
    # A new object gets the dynamic-property kind; everything else stays
    # Restricted, which is the refused kind.
    assert "kind=Document::CollaborationStructuralMutationKind::Restricted;" in selection
    assert (
        "if(newStructuralObject){kind=Document::CollaborationStructuralMutationKind"
        "::DynamicPropertyOnNewObject;}" in selection
    )
    # Each predicate is gated on the object actually being a live structural
    # object, so a detached or foreign container cannot reach the grant.
    compact_recorder = _compact(recorder)
    for predicate in GRANTED_LIVE_OBJECT_PREDICATES:
        assert f"constbool{predicate}=attachedStructuralObject&&" in compact_recorder


def test_grounded_joint_lock_matches_exactly_one_bit_on_two_named_properties() -> None:
    """Assembly grounds a part by making its placement read-only.

    The write lands on the grounded object, not on the joint the commit
    created, so the new-object grant never covers it.  The capability is
    therefore keyed on the exact mutation: one bit, two property names, one
    property type, and a live joint that actually points back at this object.
    """
    predicate = _compact(
        _slice(
            _read(RECORDER_SOURCE),
            "bool isGroundedJointPlacementLockMutation(",
            "void deferOrEmitDynamicExtension(",
        )
    )
    # Exactly the ReadOnly bit, nothing else in the same change.
    assert "changed!=(1UL<<Property::ReadOnly)" in predicate
    # Exactly the two properties Assembly locks, at their built-in type.
    assert 'name!="Placement"&&name!="LinkPlacement"' in predicate
    assert "property.getTypeId()!=PropertyPlacement::getClassTypeId()" in predicate
    # The property has to be the object's own member, not a same-named alias.
    assert "object.getPropertyByName(propertyName)!=&property" in predicate
    # A live joint in the in-list must point back at this exact object.
    assert 'joint->getPropertyByName("ObjectToGround")' in predicate
    assert "rawGround->getTypeId()==PropertyLinkGlobal::getClassTypeId()" in predicate
    assert "ground->getValue()==&object" in predicate
    assert "joint->isAttachedToDocument()" in predicate


def test_the_capability_matches_what_assembly_actually_writes() -> None:
    """Freeze the premise: the predicate is useless if grounding changes shape."""
    joint = _read(JOINT_SOURCE)
    setter = _slice(joint, "    def setReadOnly(self, joint, value):", "\n\nclass ")
    assert 'tag = "-ReadOnly"' in setter and 'tag = "ReadOnly"' in setter
    assert "obj = joint.ObjectToGround" in setter
    assert 'obj.setPropertyStatus("Placement", tag)' in setter
    assert 'obj.setPropertyStatus("LinkPlacement", tag)' in setter
    # The link the predicate keys on is the one the joint declares.
    assert _compact('"App::PropertyLinkGlobal", "ObjectToGround",') in _compact(joint)
