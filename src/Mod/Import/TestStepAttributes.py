# SPDX-License-Identifier: LGPL-2.1-or-later
"""Independent AP242 user-property import and native persistence acceptance."""
import json
import math
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import FreeCAD as App
import Import

DATA = Path(__file__).with_name('TestData') / 'StepAttributes'


class TestStepAttributes(unittest.TestCase):
    def setUp(self):
        self.existing = set(App.listDocuments())
        self.temp = tempfile.TemporaryDirectory(prefix='step-attributes-')
        self.prefs = App.ParamGet('User parameter:BaseApp/Preferences/Mod/Import')
        self.saved = {x[1]: x[2] for x in self.prefs.GetContents() or []}
        self.prefs.SetBool('ImportUserAttributes', True)
        self.prefs.SetBool('ImportSavedViews', False)
        self.prefs.SetBool('ImportSemanticPMI', False)

    def tearDown(self):
        for name in set(App.listDocuments()) - self.existing:
            App.closeDocument(name)
        for key in ('ImportUserAttributes', 'ImportSavedViews', 'ImportSemanticPMI'):
            if key in self.saved:
                self.prefs.SetBool(key, self.saved[key])
            else:
                self.prefs.RemBool(key)
        self.temp.cleanup()

    def load(self, unit='mm', links=False, doc=None, path=None, **options):
        doc = doc or App.newDocument('Attributes')
        settings = dict(merge=False, useLinkGroup=links, mode=0)
        settings.update(options)
        Import.insert(str(path or DATA / ('attributes-' + unit + '.step')), doc.Name, **settings)
        doc.recompute()
        return doc

    def group(self, doc):
        group = doc.getObject('StepAttributes')
        self.assertIsNotNone(group, 'opt-in AP242 user attributes were not imported')
        self.assertEqual(group.MetadataKind, 'UserAttributes')
        self.assertEqual(group.MetadataVersion, 1)
        return group

    def records(self, doc):
        return self.group(doc).Group

    def by_name(self, doc, name):
        matches = [r for r in self.records(doc) if r.SupplierName == name]
        self.assertTrue(matches, 'missing supplier attribute: ' + name)
        return matches[0]

    def variant(self, replacements=(), extra='', fixture='mm', **options):
        text = (DATA / ('attributes-' + fixture + '.step')).read_text()
        for old, new in replacements:
            self.assertEqual(text.count(old), 1, old)
            text = text.replace(old, new)
        text = text.replace('ENDSEC;\nEND-ISO-10303-21;', extra + '\nENDSEC;\nEND-ISO-10303-21;')
        path = Path(self.temp.name) / 'variant.step'
        path.write_text(text)
        return self.load(path=path, **options)

    def test_independent_descriptive_and_measured_properties(self):
        for links in (False, True):
            for unit, scale in (('mm', 1), ('inch', 25.4), ('metre', 1000)):
                with self.subTest(links=links, unit=unit):
                    doc = self.load(unit, links)
                    records = self.records(doc)
                    self.assertEqual(len(records), 9)
                    label = self.by_name(doc, 'Label')
                    self.assertEqual(label.Value, 'supplier label')
                    self.assertEqual(label.PropertySet, 'supplier set')
                    self.assertEqual(label.RepresentationName, 'inspection set')
                    self.assertEqual(label.PropertyDescription, 'revision context')
                    self.assertEqual(label.OwnerScope, 'definition')
                    self.assertEqual(label.AssociationStatus, 'resolved', label.UnavailableReason)
                    self.assertTrue(label.Owner)
                    self.assertNotEqual(label.Owner.Label, label.Value)
                    span = self.by_name(doc, 'span')
                    self.assertEqual(span.ValueKind, 'Length')
                    self.assertAlmostEqual(span.Value.Value, 2 * scale)
                    self.assertEqual(span.getTypeIdOfProperty('Value'), 'App::PropertyLength')
                    self.assertEqual(span.SourceValue, '2')
                    self.assertIn('LENGTH_MEASURE', span.SourceType)
                    self.assertIn('#81', span.SourceUnit)
                    self.assertAlmostEqual(self.by_name(doc, 'angle').Value.Value, 90)
                    raw = span.RawMetadata
                    span.Value = 17
                    doc.recompute()
                    self.assertAlmostEqual(span.Value.Value, 17)
                    self.assertNotIn('ReadOnly', span.getEditorMode('Value'))
                    self.assertEqual(span.RawMetadata, raw)

    def test_unicode_duplicate_names(self):
        doc = self.load()
        records = [r for r in self.records(doc) if r.SupplierName == '名称']
        self.assertEqual([r.Value for r in records], ['é温度', 'second'])
        self.assertEqual([r.SourceOrder for r in records], [2, 3])
        self.assertEqual([r.SourceId for r in records], ['#601/#604/2', '#601/#605/3'])
        self.assertNotEqual(records[0].Name, records[1].Name)

    def test_root_definition_occurrence_owners(self):
        for links in (False, True):
            doc = self.load('nested', links)
            root = self.by_name(doc, 'root')
            self.assertEqual(root.OwnerScope, 'root assembly')
            self.assertEqual(root.OwnerSourceId, '#242')
            if links:
                self.assertTrue(root.Owner, root.UnavailableReason)
                self.assertEqual(root.Owner.Label, 'Nested root')
            else:
                self.assertFalse(root.Owner)
                self.assertIn('owner', root.UnavailableReason)
            occurrence = self.by_name(doc, 'override')
            self.assertEqual(occurrence.OwnerScope, 'occurrence')
            self.assertEqual(occurrence.OwnerSourceId, '#210')
            self.assertFalse(occurrence.Owner)
            self.assertIn('path', occurrence.UnavailableReason)
            orphan = self.by_name(doc, 'orphan')
            self.assertFalse(orphan.Owner)
            self.assertIn('omitted', orphan.UnavailableReason)
            # Three repeated definitions under two subassemblies cannot select one occurrence.
            for record in self.records(doc):
                if record.OwnerSourceId == '#7':
                    self.assertFalse(record.Owner)
                    self.assertIn('occurrence', record.UnavailableReason)

    def test_complete_occurrence_path_and_degree_unit(self):
        doc = self.load('assembly', links=True)
        occurrence = self.by_name(doc, 'override')
        self.assertEqual(occurrence.OwnerScope, 'occurrence')
        self.assertEqual(occurrence.OwnerSourceId, '#220')
        self.assertEqual(occurrence.OccurrencePath, '#202/#220')
        self.assertTrue(occurrence.Owner, occurrence.UnavailableReason)
        self.assertAlmostEqual(occurrence.Owner.Placement.Base.x, 4 * 25.4)
        self.assertTrue(occurrence.Owner.isDerivedFrom('App::Link'))
        self.assertEqual(self.by_name(doc, 'root').Owner.Label, 'Placed blocks')
        doc = self.variant([("PLANE_ANGLE_MEASURE(1.5707963267948966),#84) PLANE_ANGLE_MEASURE_WITH_UNIT() REPRESENTATION_ITEM('angle')",
                             "PLANE_ANGLE_MEASURE(90.),#680) PLANE_ANGLE_MEASURE_WITH_UNIT() REPRESENTATION_ITEM('angle')")], extra="""
#680=(CONVERSION_BASED_UNIT('DEGREE',#681) NAMED_UNIT(#682) PLANE_ANGLE_UNIT());
#681=PLANE_ANGLE_MEASURE_WITH_UNIT(PLANE_ANGLE_MEASURE(0.017453292519943295),#84);
#682=DIMENSIONAL_EXPONENTS(0.,0.,0.,0.,0.,0.,0.);
""")
        angle = self.by_name(doc, 'angle')
        self.assertAlmostEqual(angle.Value.Value, 90)
        self.assertIn('DEGREE', angle.SourceUnit)
        self.assertEqual(angle.SourceValue, '90')

    def test_ambiguous_occurrence_transfer_is_unavailable(self):
        doc = self.variant([("#226=CARTESIAN_POINT('',(4.,0.,0.));",
                             "#226=CARTESIAN_POINT('',(8.,0.,0.));")], fixture='assembly')
        record = self.by_name(doc, 'override')
        self.assertFalse(record.Owner)
        self.assertIn('ambiguous', record.UnavailableReason)
        self.assertEqual(record.OccurrencePath, '#202/#220')

    def test_malformed_chain_keeps_valid_records(self):
        doc = self.variant([('#601=PROPERTY_DEFINITION_REPRESENTATION(#600,#602);',
                             '#601=PROPERTY_DEFINITION_REPRESENTATION(#600,$);')])
        records = self.records(doc)
        self.assertEqual(len(records), 2)
        malformed = next(r for r in records if r.SourceId == '#601')
        self.assertEqual(malformed.ValueStatus, 'unavailable')
        self.assertIn('malformed', malformed.UnavailableReason)
        self.assertEqual(self.by_name(doc, 'orphan').Value, 'no shape')

    def test_unknown_type_unit_is_unavailable(self):
        doc = self.load()
        for name in ('unsupported reference or array', 'unsupported area', 'unknown unit'):
            record = self.by_name(doc, name)
            self.assertEqual(record.ValueStatus, 'unavailable')
            self.assertTrue(record.UnavailableReason)
            self.assertNotIn('Value', record.PropertiesList)
            self.assertTrue(record.SourceType)
        unknown = self.by_name(doc, 'unknown unit')
        self.assertIn('supplier unit', unknown.SourceUnit)
        self.assertEqual(unknown.SourceValue, '3')

    def test_material_pmi_not_reclassified(self):
        doc = self.load(semanticPMI=True, savedViews=True)
        records = self.records(doc)
        self.assertEqual(len(records), 9)
        self.assertFalse({'steel', 'validation only', 'PMI only'} & {r.SupplierName for r in records})
        self.assertEqual(len(doc.StepPMI.Group), 5)
        self.assertEqual(len([r for r in doc.StepViews.Group if r.RecordType == 'SavedView']), 2)
        self.assertFalse(any(hasattr(r, 'Category') for r in records))

    def test_attribute_restore_copy_transactions(self):
        source = Path(self.temp.name) / 'supplier.step'
        shutil.copyfile(DATA / 'attributes-inch.step', source)
        doc = App.newDocument('PersistentAttributes')
        doc.openTransaction('import attributes')
        self.load(doc=doc, links=True, path=source)
        self.group(doc)
        doc.commitTransaction()
        names = {o.Name for o in doc.Objects}
        doc.undo()
        self.assertEqual(len(doc.Objects), 0)
        doc.redo()
        self.assertEqual({o.Name for o in doc.Objects}, names)
        span = self.by_name(doc, 'span')
        raw = span.RawMetadata
        doc.openTransaction('edit live attribute')
        span.Value = 12
        doc.commitTransaction()
        doc.undo()
        self.assertAlmostEqual(span.Value.Value, 50.8)
        doc.redo()
        self.assertAlmostEqual(span.Value.Value, 12)
        path = Path(self.temp.name) / 'attributes.FCStd'
        doc.recompute()
        doc.saveAs(str(path))
        App.closeDocument(doc.Name)
        source.unlink()
        restored = App.openDocument(str(path))
        self.assertEqual(self.by_name(restored, 'span').RawMetadata, raw)
        self.assertAlmostEqual(self.by_name(restored, 'span').Value.Value, 12)
        dest = App.newDocument('CopiedAttributes')
        copied = dest.copyObject(self.group(restored), True)
        self.assertTrue(copied.Group)
        for record in copied.Group:
            if record.Owner:
                self.assertEqual(record.Owner.Document, dest)
        self.assertTrue(next(r for r in copied.Group if r.SupplierName == 'span').Owner)
        partial_dest = App.newDocument('PartialAttributeCopy')
        partial = partial_dest.copyObject(self.by_name(restored, 'span'), False)
        self.assertFalse(partial.Owner)
        partial_dest.addObject('Part::Feature', self.by_name(restored, 'span').Owner.Name)
        partial_dest.recompute()
        partial_path = Path(self.temp.name) / 'partial.FCStd'
        partial_name = partial.Name
        partial_dest.saveAs(str(partial_path))
        App.closeDocument(partial_dest.Name)
        partial_dest = App.openDocument(str(partial_path))
        partial = partial_dest.getObject(partial_name)
        self.assertFalse(partial.Owner)
        App.closeDocument(restored.Name)
        self.assertAlmostEqual(next(r for r in copied.Group if r.SupplierName == 'span').Value.Value, 12)

    def test_repeated_insert_isolation(self):
        for links in (False, True):
            doc = self.load(links=links)
            first = self.group(doc)
            before = {o.Name for o in doc.Objects}
            doc.openTransaction('second attribute import')
            self.load(doc=doc, links=links)
            doc.commitTransaction()
            groups = [o for o in doc.Objects if getattr(o, 'MetadataKind', '') == 'UserAttributes']
            self.assertEqual(len(groups), 2)
            for group in groups:
                for record in group.Group:
                    if record.Owner:
                        self.assertEqual(record.Owner.Name in before, group == first)
            doc.undo()
            self.assertEqual({o.Name for o in doc.Objects}, before)
            doc.redo()
            second = App.newDocument('OtherAttributeDocument')
            self.load(doc=second, links=links)
            self.assertTrue(self.by_name(second, 'span').Owner)
            self.assertEqual(self.by_name(second, 'span').Owner.Document, second)

    def test_conflicting_units_and_malformed_nonfinite(self):
        self.group(self.load())
        for value in ('1.E999', '$', "'bad'"):
            with self.subTest(value=value):
                doc = self.variant([('LENGTH_MEASURE(2.)', 'LENGTH_MEASURE(' + value + ')')])
                record = self.by_name(doc, 'span')
                self.assertEqual(record.ValueStatus, 'unavailable')
                self.assertTrue(record.UnavailableReason)
                self.assertNotIn('Value', record.PropertiesList)
                self.assertEqual(self.by_name(doc, 'Label').Value, 'supplier label')
        doc = self.variant([('LENGTH_MEASURE(2.),#81', 'LENGTH_MEASURE(1.E308),#680')],
                           '#680=(LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT($,.METRE.));')
        overflow = self.by_name(doc, 'span')
        self.assertEqual(overflow.ValueStatus, 'unavailable')
        self.assertNotIn('Value', overflow.PropertiesList)
        self.assertIn('nonfinite', overflow.UnavailableReason)
        doc = self.variant([('GLOBAL_UNIT_ASSIGNED_CONTEXT((#81,#84,#85))',
                             'GLOBAL_UNIT_ASSIGNED_CONTEXT((#81,#84,#85,#680))')],
                           '#680=(LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT($,.METRE.));')
        # Explicit measure units are authoritative, independent of the geometry context.
        self.assertAlmostEqual(self.by_name(doc, 'span').Value.Value, 2)
        self.assertIn('#81', self.by_name(doc, 'span').SourceUnit)
        doc = self.variant([("LENGTH_MEASURE(2.),#81", "LENGTH_MEASURE(2.),#84")])
        self.assertEqual(self.by_name(doc, 'span').ValueStatus, 'unavailable')

    def test_partial_record_failure_isolation(self):
        doc = self.load(semanticPMI=True, savedViews=True)
        self.group(doc)
        import StepAttributes
        original = StepAttributes._property
        old = {o.Name for o in doc.Objects}
        sentinels = []
        class Observer:
            def slotCreatedObject(self, obj):
                if obj.Document == doc and obj.Name.startswith('StepAttribute') and obj.TypeId != 'App::DocumentObjectGroup':
                    sentinels.append(doc.addObject('App::FeaturePython', 'ObserverOwned').Name)
        observer = Observer()
        def fail(obj, kind, name, value, *args):
            if name == 'Value' and value == 'supplier label':
                raise RuntimeError('injected partial attribute failure')
            return original(obj, kind, name, value, *args)
        App.addDocumentObserver(observer)
        try:
            with patch.object(StepAttributes, '_property', side_effect=fail):
                self.load(doc=doc, semanticPMI=True, savedViews=True)
        finally:
            App.removeDocumentObserver(observer)
        self.assertTrue(old.issubset({o.Name for o in doc.Objects}))
        groups = [o for o in doc.Objects if getattr(o, 'MetadataKind', '') == 'UserAttributes']
        self.assertEqual(len(groups), 2)
        self.assertEqual(len(groups[1].Group), 8)
        self.assertTrue(all(doc.getObject(name) for name in sentinels))
        self.assertEqual(len([o for o in doc.Objects if getattr(o, 'MetadataKind', '') == 'SavedViews']), 2)
        self.assertEqual(len([o for o in doc.Objects if o.Name.startswith('StepPMI') and o.TypeId == 'App::DocumentObjectGroup']), 2)
        # Group setup failure rolls back just this call's explicit object list.
        with patch.object(StepAttributes, '_property', side_effect=RuntimeError('group failure')):
            self.load(doc=doc)
        self.assertEqual(len([o for o in doc.Objects if getattr(o, 'MetadataKind', '') == 'UserAttributes']), 2)

    def test_default_preference_open_and_keyword(self):
        self.group(self.load())
        self.prefs.RemBool('ImportUserAttributes')
        doc = self.load()
        self.assertIsNone(doc.getObject('StepAttributes'))
        self.prefs.SetBool('ImportUserAttributes', True)
        doc = self.load(userAttributes=False)
        self.assertIsNone(doc.getObject('StepAttributes'))
        self.prefs.SetBool('ImportUserAttributes', False)
        Import.open(str(DATA / 'attributes-mm.step'), userAttributes=True, merge=False, useLinkGroup=True)
        self.assertEqual(len(self.records(App.ActiveDocument)), 9)
        with self.assertRaises(TypeError):
            self.load(userAttributes='yes')

    def test_merged_and_reduced_owners_unavailable(self):
        self.group(self.load())
        for links in (False, True):
            doc = self.load('nested', links, merge=True)
            for record in self.records(doc):
                self.assertFalse(record.Owner)
                self.assertIn('merged', record.UnavailableReason)

    def test_real_geometry_and_metadata_parity(self):
        self.group(self.load())
        for links in (False, True):
            enabled = self.load('nested', links, savedViews=True, semanticPMI=True)
            disabled = self.load('nested', links, savedViews=True, semanticPMI=True, userAttributes=False)
            def model(doc):
                result = []
                for obj in doc.Objects:
                    if obj.isDerivedFrom('Part::Feature') and obj.Shape.Solids:
                        result.append((obj.TypeId, obj.Label, round(obj.Shape.Volume, 5), str(obj.Placement)))
                    elif obj.TypeId in ('App::Part', 'App::Link', 'App::LinkGroup'):
                        result.append((obj.TypeId, obj.Label, str(getattr(obj, 'Placement', None))))
                return sorted(result)
            self.assertEqual(model(enabled), model(disabled))
            self.assertTrue(model(enabled))
            self.assertEqual(len(enabled.StepPMI.Group), len(disabled.StepPMI.Group))
            self.assertEqual(len(enabled.StepViews.Group), len(disabled.StepViews.Group))
