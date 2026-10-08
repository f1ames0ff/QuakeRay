import hashlib
import math
from pathlib import Path
import struct
import zlib

from .errors import EvidenceError


def decode_png(raw):
    if not raw.startswith(b'\x89PNG\r\n\x1a\n') or len(raw) > 32 * 1024 * 1024:
        raise EvidenceError('INVALID_IMAGE', 'A bounded PNG is required')
    offset, compressed = 8, bytearray()
    width = height = channels = None
    ended = False
    while offset + 12 <= len(raw):
        size = struct.unpack('>I', raw[offset:offset + 4])[0]
        kind = raw[offset + 4:offset + 8]
        data = raw[offset + 8:offset + 8 + size]
        if offset + size + 12 > len(raw) or zlib.crc32(kind + data) & 0xffffffff != struct.unpack('>I', raw[offset + size + 8:offset + size + 12])[0]:
            raise EvidenceError('INVALID_IMAGE', 'PNG chunk is incomplete or corrupt')
        if kind == b'IHDR':
            width, height, bits, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', data)
            if bits != 8 or color not in (2, 6) or compression or filtering or interlace:
                raise EvidenceError('UNSUPPORTED_IMAGE', 'Only noninterlaced RGB/RGBA 8-bit PNG is supported')
            channels = 3 if color == 2 else 4
            if width * height > 16777216 or not width or not height:
                raise EvidenceError('INVALID_IMAGE', 'PNG dimensions exceed bounds')
        elif kind == b'IDAT':
            compressed.extend(data)
        elif kind == b'IEND':
            ended = True
            break
        offset += size + 12
    if channels is None or not ended:
        raise EvidenceError('INVALID_IMAGE', 'PNG header missing')
    stride = width * channels
    expected = height * (stride + 1)
    inflater = zlib.decompressobj()
    decoded = inflater.decompress(bytes(compressed), expected + 1)
    if len(decoded) != expected or not inflater.eof:
        raise EvidenceError('INVALID_IMAGE', 'PNG data size does not match dimensions')
    pixels = bytearray()
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filter_type = decoded[start]
        row = bytearray(decoded[start + 1:start + stride + 1])
        if filter_type > 4:
            raise EvidenceError('INVALID_IMAGE', 'Invalid PNG filter')
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            above = previous[x]
            diagonal = previous[x - channels] if x >= channels else 0
            correction = 0
            if filter_type == 1:
                correction = left
            elif filter_type == 2:
                correction = above
            elif filter_type == 3:
                correction = (left + above) // 2
            elif filter_type == 4:
                predicted = left + above - diagonal
                distances = [abs(predicted - value) for value in (left, above, diagonal)]
                correction = (left, above, diagonal)[distances.index(min(distances))]
            row[x] = (row[x] + correction) & 255
        pixels.extend(row)
        previous = row
    return width, height, channels, pixels


def compare_png(a, b):
    left, right = decode_png(a), decode_png(b)
    if left[:3] != right[:3]:
        raise EvidenceError('INCOMPARABLE', 'Image dimensions/channels differ')
    squared = sum((x - y) ** 2 for x, y in zip(left[3], right[3]))
    return {'width': left[0], 'height': left[1], 'channels': left[2],
            'rmse': math.sqrt(squared / len(left[3])), 'different_channels': sum(x != y for x, y in zip(left[3], right[3])),
            'baseline_sha256': hashlib.sha256(a).hexdigest(), 'candidate_sha256': hashlib.sha256(b).hexdigest(),
            'visual_acceptance': 'not_checked', 'noise_calibration': 'required', 'geometry_equivalence': 'not_checked'}
