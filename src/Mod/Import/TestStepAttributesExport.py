# SPDX-License-Identifier: LGPL-2.1-or-later
"""AP242 attribute export acceptance; independent supplier fixtures supply expectations."""
import json
from pathlib import Path
import re
import shutil
import unittest
from unittest.mock import patch

import FreeCAD as App
import Import
import Part
from TestStepAttributes import TestStepAttributes as AttributeImportTests, DATA


class TestStepAttributesExport(AttributeImportTests):
    # Reuse lifecycle helpers, without inheriting the import test inventory.
    def setUp(self):
        super().setUp()
        self.export_saved = {x[1]: x[2] for x in self.prefs.GetContents() or []}
        for key in ('ExportUserAttributes', 'ExportSemanticPMI'):
            self.prefs.SetBool(key, False)
        self.units = Part.exportUnits()

    def tearDown(self):
        for key in ('ExportUserAttributes', 'ExportSemanticPMI'):
            if key in self.export_saved:
                self.prefs.SetBool(key, self.export_saved[key])
            else:
                self.prefs.RemBool(key)
        Part.setStaticValue('write.step.unit', self.units['write.step.unit'])
        super().tearDown()

    def owners(self, doc):
        return list(dict.fromkeys(r.Owner for r in self.records(doc) if r.Owner))

    def emit(self, doc, owners=None, **options):
        self.prefs.SetBool('ExportUserAttributes', True)
        path = Path(self.temp.name) / 'export.step'
        report = Import.export(self.owners(doc) if owners is None else owners, str(path), **options)
        self.assertIsInstance(report, list, 'enabled user attributes must return per-record results')
        self.assertEqual(len([r for r in report if r.get('kind') == 'userAttribute']),
                         len(self.records(doc)), 'every user attribute needs a result')
        return path, report

    def text(self, path):
        return re.sub(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d', 'TIME', path.read_text())

    def reload(self, path):
        return self.load(path=path, userAttributes=True)

    def check_values(self, doc, span=2, angle=90, label='supplier label'):
        self.assertEqual(self.by_name(doc, 'Label').Value, label)
        self.assertAlmostEqual(self.by_name(doc, 'span').Value.Value, span)
        self.assertAlmostEqual(self.by_name(doc, 'angle').Value.Value, angle)
        duplicate = [r for r in self.records(doc) if r.SupplierName == '名称']
        self.assertEqual([r.Value for r in duplicate], ['é温度', 'second'])
        self.assertLess(duplicate[0].SourceOrder, duplicate[1].SourceOrder)
        for r in self.records(doc):
            self.assertEqual(r.PropertySet, 'supplier set')
            self.assertEqual(r.PropertyDescription, 'revision context')
            self.assertEqual(r.RepresentationName, 'inspection set')
            self.assertEqual(r.ContextIdentifier, '')
            self.assertEqual(r.ContextType, 'inch context')
            self.assertTrue(r.Owner, r.UnavailableReason)

    def test_keyword_api(self):
        doc = self.load()
        try:
            result = Import.export(self.owners(doc), str(Path(self.temp.name) / 'off.step'),
                                   userAttributes=False, semanticPMI=False)
        except TypeError as error:
            self.fail('missing userAttributes export API: ' + str(error))
        self.assertIsNone(result)
        enabled = Import.export(self.owners(doc), str(Path(self.temp.name) / 'on.step'),
                                userAttributes=True, semanticPMI=False)
        self.assertEqual(sum(r['status'] == 'written' for r in enabled), 5)
        with self.assertRaises(TypeError):
            Import.export(self.owners(doc), str(Path(self.temp.name) / 'bad.step'), userAttributes='yes')

    def test_default_off_and_explicit_override(self):
        doc = self.load()
        path = Path(self.temp.name) / 'export.step'
        self.prefs.RemBool('ExportUserAttributes')
        self.assertIsNone(Import.export(self.owners(doc), str(path)))
        expected = self.text(path)
        self.prefs.SetBool('ExportUserAttributes', True)
        try:
            result = Import.export(self.owners(doc), str(path), userAttributes=False)
        except TypeError as error:
            self.fail('missing explicit false API: ' + str(error))
        self.assertIsNone(result)
        self.assertEqual(self.text(path), expected)
        self.assertNotIn('DESCRIPTIVE_REPRESENTATION_ITEM', expected)

    def test_roundtrip_and_independent_property_graph(self):
        for legacy in (False, True):
            doc = self.load()
            path, report = self.emit(doc, legacy=legacy)
            self.assertEqual(sum(r['status'] == 'written' for r in report), 5, report)
            self.assertTrue(all(r['reason'] for r in report if r['status'] == 'skipped'))
            text = path.read_text()
            entities = dict(re.findall(r'#(\d+)\s*=\s*(.*?);', text, re.S))
            props = [(key, value) for key, value in entities.items()
                     if value.startswith("PROPERTY_DEFINITION('supplier set'")]
            self.assertEqual(len(props), 1)
            prop_id, prop = props[0]
            target = re.findall(r'#(\d+)', prop)[-1]
            self.assertTrue(entities[target].startswith('PRODUCT_DEFINITION('), entities[target])
            chain = next(v for v in entities.values() if re.match(
                r'PROPERTY_DEFINITION_REPRESENTATION\(#' + prop_id + r',', v))
            rep = entities[re.findall(r'#(\d+)', chain)[-1]]
            self.assertTrue(rep.startswith("REPRESENTATION('inspection set'"), rep)
            self.assertEqual(len(re.findall(r'#\d+', rep)), 6)
            self.assertIn("REPRESENTATION_CONTEXT('','inch context')", text)
            self.assertNotIn('AREA_MEASURE', text)
            self.check_values(self.reload(path))

    def test_native_edit_undo_source_free_reopen_and_copy(self):
        source = Path(self.temp.name) / 'source.step'
        shutil.copyfile(DATA / 'attributes-mm.step', source)
        doc = self.load(path=source, links=True)
        raw = [r.RawMetadata for r in self.records(doc)]
        doc.openTransaction('attribute edits')
        self.by_name(doc, 'span').Value = '-2000 mm'
        self.by_name(doc, 'angle').Value = '810 deg'
        self.by_name(doc, 'Label').Value = '温度 edited'
        doc.recompute()
        doc.commitTransaction()
        path, _ = self.emit(doc)
        self.check_values(self.reload(path), -2000, 810, '温度 edited')
        doc.undo()
        path, _ = self.emit(doc)
        self.check_values(self.reload(path))
        doc.redo()
        source.unlink()
        native = Path(self.temp.name) / 'native.FCStd'
        doc.saveAs(str(native))
        App.closeDocument(doc.Name)
        restored = App.openDocument(str(native))
        copied = App.newDocument('AttributesExportCopy')
        copied.copyObject(self.group(restored), True)
        for target in (restored, copied):
            target.recompute()
            self.assertEqual([r.RawMetadata for r in self.records(target)], raw)
            self.assertTrue(all(r.Owner.Document == target for r in self.records(target) if r.Owner))
            path, _ = self.emit(target)
            self.check_values(self.reload(path), -2000, 810, '温度 edited')

    def test_mm_inch_metre_signed_and_unwrapped_encodings(self):
        doc = self.load()
        self.by_name(doc, 'span').Value = -50.8
        self.by_name(doc, 'angle').Value = -1080
        for unit, expected in (('MM', -50.8), ('INCH', -2), ('M', -0.0508)):
            Part.setStaticValue('write.step.unit', unit)
            baseline_path = Path(self.temp.name) / 'export.step'
            Import.export(self.owners(doc), str(baseline_path), userAttributes=False, semanticPMI=False)
            baseline = self.text(baseline_path)
            path, _ = self.emit(doc)
            text = path.read_text()
            scalar_text = re.sub(r'\s+', '', text)
            match = re.search(r"MEASURE_WITH_UNIT\(LENGTH_MEASURE\(([^)]+)\),#(\d+)\)REPRESENTATION_ITEM\('span'\)", scalar_text)
            self.assertIsNotNone(match, text[-4000:])
            self.assertAlmostEqual(float(match[1]), expected)
            entity = re.search(r'#' + match[2] + r'\s*=\s*(.*?);', text, re.S)[1]
            self.assertIn('LENGTH_UNIT()', entity)
            angle = re.search(r"MEASURE_WITH_UNIT\(PLANE_ANGLE_MEASURE\(([^)]+)\),#(\d+)\)PLANE_ANGLE_MEASURE_WITH_UNIT\(\)REPRESENTATION_ITEM\('angle'\)", scalar_text)
            self.assertIsNotNone(angle)
            self.assertAlmostEqual(float(angle[1]), -6 * 3.141592653589793)
            self.check_values(self.reload(path), -50.8, -1080)
            self.assertEqual(Part.exportUnits()['write.step.unit'], unit)
            Import.export(self.owners(doc), str(path), userAttributes=False, semanticPMI=False)
            self.assertEqual(self.text(path), baseline, 'schema/units must be restored after emission')

    def test_omitted_deleted_shared_repeated_nested_owners(self):
        for scenario in ('omitted', 'deleted', 'shared', 'repeated', 'nested'):
            doc = self.load()
            owner = self.owners(doc)[0]
            if scenario == 'omitted':
                other = doc.addObject('Part::Feature', 'Other')
                other.Shape = owner.Shape.copy()
                selected = [other]
            elif scenario == 'deleted':
                doc.removeObject(owner.Name)
                other = doc.addObject('Part::Feature', 'Other')
                other.Shape = Part.makeBox(1, 2, 3)
                selected = [other]
            else:
                link = doc.addObject('App::Link', 'Instance')
                link.setLink(owner)
                link.Placement.Base.x = 20
                selected = [owner, link]
                if scenario in ('repeated', 'nested'):
                    group = doc.addObject('App::Part', 'Assembly')
                    group.addObject(owner)
                    group.addObject(link)
                    selected = [group]
                    if scenario == 'nested':
                        repeat = doc.addObject('App::Link', 'RepeatedAssembly')
                        repeat.setLink(group)
                        selected.append(repeat)
            path, report = self.emit(doc, selected)
            self.assertTrue(all(r['status'] == 'skipped' and r['reason'] for r in report), (scenario, report))
            self.assertNotIn("PROPERTY_DEFINITION('supplier set'", path.read_text())

    def test_live_owner_link_authoritative_partial_copy(self):
        doc = self.load()
        other = doc.addObject('Part::Feature', 'Other')
        other.Shape = Part.makeCylinder(3, 5)
        record = self.by_name(doc, 'Label')
        record.Owner = other
        path, report = self.emit(doc, [other])
        self.assertEqual(next(r for r in report if r['object'] == record.Name)['status'], 'written')
        restored = self.reload(path)
        self.assertAlmostEqual(self.by_name(restored, 'Label').Owner.Shape.Volume, other.Shape.Volume)
        dest = App.newDocument('PartialAttributes')
        dest.copyObject(self.group(doc), False)
        self.assertTrue(all(not r.Owner for r in self.records(dest)))
        shape = dest.addObject('Part::Feature', 'Replacement')
        shape.Shape = other.Shape
        _, report = self.emit(dest, [shape])
        self.assertTrue(all(r['status'] == 'skipped' for r in report))

    def test_root_and_exact_occurrence_scopes(self):
        for fixture in ('assembly', 'nested'):
            doc = self.load(fixture, links=True)
            roots = [self.by_name(doc, 'root').Owner]
            path, report = self.emit(doc, roots)
            root = self.by_name(doc, 'root')
            self.assertEqual(next(r for r in report if r['object'] == root.Name)['status'], 'written', report)
            override = self.by_name(doc, 'override')
            row = next(r for r in report if r['object'] == override.Name)
            self.assertEqual(row['status'], 'written' if fixture == 'assembly' else 'skipped', report)
            if fixture == 'assembly':
                text = path.read_text()
                entities = dict(re.findall(r'#(\d+)\s*=\s*(.*?);', text, re.S))
                instance = next(v for v in entities.values() if v.startswith("PROPERTY_DEFINITION('instance set'"))
                target = entities[re.findall(r'#(\d+)', instance)[-1]]
                self.assertTrue(target.startswith('NEXT_ASSEMBLY_USAGE_OCCURRENCE('), target)
                restored = self.load(path=path, links=True)
                occurrence = self.by_name(restored, 'override')
                self.assertEqual(occurrence.OwnerScope, 'occurrence')
                self.assertAlmostEqual(occurrence.Owner.Shape.BoundBox.XMin, 101.6)

    def test_emission_failure_discards_partial_model_retains_pmi(self):
        import StepAttributesExport
        doc = self.load(semanticPMI=True, savedViews=True)
        selected = self.owners(doc)
        path = Path(self.temp.name) / 'export.step'
        pmi = Import.export(selected, str(path), semanticPMI=True)
        expected = self.text(path)
        original = StepAttributesExport.prepare
        def inject(owners):
            items = original(owners)
            # The first descriptive item is emitted before the numeric conversion fails.
            next(i for i in items if i.get('kind') == 'Length')['value'] = 'injected failure'
            return items
        with patch.object(StepAttributesExport, 'prepare', side_effect=inject):
            _, report = self.emit(doc, selected, semanticPMI=True)
        self.assertEqual(self.text(path), expected)
        self.assertEqual([r for r in report if r.get('kind') != 'userAttribute'], pmi)
        self.assertTrue(all(r['status'] == 'skipped' and r['reason']
                            for r in report if r.get('kind') == 'userAttribute'))
        self.assertFalse(hasattr(self.reload(path), 'StepPMI'))
        restored = self.load(path=path, semanticPMI=True, savedViews=True)
        self.assertEqual(len(restored.StepPMI.Group), len(doc.StepPMI.Group))
        self.assertFalse(hasattr(restored, 'StepViews'))
        self.assertFalse(hasattr(restored, 'StepAttributes'))

    def test_geometry_placement_hierarchy_and_pmi_view_isolation(self):
        from StepPmiExport import fingerprint
        def snapshot(doc):
            result = []
            for obj in doc.Objects:
                if hasattr(obj, 'MetadataKind') or hasattr(obj, 'Category') or hasattr(obj, 'SupplierName'):
                    continue
                if obj.Name.startswith('StepPMI'):
                    continue
                result.append((obj.TypeId, obj.Label, str(getattr(obj, 'Placement', None)),
                               tuple(child.Label for child in getattr(obj, 'Group', [])),
                               fingerprint(obj) if hasattr(obj, 'Shape') else None))
            return result
        for legacy in (False, True):
            doc = self.load(semanticPMI=True, savedViews=True)
            owner = self.owners(doc)[0]
            owner.Placement = App.Placement(App.Vector(50, 20, 10),
                                            App.Rotation(App.Vector(0, 0, 1), 30))
            assembly = doc.addObject('App::Part', 'Assembly')
            assembly.addObject(owner)
            other = doc.addObject('Part::Feature', 'Other')
            other.Shape = Part.makeCylinder(2, 7)
            assembly.addObject(other)
            path = Path(self.temp.name) / 'export.step'
            def color_result():
                target = App.newDocument('AttributeColorParity')
                colors = Import.insert(str(path), target.Name, mode=0, merge=False,
                                       useLinkGroup=False, userAttributes=False, semanticPMI=False)
                return [(round(obj.Shape.Volume, 9), tuple(str(c) for c in palette))
                        for obj, palette in colors or []]
            before = snapshot(doc)
            pmi_before = [r.RawMetadata for r in doc.StepPMI.Group]
            views_before = [r.RawMetadata for r in doc.StepViews.Group]
            Import.export([assembly], str(path), semanticPMI=True, userAttributes=False,
                          legacy=legacy, keepPlacement=True)
            baseline = self.load(path=path, userAttributes=False, semanticPMI=True)
            expected = snapshot(baseline)
            expected_colors = color_result()
            _, report = self.emit(doc, [assembly], semanticPMI=True, legacy=legacy, keepPlacement=True)
            self.assertEqual(sum(r['status'] == 'written' for r in report if r.get('kind') == 'userAttribute'), 5)
            restored = self.load(path=path, userAttributes=True, savedViews=True, semanticPMI=True)
            self.assertEqual(snapshot(restored), expected)
            self.assertEqual(color_result(), expected_colors)
            self.assertEqual(snapshot(doc), before)
            self.assertEqual([r.RawMetadata for r in doc.StepPMI.Group], pmi_before)
            self.assertEqual([r.RawMetadata for r in doc.StepViews.Group], views_before)
            self.assertEqual(len(restored.StepPMI.Group), len(doc.StepPMI.Group))
            self.assertFalse(hasattr(restored, 'StepViews'))

    def test_identically_named_property_sets_remain_distinct(self):
        doc = self.variant(extra="""
#680=PROPERTY_DEFINITION('supplier set','revision context',#7);
#681=PROPERTY_DEFINITION_REPRESENTATION(#680,#682);
#682=REPRESENTATION('inspection set',(#603),#80);
""")
        path, report = self.emit(doc)
        self.assertEqual(sum(r['status'] == 'written' for r in report), 6)
        self.assertEqual(len(re.findall(r"PROPERTY_DEFINITION\('supplier set'", path.read_text())), 2)
        restored = self.reload(path)
        self.assertEqual(len([r for r in self.records(restored) if r.SupplierName == 'Label']), 2)

    def test_nonfinite_native_value_is_unavailable(self):
        doc = self.load()
        span = self.by_name(doc, 'span')
        span.Value = float('inf')
        path, report = self.emit(doc)
        row = next(r for r in report if r['object'] == span.Name)
        self.assertEqual(row['status'], 'skipped')
        self.assertIn('nonfinite', row['reason'])
        self.assertNotIn("REPRESENTATION_ITEM('span'", path.read_text())
        self.assertEqual(self.by_name(self.reload(path), 'Label').Value, 'supplier label')

    def test_color_palette_parity(self):
        doc = self.load()
        owner = self.owners(doc)[0]
        colors = [(1.0, 0.0, 0.0), (0.0, 1.0, 0.0)] * (len(owner.Shape.Faces) // 2)
        selected = [(owner, colors)]
        path = Path(self.temp.name) / 'export.step'
        palettes = []
        for enabled in (False, True):
            report = Import.export(selected, str(path), userAttributes=enabled, semanticPMI=False)
            if enabled:
                self.assertEqual(sum(r['status'] == 'written' for r in report), 5)
            target = App.newDocument('ColorsAfterAttributes')
            result = Import.insert(str(path), target.Name, merge=True, mode=0,
                                   useLinkGroup=False, userAttributes=False, semanticPMI=False)
            self.assertTrue(result, 'explicit colors must survive STEP export')
            palettes.append([(round(obj.Shape.Volume, 8), tuple(tuple(c) for c in palette))
                             for obj, palette in result])
        self.assertEqual(palettes[0], palettes[1])

    def test_disabled_and_failure_geometry_bytes_parity(self):
        import StepAttributesExport
        doc = self.load()
        path = Path(self.temp.name) / 'export.step'
        Import.export(self.owners(doc), str(path))
        expected = self.text(path)
        with patch.object(StepAttributesExport, 'prepare', side_effect=RuntimeError('injected preparation failure')):
            _, report = self.emit(doc)
        self.assertEqual(self.text(path), expected)
        self.assertTrue(all(r['status'] == 'skipped' for r in report))
        import sys
        with patch.dict(sys.modules, {'StepAttributesExport': None}):
            _, report = self.emit(doc)
        self.assertEqual(self.text(path), expected)
        self.assertTrue(all(r['status'] == 'skipped' for r in report))


# Helpers above deliberately reuse the reader fixture/lifecycle support only.
for _name in list(vars(AttributeImportTests)):
    if _name.startswith('test_') and _name not in vars(TestStepAttributesExport):
        setattr(TestStepAttributesExport, _name, None)
del AttributeImportTests
