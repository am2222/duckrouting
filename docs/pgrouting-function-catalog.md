# pgRouting function catalog

A survey of every function pgRouting exposes, classified by whether the
**algorithm** comes from the Boost Graph Library or from pgRouting's own C++.
This drives the duckrouting port order: the BGL-backed functions are reachable
by writing fresh glue over Boost, while the hand-written ones would have to be
reimplemented from scratch.

Derived from the pgRouting source tree (`src/`, `include/`), not from the
prose docs -- each row was confirmed by finding the actual `boost::` call.

## Read this first: the graph itself is always Boost

`include/cpp_common/base_graph.hpp` is a `boost::adjacency_list`. Practically
every pgRouting C++ function therefore touches Boost *somewhere*. The split
below is the one that matters for porting:

- **Group 1** -- BGL supplies the algorithm. Glue code only.
- **Group 2** -- pgRouting wrote the algorithm. Boost is just the container, or absent.
- **Group 3** -- no C++ at all; pure SQL / PL-pgSQL, some needing PostGIS.

## Licensing

pgRouting is **GPL-2.0-or-later**. Group 2 source cannot be lifted into a
permissively-licensed extension -- those need clean-room reimplementation.
Group 1 is mostly glue over Boost, which is BSL-1.0, so writing our own
wrappers is unencumbered.

---

## Group 1 -- Boost supplies the algorithm

| Function(s) | BGL algorithm | duckrouting |
| --- | --- | --- |
| `pgr_dijkstra` | `dijkstra_shortest_paths` / `_no_init` | `duckrouting_dijkstra` |
| `pgr_dijkstraCost` | `dijkstra_shortest_paths` | `duckrouting_dijkstra_cost` |
| `pgr_dijkstraCostMatrix` | `dijkstra_shortest_paths` | `duckrouting_dijkstra_cost_matrix` |
| `pgr_dijkstraVia` | `dijkstra_shortest_paths` | `duckrouting_dijkstra_via`, incl. `strict` and `u_turn_on_edge` |
| `pgr_dijkstraNear` | `dijkstra_shortest_paths` | `duckrouting_dijkstra_near` |
| `pgr_dijkstraNearCost` | `dijkstra_shortest_paths` | `duckrouting_dijkstra_near_cost` |
| `pgr_drivingDistance` | `dijkstra_shortest_paths` + visitor | `duckrouting_driving_distance` |
| `pgr_withPointsDD` | `dijkstra_shortest_paths` + visitor | not yet |
| `pgr_aStar`, `pgr_aStarCost`, `pgr_aStarCostMatrix` | `astar_search` | |
| `pgr_floydWarshall` | `floyd_warshall_all_pairs_shortest_paths` | `duckrouting_floyd_warshall` |
| `pgr_johnson` | `johnson_all_pairs_shortest_paths` | `duckrouting_johnson` |
| `pgr_bellmanFord` | `bellman_ford_shortest_paths` | `duckrouting_bellman_ford` |
| `pgr_dagShortestPath` | `dag_shortest_paths` | `duckrouting_dag_shortest_path` |
| `pgr_connectedComponents` | `connected_components` | `duckrouting_connected_components` |
| `pgr_strongComponents` | `strong_components` | `duckrouting_strong_components` |
| `pgr_biconnectedComponents` | `biconnected_components` | `duckrouting_biconnected_components` |
| `pgr_articulationPoints` | `articulation_points` | `duckrouting_articulation_points` |
| `pgr_bridges` | single-edge `biconnected_components` | `duckrouting_bridges` |
| `pgr_makeConnected` | `make_connected` | `duckrouting_make_connected` |
| `pgr_kruskal` | `kruskal_minimum_spanning_tree` | `duckrouting_kruskal` |
| `pgr_kruskalBFS`, `pgr_kruskalDFS`, `pgr_kruskalDD` | `kruskal_minimum_spanning_tree` + traversal | `duckrouting_kruskal_bfs` / `_dfs` / `_dd` |
| `pgr_prim` | `prim_minimum_spanning_tree` | `duckrouting_prim` |
| `pgr_primBFS`, `pgr_primDFS`, `pgr_primDD` | `prim_minimum_spanning_tree` + traversal | `duckrouting_prim_bfs` / `_dfs` / `_dd` |
| `pgr_breadthFirstSearch` | `breadth_first_search` | `duckrouting_breadth_first_search` |
| `pgr_depthFirstSearch` | `depth_first_search` / `undirected_dfs` | `duckrouting_depth_first_search` |
| `pgr_maxFlow`, `pgr_pushRelabel` | `push_relabel_max_flow` | |
| `pgr_edmondsKarp` | `edmonds_karp_max_flow` | |
| `pgr_boykovKolmogorov` | `boykov_kolmogorov_max_flow` | |
| `pgr_edgeDisjointPaths` | runs on the same BGL flow graph | |
| `pgr_maxFlowMinCost`, `pgr_maxFlowMinCost_Cost` | `successive_shortest_path_nonnegative_weights` + `find_flow_cost` | |
| `pgr_maxCardinalityMatch` | `edmonds_maximum_cardinality_matching` | |
| `pgr_stoerWagner` | `stoer_wagner_min_cut` | |
| `pgr_transitiveClosure` | `transitive_closure` | `duckrouting_transitive_closure` |
| `pgr_lengauerTarjanDominatorTree` | `dominator_tree` | |
| `pgr_hawickCircuits` | `hawick_circuits` | |
| `pgr_bandwidth` | `bandwidth` | |
| `pgr_betweennessCentrality` | `brandes_betweenness_centrality` | |
| `pgr_sequentialVertexColoring` | `sequential_vertex_coloring` | |
| `pgr_edgeColoring` | `edge_coloring` | |
| `pgr_bipartite` | `is_bipartite` | |
| `pgr_cuthillMckeeOrdering` | `cuthill_mckee_ordering` | `duckrouting_cuthill_mckee_ordering` |
| `pgr_kingOrdering` | `king_ordering` | `duckrouting_king_ordering` |
| `pgr_sloanOrdering` | `sloan_ordering` | `duckrouting_sloan_ordering` |
| `pgr_topologicalSort` | `topological_sort` | `duckrouting_topological_sort` |
| `pgr_boyerMyrvold`, `pgr_isPlanar` | `boyer_myrvold_planarity_test` | |
| `pgr_TSP`, `pgr_TSPeuclidean` | `metric_tsp_approx_tour` | |
| `pgr_contractionHierarchies` | `dijkstra_shortest_paths` over a CH `adjacency_list` | |

## Group 2 -- pgRouting's own algorithm

| Function(s) | What it actually is |
| --- | --- |
| `pgr_bdDijkstra`, `pgr_bdDijkstraCost`, `pgr_bdDijkstraCostMatrix` | `cpp_common/bidirectional.hpp` -- own `std::priority_queue` bidirectional search |
| `pgr_bdAstar`, `pgr_bdAstarCost`, `pgr_bdAstarCostMatrix` | same hand-written bidirectional base, plus a heuristic |
| `pgr_KSP` | `include/yen/ksp.hpp` -- own Yen's algorithm. Reimplemented independently as `duckrouting_ksp`; Boost has no k-shortest-paths routine |
| `pgr_withPointsKSP`, `pgr_turnRestrictedPath` | Yen over the withPoints / turn-restriction graphs |
| `pgr_trsp`, `pgr_trspVia`, `pgr_trsp_withPoints`, `pgr_trspVia_withPoints` | `trsp/trspHandler.cpp` -- own turn-restriction search. Zero `boost::` symbols |
| `pgr_withPoints`, `pgr_withPointsCost`, `pgr_withPointsCostMatrix`, `pgr_withPointsVia` | own edge-splitting graph rewrite, then delegates |
| `pgr_edwardMoore` | own SPFA; Boost only for edge iteration |
| `pgr_binaryBreadthFirstSearch` | own 0-1 BFS; Boost only for edge iteration |
| `pgr_chinesePostman`, `pgr_chinesePostmanCost` | own. Zero `boost::` symbols |
| `pgr_lineGraph`, `pgr_lineGraphFull` | own line-graph construction |
| `pgr_pickDeliver`, `pgr_pickDeliverEuclidean` | own VRPPDTW heuristic, 17 source files. Zero `boost::` symbols |
| `pgr_vrpOneDepot` | own legacy VRP code |
| `pgr_contraction`, `pgr_deadEndContraction`, `pgr_linearContraction` | own contraction operators applied to a `boost::adjacency_list` |

## Group 3 -- no C++; pure SQL / PL-pgSQL

| Function | Notes |
| --- | --- |
| `pgr_extractVertices` | derives a vertex table from edges |
| `pgr_findCloseEdges` | needs PostGIS geometry predicates |
| `pgr_separateCrossing` | needs PostGIS |
| `pgr_separateTouching` | needs PostGIS |
| `pgr_degree` | vertex degree over the edge table |
| `pgr_version`, `pgr_full_version` | metadata |

## Tally

| Group | Count |
| --- | --- |
| 1 -- Boost algorithm | ~55 |
| 2 -- pgRouting's own | ~30 |
| 3 -- pure SQL | 7 |

---

## Porting notes: where duckrouting differs from pgRouting

Both of these were found by running pgRouting's own test corpus
(`docqueries/dijkstra/`, `pgtap/dijkstra/`) against `duckrouting_dijkstra`.
See `test/sql/dijkstra.test`.

### Equal-cost paths tie-break differently

pgRouting's q93 and q133 ask for `12 -> 7` undirected. Two paths cost exactly
2: `12-(e12)->8-(e10)->7`, which pgRouting reports, and
`12-(e11)->11-(e8)->7`, which Boost reports. Dijkstra does not define which
equal-cost path wins, so this is not a defect in either implementation. The
tests assert hop count, endpoints and total cost for those cases, plus a check
that every reported edge really connects its two reported nodes.

The vertex orderings are the same story: `cuthillMckee`, `king` and `sloan`
return a permutation of the vertices, and the order among equal-degree vertices
is implementation-defined. Boost emits Cuthill-McKee and King *reversed*;
pgRouting undoes that by writing into `inv_permutation.rbegin()`, and so does
duckrouting -- which is why both agree on the `13, 14, 2, 4` prefix even though
the tails differ. A topological order is likewise not unique.

The spanning-tree family has the strongest form of this: every edge in the
sample graph costs 1, so the minimum spanning tree is massively non-unique --
pgRouting's own `pgr_kruskal` and `pgr_prim` return different edge sets from
each other on the same graph. The tests assert what any MST must satisfy (edge
count, total weight, connectivity preserved) rather than a fixed edge set.
`duckrouting_prim_dd` does happen to reproduce pgRouting's output exactly, and
is pinned.

`duckrouting_make_connected` differs the same way: joining n components takes
n-1 edges, but *which* vertex stands for each component is Boost's choice and
follows vertex ordering. pgRouting reports `(5,2)` and `(4,13)`; we report
`(9,2)` and `(4,13)`. Both genuinely connect the graph, so the tests assert the
count and that each pair really spans two different components.

The same thing happens in `duckrouting_ksp`: for `6 -> 17` with `k = 2`, both
paths cost 4 and pgRouting reports them in the opposite order. The tests assert
the *set* of node sequences and their costs, never the `path_id` ordering.

### Infinite edge costs need a sentinel (resolved)

`pgtap/.../edge_cases/infinity_cost.pg` sets an edge to `'Infinity'` and expects
routes through it to return `agg_cost = Infinity`. Boost alone cannot do this:
its relaxation test is `dist[u] + w < dist[v]`, which for an infinite weight
becomes `inf < inf` and is false, so the edge is never relaxed and the route
disappears. A standalone Boost probe confirmed this under both
`distance_inf` settings.

pgRouting solves it outside the algorithm, in two halves:

- on ingest, `src/cpp_common/pgdata_fetchers.cpp` maps `isinf(cost)` to
  `std::numeric_limits<double>::max()`, a large *finite* weight Boost will relax
  given that `distance_inf` is a true infinity (`DBL_MAX < inf` holds);
- on output, `to_inf()` in `src/cpp_common/to_postgres.cpp` maps values within
  `1.0` of `DBL_MAX` back to `Infinity`.

duckrouting now does the same, in `EncodeInfinity` / `DecodeInfinity`
(`src/include/duckrouting/edges.hpp`), and matches pgRouting's expected output.
One consequence inherited from pgRouting: `-Infinity` is also mapped onto the
sentinel, so it becomes a maximally expensive edge rather than "no edge",
because the `isinf` test runs before the `cost < 0` test.
