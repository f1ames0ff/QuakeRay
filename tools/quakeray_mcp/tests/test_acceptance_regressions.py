import copy
import json
from pathlib import Path
import tempfile
import unittest

from quakeray_mcp.asset_inventory import inventory
from quakeray_mcp.code_index import CodeIndex
from quakeray_mcp.compare import IDENTITY_FIELDS, compare_records
from quakeray_mcp.service import EvidenceService


ROOT = Path(__file__).resolve().parents[3]


class AcceptanceRegressionTests(unittest.TestCase):
    def test_additional_archive_and_loose_file_change_inventory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'ad/maps').mkdir(parents=True)
            (root / 'ad/pak3.pak').write_bytes(b'one')
            (root / 'ad/maps/custom.bsp').write_bytes(b'loose')
            before = inventory(root)
            self.assertEqual(len(before), 2)
            (root / 'ad/pak3.pak').write_bytes(b'two')
            self.assertNotEqual(before, inventory(root))

    def test_asset_prefix_is_not_a_generated_output_exclusion(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'ad/maps').mkdir(parents=True)
            (root / 'ad/models').mkdir()
            for name in ('ad/maps/benchmark.bsp', 'ad/models/screenshot.mdl', 'ad/perf-override.pkz'):
                (root / name).write_bytes(b'asset')
            (root / 'ad/benchmark.log').write_text('generated')
            entries = inventory(root)
            self.assertEqual(len(entries), 3)
            (root / 'ad/maps/benchmark.bsp').write_bytes(b'changed')
            self.assertNotEqual(entries, inventory(root))

    def test_renderer_hpp_change_invalidates_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'renderer/Source').mkdir(parents=True)
            (root / 'build/Debug').mkdir(parents=True)
            source = root / 'renderer/Source/Scene.cpp'
            header = root / 'renderer/Source/Scene.hpp'
            source.write_text('int foo();')
            header.write_text('one')
            (root / 'build/Debug/compile_commands.json').write_text('[]')
            index = CodeIndex(root, root / 'missing-clangd')
            before = index.identity(source)
            header.write_text('two')
            self.assertNotEqual(before, index.identity(source))

    def test_refreshed_invalid_records_never_pass(self):
        record = {'identity': {key: 'matching' for key in IDENTITY_FIELDS},
                  'validity': 'valid', 'provenance_verified': True, 'aggregation_kind': 'per_frame',
                  'map': 'maps/start.bsp', 'metrics': {'mean_ms': 20, 'p95_ms': 25}}
        changed = copy.deepcopy(record)
        changed['provenance_verified'] = False
        class FakeStore:
            def reverify_record(self, identifier):
                return changed
            def find_run(self, identifier):
                return record
        service = EvidenceService(ROOT, enable_jobs=False)
        service.jobs = FakeStore()
        result = service.compare_retained_runs('baseline', 'candidate')
        self.assertEqual(result['comparability'], 'incomparable')
        self.assertEqual(result['performance'], 'not_checked')

    def test_overlay_diagnostic_not_performance_control(self):
        record = {'identity': {key: 'matching' for key in IDENTITY_FIELDS},
                  'validity': 'valid', 'provenance_verified': True, 'aggregation_kind': 'per_frame',
                  'map': 'maps/start.bsp', 'metrics': {'mean_ms': 20, 'p95_ms': 25}}
        record['identity']['instrumentation'] = {'stats_level': 3}
        result = compare_records(record, record)
        self.assertEqual(result['performance'], 'not_checked')


if __name__ == '__main__':
    unittest.main()
