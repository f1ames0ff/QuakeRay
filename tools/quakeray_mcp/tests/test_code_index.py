# Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

from pathlib import Path
import os
import unittest

from quakeray_mcp.code_index import CodeIndex
from quakeray_mcp.errors import EvidenceError


ROOT = Path(__file__).resolve().parents[3]


class CodeIndexTests(unittest.TestCase):
    def test_source_escape_rejected(self):
        index = CodeIndex(ROOT, Path('missing-clangd'))
        with self.assertRaises(EvidenceError):
            index.file('../private.cpp')

    def test_missing_backend_not_fabricated(self):
        index = CodeIndex(ROOT, Path('missing-clangd'))
        with self.assertRaises(EvidenceError):
            index.find('R_DrawViewModel', 'Quake/gl_rmain.c')

    @unittest.skipUnless(os.environ.get('QUAKERAY_TEST_CLANGD'), 'Explicit clangd integration opt-in')
    def test_real_viewmodel_call_path(self):
        index = CodeIndex(ROOT, os.environ['QUAKERAY_TEST_CLANGD'])
        result = index.find('R_DrawViewModel', 'Quake/gl_rmain.c')
        symbol = next(item for item in result['symbols'] if item['name'] == 'R_DrawViewModel')
        callers = index.related(symbol['symbol_id'], 'incomingCalls')
        self.assertIn('R_DrawViewModelTask', [item['from']['name'] for item in callers['edges']])
        self.assertEqual(callers['coverage'], 'resolved_edges_only')


if __name__ == '__main__':
    unittest.main()
