import urllib.request, urllib.parse, json, os
from collections import Counter

EP = 'https://overpass-api.de/api/interpreter'
UA = 'porkchop-flock-map/0.1 (personal counter-surveillance research)'

def op(q, timeout=180):
    data = urllib.parse.urlencode({'data': q}).encode()
    req = urllib.request.Request(EP, data=data, headers={'User-Agent': UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)

# Huntsville, AL box: ~100 miles each direction from 34.7304,-86.5861
box = '(33.281,-88.345,36.179,-84.827)'
q = f'[out:json][timeout:120];node["surveillance:type"="ALPR"]{box};out;'
d = op(q)
nodes = [e for e in d.get('elements', []) if e.get('type') == 'node']
print('HUNTSVILLE_BOX_NODES:', len(nodes))

outdir = os.path.join(os.path.expanduser('~'), 'porkchop-contrib', 'mapdata')
os.makedirs(outdir, exist_ok=True)
path = os.path.join(outdir, 'flock_map_huntsville.csv')
with open(path, 'w', encoding='utf-8') as f:
    f.write('lat,lon,operator,direction\n')
    for e in nodes:
        t = e.get('tags', {})
        oper = (t.get('operator') or t.get('brand') or '').replace(',', ' ').replace('\n', ' ')
        dirn = (t.get('direction') or '').replace(',', ' ')
        f.write(f"{e['lat']:.6f},{e['lon']:.6f},{oper},{dirn}\n")
print('SAVED_FILE:', path)
print('FILE_BYTES:', os.path.getsize(path))

c = Counter((e.get('tags', {}).get('operator') or e.get('tags', {}).get('brand') or 'unknown') for e in nodes)
print('TOP_OPERATORS:', c.most_common(6))
