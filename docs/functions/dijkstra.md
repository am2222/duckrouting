# Dijkstra family

Shortest-path routing over a weighted graph. Every function here runs
`boost::dijkstra_shortest_paths` and follows pgRouting's semantics for costs,
one-way edges and result columns.

All of them take an `edges_sql` string as their first argument. See
[The edges query](/guide/edges-query) for the columns it must return and how
costs are interpreted.

::: tip Vertex arguments
Wherever a vertex id is accepted you may pass either a single `BIGINT` or a
`BIGINT[]`. Lists are de-duplicated and sorted, and pairs whose start and end
coincide are dropped, so `[7,10,7]` behaves as `[7,10]`.
:::

## duckrouting_dijkstra

### Signatures

```sql
TABLE duckrouting_dijkstra (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT)
TABLE duckrouting_dijkstra (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT[])
TABLE duckrouting_dijkstra (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT)
TABLE duckrouting_dijkstra (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT[])
-- each also accepts a trailing directed BOOLEAN, or directed => BOOLEAN
```

### Description

Returns the shortest path for every combination of start and end vertex, one
row per node along each path. Unreachable pairs, and pairs whose endpoints
coincide, contribute no rows rather than raising an error.

Returns `seq`, `path_seq`, `start_vid`, `end_vid`, `node`, `edge`, `cost` and
`agg_cost`. `seq` numbers the whole result; `path_seq` restarts at 1 for each
path. `edge` is the edge used to leave `node`, and is `-1` on the last row of a
path, where `cost` is `0`. `agg_cost` is the cost accumulated *before* reaching
`node`, so the final row carries the total.

`directed` defaults to `true`.

### Example

```sql
SELECT * FROM duckrouting_dijkstra(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  6, 10);
-- → seq  path_seq  start_vid  end_vid  node  edge  cost  agg_cost
-- →   1         1          6       10     6     4   1.0       0.0
-- →   2         2          6       10     7     8   1.0       1.0
-- →   3         3          6       10    11     9   1.0       2.0
-- →   4         4          6       10    16    16   1.0       3.0
-- →   5         5          6       10    15     3   1.0       4.0
-- →   6         6          6       10    10    -1   0.0       5.0
```

The same query undirected takes edge 2 directly, because its `reverse_cost` is
traversable even though its `cost` is `-1`:

```sql
SELECT node, edge, agg_cost FROM duckrouting_dijkstra(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  6, 10, directed => false);
-- → node  edge  agg_cost
-- →    6     2       0.0
-- →   10    -1       1.0
```

Many-to-many, by passing lists:

```sql
SELECT DISTINCT start_vid, end_vid FROM duckrouting_dijkstra(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  [7, 10], [10, 15]);
-- → start_vid  end_vid
-- →         7       10
-- →         7       15
-- →        10       15
```

## duckrouting_dijkstra_cost

### Signatures

```sql
TABLE duckrouting_dijkstra_cost (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT)
TABLE duckrouting_dijkstra_cost (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT[])
TABLE duckrouting_dijkstra_cost (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT)
TABLE duckrouting_dijkstra_cost (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT[])
-- each also accepts a trailing directed BOOLEAN, or directed => BOOLEAN
```

### Description

The total cost of each shortest path, without the per-node detail. One row per
reachable pair, returning `start_vid`, `end_vid` and `agg_cost`.

Use this when you only need distances; it skips building the path itself.

### Example

```sql
SELECT * FROM duckrouting_dijkstra_cost(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  6, 10);
-- → start_vid  end_vid  agg_cost
-- →         6       10       5.0
```

## duckrouting_dijkstra_cost_matrix

### Signatures

```sql
TABLE duckrouting_dijkstra_cost_matrix (edges_sql VARCHAR, vids BIGINT[])
TABLE duckrouting_dijkstra_cost_matrix (edges_sql VARCHAR, vids BIGINT[], directed BOOLEAN)
```

### Description

Routes one list of vertices against itself, giving the cost of every ordered
pair. Identical to calling `duckrouting_dijkstra_cost` with the same list as
both arguments.

With `directed => false` the matrix is symmetric, which is what the
travelling-salesman style consumers expect.

### Example

```sql
SELECT * FROM duckrouting_dijkstra_cost_matrix(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  [5, 6, 10], false);
-- → start_vid  end_vid  agg_cost
-- →         5        6       1.0
-- →         5       10       2.0
-- →         6        5       1.0
-- →         6       10       1.0
-- →        10        5       2.0
-- →        10        6       1.0
```

## duckrouting_driving_distance

### Signatures

```sql
TABLE duckrouting_driving_distance (edges_sql VARCHAR, start_vid BIGINT,   distance DOUBLE)
TABLE duckrouting_driving_distance (edges_sql VARCHAR, start_vid BIGINT[], distance DOUBLE)
-- each also accepts a trailing directed BOOLEAN,
-- plus named directed => BOOLEAN and equicost => BOOLEAN
```

### Description

Every vertex reachable from `start_vid` within a total cost of `distance` --
the service area, or isochrone, around a point. Returns the shortest-path tree
as `seq`, `depth`, `start_vid`, `pred`, `node`, `edge`, `cost` and `agg_cost`.

`depth` is the hop count from the root, `pred` is the previous node and `edge`
is the edge joining them. The root appears at `depth` 0 as its own predecessor,
with `edge` `-1`. Rows are ordered by `start_vid`, then `depth`, then `node`.

With `equicost => true` and several start vertices, each node is reported only
for the start that reaches it most cheaply, so no node appears twice.

### Example

```sql
SELECT depth, pred, node, edge, agg_cost FROM duckrouting_driving_distance(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  11, 2.0);
-- → depth  pred  node  edge  agg_cost
-- →     0    11    11    -1       0.0
-- →     1    11     7     8       1.0
-- →     1    11    12    11       1.0
-- →     1    11    16     9       1.0
-- →     2     7     3     7       2.0
-- →     2     7     6     4       2.0
-- →     2     7     8    10       2.0
-- →     2    16    15    16       2.0
-- →     2    12    17    13       2.0
```

::: warning Equal-cost paths
When two paths tie on cost, which one is reported is not defined by Dijkstra,
and duckrouting may pick a different one than pgRouting. Above, node 17 is
reached at cost 2 via either vertex 12 or vertex 16. Do not assert on a
specific tie-break; assert on `agg_cost`.
:::

## Not yet implemented

These belong to the family and are Boost-backed, but are not built yet:
`pgr_dijkstraVia`, `pgr_dijkstraNear`, `pgr_dijkstraNearCost`.
`pgr_KSP` (Yen) is in the family too but is not a Boost algorithm -- see the
[pgRouting catalog](/pgrouting-function-catalog).
