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

## duckrouting_dijkstra_via

### Signatures

```sql
TABLE duckrouting_dijkstra_via (edges_sql VARCHAR, via_vids BIGINT[])
TABLE duckrouting_dijkstra_via (edges_sql VARCHAR, via_vids BIGINT[], directed BOOLEAN)
-- also accepts directed => BOOLEAN
```

### Description

Routes through a sequence of vertices in order, one leg per consecutive pair.
Returns `seq`, `path_id`, `path_seq`, `start_vid`, `end_vid`, `node`, `edge`,
`cost`, `agg_cost` and `route_agg_cost`.

`path_id` numbers the legs from 1. `agg_cost` restarts on each leg, while
`route_agg_cost` accumulates across the whole route. The last row of every leg
carries `edge = -1`, except the last leg of the route, which carries `-2` --
that is how pgRouting marks the end of the journey rather than the end of a leg.

A leg with no path contributes no rows and the route continues from the next
via vertex.

### Example

```sql
SELECT path_id, node, edge, agg_cost, route_agg_cost
FROM duckrouting_dijkstra_via(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  [5, 1, 8]);
-- → path_id  node  edge  agg_cost  route_agg_cost
-- →       1     5     1       0.0             0.0
-- →       1     6     4       1.0             1.0
-- →       1     7     7       2.0             2.0
-- →       1     3     6       3.0             3.0
-- →       1     1    -1       4.0             4.0
-- →       2     1     6       0.0             4.0
-- →       2     3     7       1.0             5.0
-- →       2     7    10       2.0             6.0
-- →       2     8    -2       3.0             7.0
```

The total for the whole route is the `route_agg_cost` on the `-2` row:

```sql
SELECT route_agg_cost FROM duckrouting_dijkstra_via(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  [5, 7, 1, 8, 15])
WHERE edge = -2;
-- → 11.0
```

## duckrouting_dijkstra_near

### Signatures

```sql
TABLE duckrouting_dijkstra_near (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT)
TABLE duckrouting_dijkstra_near (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT[])
TABLE duckrouting_dijkstra_near (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT)
TABLE duckrouting_dijkstra_near (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT[])
-- each also accepts a trailing directed BOOLEAN,
-- plus named directed => BOOLEAN and cap => BIGINT
```

### Description

Of all the (start, end) combinations, returns only the `cap` cheapest, as full
paths. `cap` defaults to 1, so by default you get the single nearest pair.

Column shape is identical to `duckrouting_dijkstra`. Use this to answer "which
of these depots is closest, and how do I get there" in one query instead of
routing to every candidate and sorting afterwards.

### Example

Of vertices 10, 11 and 1, which is nearest to 6?

```sql
SELECT start_vid, end_vid, node, edge, agg_cost
FROM duckrouting_dijkstra_near(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  6, [10, 11, 1]);
-- → start_vid  end_vid  node  edge  agg_cost
-- →         6       11     6     4       0.0
-- →         6       11     7     8       1.0
-- →         6       11    11    -1       2.0
```

`cap` takes the two cheapest pairs across every start:

```sql
SELECT start_vid, end_vid, node, edge, agg_cost
FROM duckrouting_dijkstra_near(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  [10, 11, 1], 6, cap => 2);
-- → start_vid  end_vid  node  edge  agg_cost
-- →        10        6    10     2       0.0
-- →        10        6     6    -1       1.0
-- →        11        6    11     8       0.0
-- →        11        6     7     4       1.0
-- →        11        6     6    -1       2.0
```

## duckrouting_dijkstra_near_cost

### Signatures

```sql
TABLE duckrouting_dijkstra_near_cost (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT)
TABLE duckrouting_dijkstra_near_cost (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT[])
TABLE duckrouting_dijkstra_near_cost (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT)
TABLE duckrouting_dijkstra_near_cost (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT[])
-- each also accepts a trailing directed BOOLEAN,
-- plus named directed => BOOLEAN and cap => BIGINT
```

### Description

The same selection as `duckrouting_dijkstra_near`, returning only
`start_vid`, `end_vid` and `agg_cost`.

### Example

```sql
SELECT * FROM duckrouting_dijkstra_near_cost(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  [10, 11, 1], 6, cap => 2);
-- → start_vid  end_vid  agg_cost
-- →        10        6       1.0
-- →        11        6       2.0
```

## See also

[`duckrouting_ksp`](/functions/ksp) returns the *k* shortest paths for a pair
rather than only the best one. It is grouped with this family by pgRouting but
is not a Boost algorithm.
