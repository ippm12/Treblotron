"""Conservative clearance/reachability checks for the seven starter courses.

Checks swept bounds and a ball-sized potential route around stationary geometry,
including paired portal transitions. Moving obstacles and timed lasers require
playtesting: the route check deliberately does not claim their timing is solvable.
"""
from collections import deque
import json
import math
from pathlib import Path

NAMES = ('tungsten-ridge', 'checkout-valley', 'ochemont', 'treble-beach',
         'the-big-fish', 'whistling-flights', 'double-dunes')

def check(root, names=NAMES):
    count = 0
    for name in names:
        directory = root / name
        manifest = json.loads((directory / 'course.json').read_text())
        assert manifest['theme'] == name and len(set(manifest['holes'])) == 9
        for hole_id in manifest['holes']:
            count += 1
            h = json.loads((directory / 'holes' / (hole_id + '.json')).read_text())
            label = f'{name}/{hole_id}'
            lo, hi = h['bounds']['topLeft'], h['bounds']['bottomRight']
            rails = {r['id']: r for r in h['rails']}
            def positions(obj, key='center'):
                x, y = obj[key]
                offsets = [s['offset'] for s in rails[obj['rail']]['stops']] if obj.get('rail') else [(0, 0)]
                return [(x+dx, y+dy) for dx, dy in offsets]
            obstacles = []
            for obj in h['walls'] + h['bumpers'] + [s for s in h['surfaces'] if s['kind'] == 'water']:
                size = obj.get('size', [2*obj.get('radius', 0)]*2)
                angle = math.radians(obj.get('angleDegrees', 0))
                c, s = abs(math.cos(angle)), abs(math.sin(angle))
                extent = [c*size[0]+s*size[1], s*size[0]+c*size[1]]
                points = positions(obj)
                rect = (min(p[0] for p in points)-extent[0]/2,
                        min(p[1] for p in points)-extent[1]/2,
                        max(p[0] for p in points)+extent[0]/2,
                        max(p[1] for p in points)+extent[1]/2)
                assert lo[0]-.001 <= rect[0] <= rect[2] <= hi[0]+.001 and lo[1]-.001 <= rect[1] <= rect[3] <= hi[1]+.001, (label, obj['id'], 'sweeps outside bounds')
                if not obj.get('rail'):
                    obstacles.append((obj, size, math.cos(angle), math.sin(angle)))
            def blocked(p, radius):
                for obj, size, c, s in obstacles:
                    dx, dy = p[0]-obj['center'][0], p[1]-obj['center'][1]
                    if 'radius' in obj:
                        if math.hypot(dx, dy) < obj['radius']+radius: return True
                    else:
                        x, y = abs(dx*c+dy*s), abs(-dx*s+dy*c)
                        if x <= size[0]/2+radius and y <= size[1]/2+radius: return True
                return False
            def clear(p, radius=19):
                x, y = p
                return (lo[0]+radius <= x <= hi[0]-radius and lo[1]+radius <= y <= hi[1]-radius
                        and not blocked(p, radius))
            assert clear(h['tee']['position']), (label, 'unsafe tee')
            for obj in [h['cup']] + h['portals']:
                for p in positions(obj, 'position' if obj is h['cup'] else 'center'):
                    assert clear(p, h['cup']['radius']+19), (label, obj['id'], 'unsafe cup/portal clearance')
            # Ten-pixel grid; inflate obstacles by half a grid edge beyond ball
            # radius so a clear node-to-node segment cannot cut a thin obstacle.
            step = 10
            width, height = int((hi[0]-lo[0])/step)+1, int((hi[1]-lo[1])/step)+1
            def cell(p): return (round((p[0]-lo[0])/step), round((p[1]-lo[1])/step))
            start = cell(h['tee']['position'])
            queue, seen = deque([start]), {start}
            portal_links = {}
            for portal in h['portals']:
                partner = next(p for p in h['portals'] if p['pair'] == portal['pair'] and p['id'] != portal['id'])
                for a in positions(portal):
                    portal_links.setdefault(cell(a), set()).update(cell(b) for b in positions(partner))
            while queue:
                x,y = queue.popleft()
                for nx,ny in [(x+1,y),(x-1,y),(x,y+1),(x,y-1), *portal_links.get((x,y), ())]:
                    if 0 <= nx < width and 0 <= ny < height and (nx,ny) not in seen and clear((lo[0]+nx*step,lo[1]+ny*step),21):
                        seen.add((nx,ny)); queue.append((nx,ny))
            for p in positions(h['cup'], 'position'):
                assert cell(p) in seen, (label, 'no potential route to cup, including portals')
    print(f'PASS {count} starter layouts: swept bounds, static clearances, potential routes including portals')

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--courses', nargs='+', choices=NAMES, default=NAMES)
    args = parser.parse_args()
    check(Path(__file__).resolve().parents[1] / 'assets/courses', args.courses)
