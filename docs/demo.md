---
title: Live demo
---

<script setup>
import { defineAsyncComponent } from 'vue'
// Async plus <ClientOnly> keeps MapLibre and duckdb-wasm out of the SSR build,
// which runs in Node and has no window to give them.
const RoutingDemo = defineAsyncComponent(() => import('./.vitepress/components/RoutingDemo.vue'))
</script>

# Live demo

Routing over 1,912 streets of central Amsterdam, in your browser. There is no
server: DuckDB is compiled to WebAssembly, duckrouting is installed into it
from the community repository, and every route below comes back from a SQL
query run a few milliseconds earlier.

The map loads immediately. The query engine is about 36&nbsp;MB, so it only
downloads when you ask for it.

<ClientOnly>
  <RoutingDemo />
</ClientOnly>

## What it is running

The demo is the extension you would get from `INSTALL duckrouting FROM
community`, against the same edges contract every page here describes:

```sql
INSTALL duckrouting FROM community;
LOAD duckrouting;
```

The network was extracted from OpenStreetMap once and committed as GeoJSON, one
feature per edge. Each carries what the routing functions need, and nothing
else:

| Column | Meaning |
| --- | --- |
| `id`, `source`, `target` | the edge and the vertices it joins |
| `cost` | length in metres |
| `reverse_cost` | the same length, or `-1` on a one-way street |
| `capacity`, `reverse_capacity` | vehicles per hour, for the flow functions |
| `x1, y1, x2, y2` | endpoint coordinates, which A\* estimates from |

1,417 of the 1,912 edges are one-way, which is why a route out and the route
back are rarely the same — see [The edges query](/guide/edges-query) for why a
negative `reverse_cost` means "not traversable" rather than "cheap".

## Pick points

**Shortest path** runs [`duckrouting_dijkstra`](/functions/dijkstra) between the
two vertices you click, and draws every edge it returns.

**Driving distance** runs
[`duckrouting_driving_distance`](/functions/dijkstra#duckrouting-driving-distance)
from one vertex out to a cost radius — the service area around a point. Drag
the slider and watch it grow along the streets rather than as a circle.

**K alternatives** runs [`duckrouting_ksp`](/functions/ksp), Yen's algorithm,
and draws the cheapest route over the alternatives so you can see how much
detour the second and third best cost.

**Maximum flow** runs
[`duckrouting_edmonds_karp`](/functions/flow) over the capacity columns instead
of the costs. It answers a different question from the others: not how do I get
there, but how much can get there at once. Line width is the volume on that
edge, and the red segments are **saturated** — full to capacity. Those are the
reason the total is the number it is; widening anything else changes nothing.

Clicking two points on ordinary residential streets usually gives one corridor
at a single volume, because the bottleneck is the street you started on. Pick
two points on bigger roads and the flow splits across parallel routes.

**Travelling salesman** chains three functions. Click three or more stops and
it builds a cost matrix over them with
[`duckrouting_dijkstra_cost_matrix`](/functions/dijkstra), puts them in order
with [`duckrouting_tsp`](/functions/tsp), then hands that order to
[`duckrouting_dijkstra_via`](/functions/dijkstra#duckrouting-dijkstra-via) to
turn it back into streets. The tour re-solves on every new stop, and the order
it chose is printed with the result — rarely the order you clicked.

## Whole network

These four need no clicks. They run over all 1,912 streets at once and say
something about the shape of the network rather than about a journey through
it.

**Spanning tree** runs [`duckrouting_kruskal`](/functions/spanning-trees). 1,483
of the streets are enough to keep every junction reachable; the 429 drawn in
amber are redundancy — remove any one and nothing is cut off.

**One-way traps** runs
[`duckrouting_strong_components`](/functions/components). Strongly connected
means every junction can reach every other *following edge direction*. Central
Amsterdam splits into 126 such components: one body of 1,342 junctions, and 125
small pockets holding 142 junctions between them that you can drive into but
not back out of, or the reverse.

**Critical links** runs [`duckrouting_bridges`](/functions/components) and
[`duckrouting_articulation_points`](/functions/components) together — the 169
edges and 148 junctions whose removal would split the network. In a canal city
a good number of them are literally bridges.

**Centrality** runs
[`duckrouting_betweenness_centrality`](/functions/analysis#duckrouting-betweenness-centrality),
colouring every junction by how often it lies on a shortest path between two
others. The arteries come out red.

**Contraction** runs [`duckrouting_contraction`](/functions/contraction),
which absorbs dead ends and collapses chains of degree-two junctions into
shortcut edges. The junctions it can dispose of are marked; what is left is the
smaller graph a preprocessed router would actually search.

Whichever mode is selected, the exact SQL that produced what you are looking at
is printed under the map.

::: tip Why the engine is a separate download
DuckDB-Wasm is ~36&nbsp;MB and duckrouting adds 1.3&nbsp;MB on top. Loading
that on page view would make every visit to this page expensive, so the map and
the network (97&nbsp;KB) come first and the engine starts on request.
:::

::: warning Map data
Street geometry, one-way restrictions and road classes come from
[OpenStreetMap](https://www.openstreetmap.org/copyright), licensed ODbL.
Capacities are rough per-lane planning figures derived from the road class, not
measurements — they are there to make the flow demo mean something, not to
model Amsterdam's traffic.
:::
