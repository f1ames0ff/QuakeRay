import struct
import unittest
import zlib

from quakeray_mcp.errors import EvidenceError
from quakeray_mcp.images import compare_png


def png(value):
    def chunk(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes([0, value, 0, 0, 255]))) + chunk(b'IEND', b'')


class ImageTests(unittest.TestCase):
    def test_equal_pixels_not_visual_pass(self):
        result = compare_png(png(10), png(10))
        self.assertEqual(result['rmse'], 0)
        self.assertEqual(result['visual_acceptance'], 'not_checked')

    def test_changed_pixel_diagnostic(self):
        result = compare_png(png(0), png(20))
        self.assertEqual(result['rmse'], 10)
        self.assertEqual(result['different_channels'], 1)

    def test_corrupt_crc_rejected(self):
        raw = bytearray(png(1))
        raw[-1] ^= 1
        with self.assertRaises(EvidenceError):
            compare_png(bytes(raw), png(1))

    def test_missing_end_chunk_rejected(self):
        with self.assertRaises(EvidenceError):
            compare_png(png(1)[:-12], png(1))
