# K shortest paths

Alternative routes between the same pair of vertices, not just the best one.

::: info Not a Boost algorithm
Every other routing function here delegates to the Boost Graph Library, but
Boost has no k-shortest-paths routine. `duckrouting_ksp` implements
[Yen's algorithm](https://en.wikipedia.org/wiki/Yen%27s_algorithm) (1971)
directly, on top of repeated constrained shortest-path solves. pgRouting's
`pgr_KSP` is likewise its own code rather than a Boost wrapper -- see the
[pgRouting catalog](/pgrouting-function-catalog).
:::

## duckrouting_ksp

### Signatures

```sql
TABLE duckrouting_ksp (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT,   k BIGINT)
TABLE duckrouting_ksp (edges_sql VARCHAR, start_vid BIGINT,   end_vid BIGINT[], k BIGINT)
TABLE duckrouting_ksp (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT,   k BIGINT)
TABLE duckrouting_ksp (edges_sql VARCHAR, start_vid BIGINT[], end_vid BIGINT[], k BIGINT)
-- each also accepts a trailing directed BOOLEAN,
-- plus named directed => BOOLEAN and heap_paths => BOOLEAN
```

### Description

Returns up to `k` shortest **loopless** paths for every (start, end)
combination, cheapest first. Returns `seq`, `path_id`, `path_seq`,
`start_vid`, `end_vid`, `node`, `edge`, `cost` and `agg_cost`.

`path_id` runs across the whole result rather than restarting for each pair, so
with two pairs and `k = 2` you get `path_id` 1 through 4. Within a pair, costs
are non-decreasing in `path_id`.

If fewer than `k` distinct paths exist, fewer are returned; this is not an
error. `k` must be at least 1.

With `heap_paths => true`, the candidate paths that Yen's algorithm generated
but did not select are returned as well, so the result may exceed `k`.

### Example

Two ways to get from 6 to 17, both costing 4:

```sql
SELECT path_id, path_seq, node, edge, agg_cost
FROM duckrouting_ksp(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  6, 17, 2);
-- → path_id  path_seq  node  edge  agg_cost
-- →       1         1     6     4       0.0
-- →       1         2     7     8       1.0
-- →       1         3    11     9       2.0
-- →       1         4    16    15       3.0
-- →       1         5    17    -1       4.0
-- →       2         1     6     4       0.0
-- →       2         2     7    10       1.0
-- →       2         3     8    12       2.0
-- →       2         4    12    13       3.0
-- →       2         5    17    -1       4.0
```

Collapsing each path to its node sequence is often what you actually want:

```sql
SELECT path_id,
       list_aggregate(list(node ORDER BY path_seq), 'string_agg', ',') AS route,
       max(agg_cost) AS cost
FROM duckrouting_ksp(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  6, 17, 2)
GROUP BY path_id ORDER BY path_id;
-- → path_id  route             cost
-- →       1  6,7,11,16,17       4.0
-- →       2  6,7,8,12,17        4.0
```

::: warning Equal-cost paths
When several paths tie on cost, their order is not defined. Above, both routes
cost 4, and pgRouting happens to report them in the opposite order. Treat
`path_id` as a grouping key, not as a stable ranking among equal costs.
:::

`k = 1` is exactly `duckrouting_dijkstra`:

```sql
SELECT count(*) FROM (
  SELECT path_seq, node, edge, agg_cost
  FROM duckrouting_ksp('SELECT id, source, target, cost, reverse_cost FROM edges', 6, 17, 1)
  EXCEPT
  SELECT path_seq, node, edge, agg_cost
  FROM duckrouting_dijkstra('SELECT id, source, target, cost, reverse_cost FROM edges', 6, 17)
);
-- → 0
```
