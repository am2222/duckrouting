# Traversal and ordering

Walking a graph, and arranging its vertices in a useful sequence.

## duckrouting_breadth_first_search

### Signatures

```sql
TABLE duckrouting_breadth_first_search (edges_sql VARCHAR, root BIGINT)
TABLE duckrouting_breadth_first_search (edges_sql VARCHAR, root BIGINT[])
TABLE duckrouting_breadth_first_search (edges_sql VARCHAR, root BIGINT,   max_depth BIGINT)
TABLE duckrouting_breadth_first_search (edges_sql VARCHAR, root BIGINT[], max_depth BIGINT)
-- also accepts max_depth => BIGINT and directed => BOOLEAN
```

### Description

Visits every vertex reachable from `root`, nearest first. Returns `seq`,
`depth`, `start_vid`, `pred`, `node`, `edge`, `cost` and `agg_cost` -- the same
shape as `driving_distance`.

Because it is breadth first, `depth` never decreases as `seq` advances. The
root is `depth` 0, its own predecessor, with `edge` `-1`.

### Example

```sql
SELECT seq, depth, pred, node, edge FROM duckrouting_breadth_first_search(
  'SELECT id, source, target, cost, reverse_cost FROM edges ORDER BY id', 6, 2);
-- → seq  depth  pred  node  edge
-- →   1      0     6     6    -1
-- →   2      1     6     5     1
-- →   3      1     6     7     4
-- →   4      2     7     3     7
-- →   5      2     7    11     8
-- →   6      2     7     8    10
```

Vertex 10 is missing at depth 1 because edge 2 (`6 → 10`) has `cost = -1`. Pass
`directed => false` and it appears immediately.

## duckrouting_depth_first_search

### Signatures

```sql
TABLE duckrouting_depth_first_search (edges_sql VARCHAR, root BIGINT)
TABLE duckrouting_depth_first_search (edges_sql VARCHAR, root BIGINT[])
TABLE duckrouting_depth_first_search (edges_sql VARCHAR, root BIGINT,   max_depth BIGINT)
TABLE duckrouting_depth_first_search (edges_sql VARCHAR, root BIGINT[], max_depth BIGINT)
-- also accepts max_depth => BIGINT and directed => BOOLEAN
```

### Description

The same walk, following each branch to its end before backtracking. Identical
column shape, and it reaches exactly the same vertices -- only the order
differs, and `depth` rises and falls as the search descends and returns.

### Example

```sql
SELECT seq, depth, pred, node, edge FROM duckrouting_depth_first_search(
  'SELECT id, source, target, cost, reverse_cost FROM edges ORDER BY id', 6);
-- → seq  depth  pred  node  edge
-- →   1      0     6     6    -1
-- →   2      1     6     5     1
-- →   3      1     6     7     4
-- →   4      2     7     3     7
-- →   5      3     3     1     6
-- →   6      2     7    11     8
-- →  ...
```

`seq` 6 dropping back to `depth` 2 is the backtrack: vertex 3's branch ended at
vertex 1, so the search returned to vertex 7.

## duckrouting_bellman_ford

### Signatures

```sql
TABLE duckrouting_bellman_ford (edges_sql VARCHAR, start_vid BIGINT|BIGINT[], end_vid BIGINT|BIGINT[])
TABLE duckrouting_bellman_ford (edges_sql VARCHAR, start_vid BIGINT|BIGINT[], end_vid BIGINT|BIGINT[], directed BOOLEAN)
-- also accepts directed => BOOLEAN
```

### Description

Shortest path via `boost::bellman_ford_shortest_paths`. Same column shape as
[`duckrouting_dijkstra`](/functions/dijkstra).

Bellman-Ford is slower than Dijkstra but tolerates **negative edge weights**,
which Dijkstra cannot. With non-negative weights the two agree exactly.

::: warning Negative costs mean something else here
pgRouting uses a negative `cost` to mean "this edge does not exist in that
direction", and duckrouting follows that convention throughout. So a negative
value in the edges query removes the edge rather than giving it a negative
weight, and Bellman-Ford's tolerance for negative weights is not reachable
through the standard edges contract.
:::

### Example

```sql
SELECT path_seq, node, edge, agg_cost FROM duckrouting_bellman_ford(
  'SELECT id, source, target, cost, reverse_cost FROM edges', 6, 10, true);
-- → path_seq  node  edge  agg_cost
-- →        1     6     4       0.0
-- →        2     7     8       1.0
-- →        3    11     9       2.0
-- →        4    16    16       3.0
-- →        5    15     3       4.0
-- →        6    10    -1       5.0
```

## duckrouting_dag_shortest_path

### Signatures

```sql
TABLE duckrouting_dag_shortest_path (edges_sql VARCHAR, start_vid BIGINT|BIGINT[], end_vid BIGINT|BIGINT[])
```

### Description

Shortest path over a **directed acyclic** graph, via
`boost::dag_shortest_paths`. It topologically sorts first and then relaxes in
that order, which is linear in the size of the graph -- faster than Dijkstra,
at the price of requiring a DAG.

There is no `directed` parameter: a DAG is directed by definition. A graph
containing a cycle raises an error rather than returning a wrong answer.

### Example

```sql
SELECT path_seq, node, edge, agg_cost FROM duckrouting_dag_shortest_path(
  'SELECT id, source, target, cost FROM edges', 5, 11);
-- → path_seq  node  edge  agg_cost
-- →        1     5     1       0.0
-- →        2     6     4       1.0
-- →        3     7     8       2.0
-- →        4    11    -1       3.0
```

## duckrouting_transitive_closure

### Signatures

```sql
TABLE duckrouting_transitive_closure (edges_sql VARCHAR)
```

### Description

Every vertex reachable from every vertex. Returns `node` and `targets`, a
`BIGINT[]` of everything reachable from it.

The array is sorted ascending. pgRouting returns Boost's internal order, so the
*sets* match but the element order is ours.

### Example

```sql
SELECT * FROM duckrouting_transitive_closure(
  'SELECT id, source, target, cost, reverse_cost FROM edges WHERE id IN (2,3,5,11,12,13,15)')
ORDER BY node;
-- → node  targets
-- →    6  []
-- →    8  [12, 16, 17]
-- →   10  [6, 11, 12, 16, 17]
-- →   11  [12, 16, 17]
-- →   12  [16, 17]
-- →   15  [6, 10, 11, 12, 16, 17]
```

## Vertex orderings

Four functions that return a permutation of the vertices as `seq` and `node`.
The first three are bandwidth-reduction orderings, used to make a graph's
adjacency matrix narrower around the diagonal.

| Function | Boost algorithm | Graph |
| --- | --- | --- |
| `duckrouting_cuthill_mckee_ordering` | `cuthill_mckee_ordering` | undirected |
| `duckrouting_king_ordering` | `king_ordering` | undirected |
| `duckrouting_sloan_ordering` | `sloan_ordering` | undirected |
| `duckrouting_topological_sort` | `topological_sort` | directed, acyclic |

### Signatures

```sql
TABLE duckrouting_cuthill_mckee_ordering (edges_sql VARCHAR)
TABLE duckrouting_king_ordering (edges_sql VARCHAR)
TABLE duckrouting_sloan_ordering (edges_sql VARCHAR)
TABLE duckrouting_topological_sort (edges_sql VARCHAR)
```

### Example

```sql
SELECT list(node ORDER BY seq) FROM duckrouting_cuthill_mckee_ordering(
  'SELECT id, source, target, cost, reverse_cost FROM edges');
-- → [13, 14, 2, 4, 5, 1, 6, 3, 10, 7, 9, 15, 11, 8, 16, 12, 17]
```

`duckrouting_topological_sort` orders a DAG so that every edge runs forwards.
It raises an error on a cyclic graph:

```sql
SELECT * FROM duckrouting_topological_sort(
  'SELECT id, source, target, cost, reverse_cost FROM edges');
-- → Invalid Input Error: the graph contains a cycle, so it is not a DAG
```

::: warning Orderings are not unique
Which of several equal-degree vertices comes first is implementation-defined,
and a topological order is not unique either. Boost emits Cuthill-McKee and King
*reversed*; pgRouting undoes that by writing into `rbegin()`, and duckrouting
does the same -- so both agree on the leading `13, 14, 2, 4` here while the
tails differ. Sloan starts from a pseudo-peripheral pair and covers only one
connected component, so it returns 13 of these 17 vertices.
:::
