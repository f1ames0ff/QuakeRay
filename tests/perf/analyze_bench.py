import re
import sys

PATH = sys.argv[1] if len(sys.argv) > 1 else r'build\Debug\ad\benchmark.log'
COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 4

HEAD = re.compile(r'^#\s*rt_bench\s+(.*?)\s+demo=(\S+)\s+frames=(\d+)\s+seconds=([0-9.]+)\s+fps=([0-9.]+)\s+interrupted=(\d+)')
SLOT = re.compile(r'^cpu\.slot\s+(.+?)\s+avg_ms=([0-9.]+)\s+max_ms=([0-9.]+)')
CLUST = re.compile(r'^cpu\.cluster\s+lists\s+hits=(\d+)\s+misses=(\d+)\s+set=(\d+)\s+move=(\d+)\s+other=(\d+)')
SET = re.compile(r'^settings\s+(.*)$')

SLOTS = ['frame', 'clusters', 'clust lists', 'clust topup', 'clust tail', 'clust fill', 'clust upload', 'clust publish']

blocks = []
cur = None

try:
    handle = open(PATH, 'r', encoding='utf-8', errors='replace')
except OSError as error:
    sys.exit(f'cannot open {PATH}: {error}')

with handle:
    for line in handle:
        m = HEAD.match(line)
        if m:
            if cur:
                blocks.append(cur)
            cur = {
                'stamp': m.group(1), 'demo': m.group(2), 'frames': int(m.group(3)),
                'seconds': float(m.group(4)), 'fps': float(m.group(5)),
                'sampling': '?', 'reach': '?', 'reach_max': '?', 'vid': '?',
                'slots': {}, 'hits': '-', 'misses': '-',
            }
            continue
        if cur is None:
            continue
        m = SET.match(line)
        if m:
            body = m.group(1)
            for key, pattern in (('sampling', r'rt_cluster_sampling=(\S+)'),
                                 ('reach', r'rt_light_reach_static=(\S+)'),
                                 ('reach_dynamic', r'rt_light_reach_dynamic=(\S+)'),
                                 ('vid', r'vid=(\S+)')):
                mm = re.search(pattern, body)
                if mm:
                    cur[key] = mm.group(1)
            continue
        m = SLOT.match(line)
        if m:
            cur['slots'][m.group(1).strip()] = (float(m.group(2)), float(m.group(3)))
            continue
        m = CLUST.match(line)
        if m:
            cur['hits'], cur['misses'] = m.group(1), m.group(2)

if cur:
    blocks.append(cur)

if not blocks:
    sys.exit(f'no rt_bench blocks in {PATH}')

print(f'{PATH}: {len(blocks)} block(s), showing the last {min(COUNT, len(blocks))}')
print()

header = f"{'#':>3} {'stamp':19} {'demo':18} {'samp':>4} {'reach':>5} {'frames':>7} {'sec':>6} {'wallfps':>7} " + \
         ' '.join(f'{name[:12]:>12}' for name in SLOTS) + f" {'hits':>6} {'miss':>5}"

print(header)
for i, b in enumerate(blocks[-COUNT:], start=len(blocks) - min(COUNT, len(blocks))):
    row = (f"{i:>3} {b['stamp']:19} {b['demo'][:18]:18} {b['sampling']:>4} {b['reach']:>5} "
           f"{b['frames']:>7} {b['seconds']:>6.1f} {b['fps']:>7.1f} ")
    for name in SLOTS:
        value = b['slots'].get(name)
        row += f"{(value[0] if value else float('nan')):>12.2f} "
    row += f"{b['hits']:>6} {b['misses']:>5}"
    print(row)

print()
print('columns are cpu.slot averages, ms; "reach" is rt_light_reach_static; compare blocks with the same')
print('demo, vid, reach and sampling flag only.')
