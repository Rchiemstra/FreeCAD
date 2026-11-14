# SPDX-License-Identifier: LGPL-2.1-or-later
"""Live native AP242 attributes. RawMetadata is provenance, never an export value.

Only descriptive strings, signed lengths (native mm) and unwrapped angles
(native degrees) are supported. Converter observations establish export scope.
"""
import math


def objects(owners):
    return [obj for doc in dict.fromkeys(owner.Document for owner in owners)
            for obj in doc.Objects if any(getattr(group, 'MetadataKind', '') == 'UserAttributes'
                    and obj in getattr(group, 'Group', [])
                    for group in obj.InList)]


def prepare(owners):
    result = []
    sets = {}
    for obj in objects(owners):
        item = dict(object=obj, reason='')
        result.append(item)
        try:
            for field in ('SupplierName', 'Owner', 'ValueKind'):
                if not hasattr(obj, field):
                    raise ValueError('missing native field: ' + field)
            for field in ('SupplierName', 'ValueKind', 'OwnerScope', 'ValueStatus',
                          'UnavailableReason', 'PropertyDefinitionId', 'RepresentationId',
                          'PropertySet', 'PropertyDescription', 'RepresentationName',
                          'ContextIdentifier', 'ContextType'):
                if not isinstance(getattr(obj, field), str):
                    raise ValueError('invalid native text field: ' + field)
            item.update(owner=obj.Owner, kind=obj.ValueKind)
            if not obj.Owner or obj.Document.getObject(obj.Owner.Name) is not obj.Owner:
                raise ValueError('live owner unavailable or deleted')
            if obj.ValueStatus != 'available' or not hasattr(obj, 'Value'):
                raise ValueError('unsupported or unavailable value: ' + obj.UnavailableReason)
            if obj.OwnerScope not in ('root assembly', 'definition', 'occurrence'):
                raise ValueError('unsupported owner scope')
            kind = obj.ValueKind
            value = obj.Value
            if kind in ('Length', 'Angle'):
                expected = 'App::PropertyDistance' if kind == 'Length' else 'Import::PropertyUnwrappedAngle'
                if obj.getTypeIdOfProperty('Value') != expected or not math.isfinite(value.Value):
                    raise ValueError('nonfinite or unsupported native measure')
                value = value.Value
            elif kind != 'String' or not isinstance(value, str):
                raise ValueError('unsupported native value encoding')
            if kind == 'String' and '\x00' in value:
                raise ValueError('embedded NUL string encoding unavailable')
            # Keep each native record in group order. No imported entity ID or
            # object name is used to find an owner, value or another record.
            groups = tuple(group for group in obj.InList
                           if getattr(group, 'MetadataKind', '') == 'UserAttributes'
                           and obj in getattr(group, 'Group', []))
            # Provenance separates identically named property sets; it never binds owners.
            key = (groups, obj.PropertyDefinitionId, obj.RepresentationId)
            set_index = sets.setdefault(key, len(sets))
            item.update(value=value, scope=obj.OwnerScope, name=obj.SupplierName, setIndex=set_index,
                        propertySet=obj.PropertySet, description=obj.PropertyDescription,
                        representation=obj.RepresentationName,
                        contextIdentifier=obj.ContextIdentifier, contextType=obj.ContextType)
        except Exception as error:
            item['reason'] = str(error)
    return result
