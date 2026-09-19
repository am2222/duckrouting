# A*

Shortest paths, guided by knowing roughly where the destination is.

A\* is Dijkstra with a hint: at each step it prefers vertices that look closer
to the goal. Given a sensible hint it explores far less of the graph, and it
returns **the same answer** — the heuristic changes the work done, not the
result.

::: warning A different edges query
These functions need coordinates on top of the usual columns:

| Column | Type | Meaning |
| --- | --- | --- |
| `id`, `source`, `target`, `cost` | | as everywhere else |
| `reverse_cost` | numeric | optional, as everywhere else |
| `x1`, `y1` | numeric | **required** — where `source` is |
| `x2`, `y2` | numeric | **required** — where `target` is |

Without coordinates there is nothing to estimate from, so a missing `x1` is a
binder error rather than a silent fallback to Dijkstra.
:::

## Heuristics

`heuristic` picks how the remaining distance is estimated, using `dx` and `dy`
between a vertex and the goal. These are pgRouting's numbers and formulas.

| `heuristic` | Estimate | Notes |
| --- | --- | --- |
| 0 | `0` | no hint at all — identical to Dijkstra |
| 1 | `abs(max(dx, dy)) * factor` | |
| 2 | `abs(min(dx, dy)) * factor` | |
| 3 | `(dx² + dy²) * factor²` | squared Euclidean |
| 4 | `sqrt(dx² + dy²) * factor` | straight-line distance |
| 5 | `(abs(dx) + abs(dy)) * factor` | Manhattan — **the default** |

`factor` scales the estimate into the same units as `cost`: if your costs are
travel times in seconds and your coordinates are metres, `factor` converts
between them. `epsilon` multiplies `factor`, and must be at least 1.

::: tip Pick a heuristic that cannot overshoot
A\* only returns a true shortest path while the estimate never *exceeds* the
real remaining cost. Overshooting — usually from a `factor` that is too large —
makes it faster and wrong. When in doubt, `heuristic => 0` is exactly Dijkstra.
:::

## duckrouting_astar

### Signatures

```sql
TABLE duckrouting_astar (edges_sql VARCHAR, start_vid BIGINT|BIGINT[], end_vid BIGINT|BIGINT[])
TABLE duckrouting_astar (edges_sql VARCHAR, start_vid BIGINT|BIGINT[], end_vid BIGINT|BIGINT[], directed BOOLEAN)
-- also accepts directed =>, heuristic =>, factor => and epsilon =>
```

### Description

Same columns as [`duckrouting_dijkstra`](/functions/dijkstra): `seq`,
`path_seq`, `start_vid`, `end_vid`, `node`, `edge`, `cost`, `agg_cost`.

### Example

```sql
SELECT path_seq, node, edge, agg_cost FROM duckrouting_astar(
  'SELECT id, source, target, cost, reverse_cost, x1, y1, x2, y2 FROM edges',
  6, 10, heuristic => 3, factor => 3.5);
-- → path_seq  node  edge  agg_cost
-- →        1     6     4       0.0
-- →        2     7     8       1.0
-- →        3    11     9       2.0
-- →        4    16    16       3.0
-- →        5    15     3       4.0
-- →        6    10    -1       5.0
```

Cost 5, exactly what `duckrouting_dijkstra` returns for the same pair.

## duckrouting_astar_cost

### Signatures

```sql
TABLE duckrouting_astar_cost (edges_sql VARCHAR, start_vid BIGINT|BIGINT[], end_vid BIGINT|BIGINT[])
TABLE duckrouting_astar_cost (edges_sql VARCHAR, start_vid BIGINT|BIGINT[], end_vid BIGINT|BIGINT[], directed BOOLEAN)
-- also accepts directed =>, heuristic =>, factor => and epsilon =>
```

### Description

Total cost per reachable pair: `start_vid`, `end_vid`, `agg_cost`.

### Example

```sql
SELECT * FROM duckrouting_astar_cost(
  'SELECT id, source, target, cost, reverse_cost, x1, y1, x2, y2 FROM edges',
  6, [10, 12]);
-- → start_vid  end_vid  agg_cost
-- →         6       10       5.0
-- →         6       12       3.0
```

## duckrouting_astar_cost_matrix

### Signatures

```sql
TABLE duckrouting_astar_cost_matrix (edges_sql VARCHAR, vids BIGINT[])
TABLE duckrouting_astar_cost_matrix (edges_sql VARCHAR, vids BIGINT[], directed BOOLEAN)
-- also accepts directed =>, heuristic =>, factor => and epsilon =>
```

### Description

One vertex list routed against itself, giving every ordered pair. Identical to
calling `duckrouting_astar_cost` with the same list twice.

::: warning Equal-cost paths
As with Dijkstra, ties are broken arbitrarily. `6 -> 12` costs 3 either via
vertex 8 (pgRouting's answer) or via vertex 11 (Boost's). Assert on `agg_cost`,
not on a specific route.
:::
