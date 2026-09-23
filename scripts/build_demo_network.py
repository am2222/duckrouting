#!/usr/bin/env python3
"""Builds the road network the docs demo routes over.

Fetches a small area from OpenStreetMap and writes it as GeoJSON in the shape
duckrouting's edges query wants: id, source, target, cost, reverse_cost, the
four A* coordinates, and capacities for the flow functions.

The output is committed, so this only runs when the area changes. Run it with
no arguments to regenerate docs/public/demo-network.geojson in place.

Noding is the part that usually goes wrong, and OSM makes it easy: a way is a
list of node ids, and two ways that meet share the node id rather than merely
touching geometrically. So an edge is a run of a way between two nodes that
appear in more than one way, and no intersection test is needed.

OpenStreetMap data is ODbL. Anything rendered from this file has to carry the
attribution, which the demo component puts under the map.
"""

import json
import math
import os
import sys
import urllib.request
from collections import defaultdict, deque

# The main instance rate-limits and sheds load, and this script is rare enough
# that waiting out a 504 is not worth it. Tried in order.
OVERPASS = (
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
)

# A slice of central Amsterdam: the canal belt west of the centre. Chosen
# because the canals force routes to run to a bridge rather than straight at
# the target, which makes a shortest path visibly a shortest path, and because
# the area is thick with one-way streets.
BBOX = (52.362, 4.872, 52.382, 4.906)  # south, west, north, east

# Ways that a car can use. Footpaths and cycleways are left out: the demo talks
# about driving distance and one-way streets, so a pedestrian shortcut through
# the middle of it would be misleading.
DRIVABLE = (
    "motorway|trunk|primary|secondary|tertiary|unclassified|residential|"
    "living_street|motorway_link|trunk_link|primary_link|secondary_link|"
    "tertiary_link"
)

# Vehicles per hour one lane of each class carries. These are rough planning
# figures, not measurements -- the flow demo is about showing where a network
# bottlenecks, and the ratios between classes are what matter for that.
LANE_CAPACITY = {
    "motorway": 2000, "trunk": 1800, "primary": 1500, "secondary": 1200,
    "tertiary": 1000, "unclassified": 800, "residential": 600,
    "living_street": 300,
}


def fetch(query):
    last = None
    for endpoint in OVERPASS:
        request = urllib.request.Request(
            endpoint,
            data=("data=" + query).encode(),
            headers={"User-Agent": "duckrouting-docs-demo/1.0"},
        )
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                return json.load(response)
        except Exception as error:  # noqa: BLE001 - any failure moves to the next mirror
            print(f"  {endpoint}: {error}", file=sys.stderr)
            last = error
    raise SystemExit(f"every Overpass endpoint failed; last error: {last}")


def haversine(a, b):
    """Metres between two (lon, lat) pairs."""
    radius = 6371000.0
    lon1, lat1 = math.radians(a[0]), math.radians(a[1])
    lon2, lat2 = math.radians(b[0]), math.radians(b[1])
    h = (math.sin((lat2 - lat1) / 2) ** 2
         + math.cos(lat1) * math.cos(lat2) * math.sin((lon2 - lon1) / 2) ** 2)
    return 2 * radius * math.asin(math.sqrt(h))


def one_way(tags):
    """Whether the way is one-way, and whether it runs against its node order."""
    value = tags.get("oneway", "")
    if value in ("yes", "true", "1"):
        return True, False
    if value in ("-1", "reverse"):
        return True, True
    if tags.get("junction") in ("roundabout", "circular") and value != "no":
        return True, False
    return False, False


def capacity_of(tags):
    base = LANE_CAPACITY.get(tags.get("highway", ""), 600)
    try:
        lanes = max(1, int(float(tags.get("lanes", "2"))))
    except ValueError:
        lanes = 2
    return base * lanes


def largest_component(edges):
    """Keeps only the biggest connected component.

    A demo that drops the user on an isolated stub looks like a broken routing
    engine rather than a disconnected graph, so the stubs go.
    """
    neighbours = defaultdict(set)
    for edge in edges:
        neighbours[edge["source"]].add(edge["target"])
        neighbours[edge["target"]].add(edge["source"])

    seen, best = set(), set()
    for start in neighbours:
        if start in seen:
            continue
        component, queue = set(), deque([start])
        while queue:
            node = queue.popleft()
            if node in component:
                continue
            component.add(node)
            queue.extend(n for n in neighbours[node] if n not in component)
        seen |= component
        if len(component) > len(best):
            best = component

    return [e for e in edges if e["source"] in best and e["target"] in best]


def build():
    query = (
        f'[out:json][timeout:180];'
        f'way["highway"~"^({DRIVABLE})$"]'
        f'({BBOX[0]},{BBOX[1]},{BBOX[2]},{BBOX[3]});'
        f'(._;>;);out body;'
    )
    print(f"querying overpass for {BBOX} ...", file=sys.stderr)
    data = fetch(query)

    coordinates = {}
    ways = []
    for element in data["elements"]:
        if element["type"] == "node":
            coordinates[element["id"]] = (element["lon"], element["lat"])
        elif element["type"] == "way":
            ways.append(element)
    print(f"  {len(ways)} ways, {len(coordinates)} nodes", file=sys.stderr)

    # A node shared by more than one way is a junction; so is either end of a
    # way, because that is where it meets whatever continues from it.
    uses = defaultdict(int)
    for way in ways:
        for node in way["nodes"]:
            uses[node] += 1
    junctions = {n for n, count in uses.items() if count > 1}
    for way in ways:
        if way["nodes"]:
            junctions.add(way["nodes"][0])
            junctions.add(way["nodes"][-1])

    edges = []
    for way in ways:
        tags = way.get("tags", {})
        oneway, reversed_order = one_way(tags)
        capacity = capacity_of(tags)
        nodes = [n for n in way["nodes"] if n in coordinates]
        if reversed_order:
            nodes = list(reversed(nodes))

        run = []
        for node in nodes:
            run.append(node)
            if len(run) > 1 and node in junctions:
                points = [coordinates[n] for n in run]
                length = sum(haversine(points[i], points[i + 1])
                             for i in range(len(points) - 1))
                if length > 0:
                    edges.append({
                        "source": run[0], "target": run[-1],
                        "cost": round(length, 2),
                        # pgRouting's convention, which duckrouting follows: a
                        # negative reverse_cost means the edge does not exist in
                        # that direction. It is how a one-way street is written.
                        "reverse_cost": -1 if oneway else round(length, 2),
                        "capacity": capacity,
                        "reverse_capacity": -1 if oneway else capacity,
                        "points": points,
                    })
                run = [node]

    edges = largest_component(edges)

    # Dense vertex ids. OSM's are int64 and meaningless on screen; the demo
    # shows them, and 1..N reads far better than 25489301.
    vertices, features = {}, []
    for index, edge in enumerate(edges, start=1):
        for node in (edge["source"], edge["target"]):
            if node not in vertices:
                vertices[node] = len(vertices) + 1
        points = edge["points"]
        features.append({
            "type": "Feature",
            "properties": {
                "id": index,
                "source": vertices[edge["source"]],
                "target": vertices[edge["target"]],
                "cost": edge["cost"],
                "reverse_cost": edge["reverse_cost"],
                "capacity": edge["capacity"],
                "reverse_capacity": edge["reverse_capacity"],
                # A* estimates from the endpoints, so it needs them as columns
                # rather than having to reach into the geometry.
                "x1": round(points[0][0], 6), "y1": round(points[0][1], 6),
                "x2": round(points[-1][0], 6), "y2": round(points[-1][1], 6),
            },
            "geometry": {
                "type": "LineString",
                "coordinates": [[round(x, 6), round(y, 6)] for x, y in points],
            },
        })

    return {
        "type": "FeatureCollection",
        "attribution": "© OpenStreetMap contributors (ODbL)",
        "features": features,
    }


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "docs", "public", "demo-network.geojson")
    collection = build()
    with open(out, "w") as handle:
        json.dump(collection, handle, separators=(",", ":"))
    nodes = {f["properties"]["source"] for f in collection["features"]}
    nodes |= {f["properties"]["target"] for f in collection["features"]}
    print(f"wrote {len(collection['features'])} edges, {len(nodes)} vertices, "
          f"{os.path.getsize(out) / 1024:.0f} KB -> {os.path.relpath(out)}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
