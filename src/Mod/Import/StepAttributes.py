# SPDX-License-Identifier: LGPL-2.1-or-later
"""Owned AP242 user attributes, schema 1, independent of PMI and saved views.

Import[Gui].open/insert(userAttributes=True) overrides ImportUserAttributes in
BaseApp/Preferences/Mod/Import (default off). Each representation item has its
own record, preserving supplier spelling, duplicates and item order. Lengths
are mm and angles degrees. Only finite explicitly unit-bearing measures and
Unicode descriptive strings are available; other encodings retain reasons.
Native Value properties are editable and authoritative. Immutable RawMetadata
is provenance only: no proxy replays it on restore, copy, recompute or undo.
Native Owner links remap through ordinary document copy, including omission.
No source file, XCAF handle or GUI activation is needed after import.
"""
import json
import math


def _property(obj, kind, name, value, description='Owned STEP user attribute data'):
    obj.addProperty('App::Property' + kind, name, 'STEP user attributes', description)
    setattr(obj, name, value)
    if name != 'Value':
        obj.setEditorMode(name, 1)


def _remove(document, objects):
    for name, obj in reversed(objects):
        try:
            if document.getObject(name) is obj:
                document.removeObject(name)
        except Exception:
            pass


def create(document, records):
    """Create records independently; remove only objects explicitly created here."""
    if not records:
        return None
    created = []
    try:
        group = document.addObject('App::DocumentObjectGroup', 'StepAttributes')
        created.append((group.Name, group))
        group.Label = 'STEP user attributes'
        _property(group, 'Integer', 'MetadataVersion', 1)
        _property(group, 'String', 'MetadataKind', 'UserAttributes')
        warnings, imported = [], []
        for record in records:
            record_created = []
            try:
                obj = document.addObject('App::FeaturePython', 'StepAttribute')
                record_created.append((obj.Name, obj))
                created.append((obj.Name, obj))
                obj.Label = record.get('SupplierName', '') or 'STEP attribute'
                for name, value in record.items():
                    if name in ('value', 'ObjectName', 'ValueReason', 'OwnerReason'):
                        continue
                    _property(obj, 'Integer' if name == 'SourceOrder' else 'String', name, value)
                kind = record.get('ValueKind', '')
                value_reason = record.get('ValueReason', '')
                if kind in ('Length', 'Angle'):
                    if not math.isfinite(record['value']):
                        value_reason = 'nonfinite converted value'
                    elif not value_reason:
                        _property(obj, kind, 'Value', record['value'], 'Editable live value for later export')
                elif kind == 'String' and not value_reason:
                    _property(obj, 'String', 'Value', record['SourceValue'], 'Editable live value for later export')
                else:
                    value_reason = value_reason or 'unsupported source value'
                owner_reason = record.get('OwnerReason', '')
                owner = document.getObject(record['ObjectName']) if record.get('ObjectName') else None
                if owner is None:
                    owner_reason = owner_reason or 'omitted exact owner'
                _property(obj, 'Link', 'Owner', owner if not owner_reason else None,
                          'Exact observed owner in this import; native copy remaps this link')
                _property(obj, 'String', 'ValueStatus', 'unavailable' if value_reason else 'available')
                _property(obj, 'String', 'AssociationStatus', 'unavailable' if owner_reason else 'resolved')
                _property(obj, 'String', 'UnavailableReason', '; '.join(
                    r for r in (value_reason, owner_reason) if r))
                _property(obj, 'String', 'RawMetadata', json.dumps(
                    {'schemaVersion': 1, 'record': record}, ensure_ascii=False,
                    sort_keys=True, allow_nan=False))
                group.addObject(obj)
                if owner and not owner_reason and owner not in imported:
                    imported.append(owner)
            except Exception as error:
                _remove(document, record_created)
                warnings.append(str(record.get('SourceId', 'unknown')) + ': ' + str(error))
        _property(group, 'StringList', 'Warnings', warnings)
        _property(group, 'LinkList', 'ImportedObjects', imported)
        return group
    except Exception:
        _remove(document, created)
        raise
