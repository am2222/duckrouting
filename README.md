# duckrouting

Graph routing for DuckDB. Shortest paths, flow, spanning trees, contraction and
more — following [pgRouting](https://pgrouting.org/)'s function semantics and
powered by the [Boost Graph Library](https://www.boost.org/doc/libs/release/libs/graph/).

**90 of pgRouting's 93 functions**, checked against pgRouting's own published
output. [Documentation](https://am2222.github.io/duckrouting/) ·
[Function catalog](docs/pgrouting-function-catalog.md)

```sql
INSTALL duckrouting FROM community;
LOAD duckrouting;
```

## A first route

There is no topology to build and no geometry required. Any table with an id,
two endpoints and a cost is a graph:

```sql
CREATE TABLE edges (
  id BIGINT, source BIGINT, target BIGINT, cost DOUBLE, reverse_cost DOUBLE
);
INSERT INTO edges VALUES
  (1, 5, 6, 1, 1), (2, 6, 10, -1, 1), (3, 10, 15, -1, 1), (4, 6, 7, 1, 1);

SELECT node, edge, agg_cost FROM duckrouting_dijkstra(
  'SELECT id, source, target, cost, reverse_cost FROM edges', 5, 7);
-- node | edge | agg_cost
--    5 |    1 |      0.0
--    6 |    4 |      1.0
--    7 |   -1 |      2.0
```

The first argument is a **string containing SQL**, not a table. That is how
pgRouting works, and keeping it means the graph can be filtered, joined or
computed on the fly without materialising a separate topology.

## The edges query

| Column | Type | |
| --- | --- | --- |
| `id` | `BIGINT` | required |
| `source`, `target` | `BIGINT` | required |
| `cost` | `DOUBLE` | required — weight of `source` → `target` |
| `reverse_cost` | `DOUBLE` | optional — weight of `target` → `source` |

**A negative cost means the edge does not exist in that direction.** It is not a
cheap edge and not an error: it is how one-way streets are written, and it is
pgRouting's convention. A missing or `NULL` `reverse_cost` behaves as `-1`, so
omitting it makes every edge strictly one-way.

Three families need more: A\* wants `x1, y1, x2, y2` for its heuristic, the flow
functions want `capacity` and `reverse_capacity` instead of costs, and the
`withPoints` family takes a second query of points sitting partway along edges.

## What's here

| Family | Functions |
| --- | --- |
| [Dijkstra](https://am2222.github.io/duckrouting/functions/dijkstra) | `dijkstra`, `_cost`, `_cost_matrix`, `_via`, `_near`, `_near_cost`, `driving_distance` |
| [A\*](https://am2222.github.io/duckrouting/functions/astar) | `astar`, `_cost`, `_cost_matrix` |
| Bidirectional | `bd_dijkstra`, `bd_astar`, and their cost and matrix forms |
| [K shortest paths](https://am2222.github.io/duckrouting/functions/ksp) | `ksp` (Yen), `with_points_ksp` |
| [All pairs](https://am2222.github.io/duckrouting/functions/all-pairs) | `floyd_warshall`, `johnson` |
| [Components](https://am2222.github.io/duckrouting/functions/components) | `connected_components`, `strong_components`, `biconnected_components`, `articulation_points`, `bridges`, `make_connected` |
| [Spanning trees](https://am2222.github.io/duckrouting/functions/spanning-trees) | `kruskal`, `prim`, and their BFS/DFS/DD traversals |
| [Traversal & ordering](https://am2222.github.io/duckrouting/functions/traversal) | breadth/depth first search, `bellman_ford`, `dag_shortest_path`, `transitive_closure`, four vertex orderings |
| [Flow](https://am2222.github.io/duckrouting/functions/flow) | `max_flow`, `push_relabel`, `edmonds_karp`, `boykov_kolmogorov`, `max_flow_min_cost`, `edge_disjoint_paths`, `max_cardinality_match` |
| [Analysis](https://am2222.github.io/duckrouting/functions/analysis) | colouring, planarity, betweenness centrality, min cut, circuits, dominator tree |
| [Contraction & points](https://am2222.github.io/duckrouting/functions/contraction) | `contraction_hierarchies`, `dead_end_contraction`, `linear_contraction`, the `with_points` family |
| Turn restrictions | `trsp`, `trsp_via`, `trsp_with_points`, `trsp_via_with_points` |
| [Travelling salesman](https://am2222.github.io/duckrouting/functions/tsp) | `tsp`, `tsp_euclidean` |
| Other | `chinese_postman`, `line_graph`, `extract_vertices`, `degree` |

Three geometry helpers — `find_close_edges`, `separate_crossing`,
`separate_touching` — are SQL macros over DuckDB's `spatial` extension. Run
`INSTALL spatial; LOAD spatial;` before calling one; nothing else needs it.

**Not implemented by choice:** `pgr_pickDeliver`, `pgr_pickDeliverEuclidean` and
`pgr_vrpOneDepot` — vehicle routing with capacities and time windows. Matching
pgRouting here would mean vendoring its GPL-2.0-or-later VRP source, which would
relicense this extension as a whole and close the door on anyone embedding it in
a commercial product. Three functions out of 93 is not worth that. See the
[catalog](docs/pgrouting-function-catalog.md) for the reasoning.

## How results are verified

Expected results come from pgRouting's own documentation queries and test suite,
not from hand-written expectations. Where the two legitimately differ — because
a shortest path ties, a spanning tree is not unique, or a matching has several
maximum solutions — the difference is documented in the
[catalog](docs/pgrouting-function-catalog.md) and the tests assert the
properties that *are* determined.

A few differences worth knowing about up front:

- **Equal-cost paths** tie-break arbitrarily. `6 → 12` costs 3 whether you go
  via vertex 8 or vertex 11.
- **Minimum spanning trees** are not unique when edges tie; pgRouting's own
  `kruskal` and `prim` disagree with each other on the sample graph.
- **Contraction hierarchies** depend on contraction order, which pgRouting does
  not document.

## Building

```shell
git clone --recurse-submodules https://github.com/am2222/duckrouting
cd duckrouting

git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake

make
```

Boost.Graph comes from vcpkg, pinned by the `builtin-baseline` in `vcpkg.json`
to the same commit DuckDB's own CI uses, so local and CI builds resolve the same
version.

Then:

```shell
./build/release/duckdb          # a shell with the extension already loaded
make test                       # run the SQL test suite
```

The geometry tests need `INSTALL spatial` and are skipped otherwise.

## Licence

MIT. Note that pgRouting itself is GPL-2.0-or-later: duckrouting follows its
*semantics* and is checked against its published *output*, but shares none of
its source.
