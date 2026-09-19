# All pairs

Shortest-path costs between every pair of vertices at once. Both functions
return `start_vid`, `end_vid` and `agg_cost`, omitting self pairs and
unreachable pairs rather than reporting them as zero or NULL.

::: tip These two agree
`floydWarshall` and `johnson` solve the same problem by different routes, so on
any graph they return identical results. Johnson is the better choice on sparse
graphs; Floyd-Warshall on dense ones.
:::

## duckrouting_floyd_warshall

### Signatures

```sql
TABLE duckrouting_floyd_warshall (edges_sql VARCHAR)
TABLE duckrouting_floyd_warshall (edges_sql VARCHAR, directed BOOLEAN)
-- also accepts directed => BOOLEAN
```

### Description

All-pairs shortest path costs via `boost::floyd_warshall_all_pairs_shortest_paths`.
Runs in O(V³) regardless of how sparse the graph is.

Because no edge is ever reported back, the `id` column of the edges query is
optional here.

### Example

```sql
SELECT * FROM duckrouting_floyd_warshall(
  'SELECT id, source, target, cost, reverse_cost FROM edges WHERE id < 5')
ORDER BY start_vid, end_vid;
-- → start_vid  end_vid  agg_cost
-- →         5        6       1.0
-- →         5        7       2.0
-- →         6        5       1.0
-- →         6        7       1.0
-- →         7        5       2.0
-- →         7        6       1.0
-- →        10        5       2.0
-- →        10        6       1.0
-- →        10        7       2.0
-- →        15        5       3.0
-- →        15        6       2.0
-- →        15        7       3.0
-- →        15       10       1.0
```

## duckrouting_johnson

### Signatures

```sql
TABLE duckrouting_johnson (edges_sql VARCHAR)
TABLE duckrouting_johnson (edges_sql VARCHAR, directed BOOLEAN)
-- also accepts directed => BOOLEAN
```

### Description

All-pairs shortest path costs via `boost::johnson_all_pairs_shortest_paths`,
which reweights the graph and then runs Dijkstra from every vertex. Faster than
Floyd-Warshall on sparse graphs.

As with `floyd_warshall`, the `id` column is optional -- pgRouting documents
this one with a bare `SELECT source, target, cost`.

### Example

```sql
SELECT * FROM duckrouting_johnson(
  'SELECT source, target, cost FROM edges WHERE id < 5')
ORDER BY start_vid, end_vid;
-- → start_vid  end_vid  agg_cost
-- →         5        6       1.0
-- →         5        7       2.0
-- →         6        7       1.0
```

Without `reverse_cost` every edge is one-way, which is why this returns three
rows where the `floyd_warshall` example above returns thirteen.
