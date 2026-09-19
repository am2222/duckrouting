# Travelling salesman

The shortest tour visiting every vertex once and returning to the start.

::: warning These are approximations
Both functions use Boost's `metric_tsp_approx`, which guarantees a tour **at
most twice the optimum** when the costs obey the triangle inequality — not the
optimum itself. Which tour it finds depends on spanning-tree tie-breaking, so
treat the stops as one good answer rather than the answer.
:::

::: tip cost means something different here
In every other family `cost` is the cost of *leaving* a node. In a tour it is
the cost of **arriving**, so the first row is always 0 and the last closes the
loop back to the start. That is pgRouting's convention too.
:::

## duckrouting_tsp

### Signatures

```sql
TABLE duckrouting_tsp (matrix_sql VARCHAR)
TABLE duckrouting_tsp (matrix_sql VARCHAR, start_id BIGINT)
TABLE duckrouting_tsp (matrix_sql VARCHAR, start_id BIGINT, end_id BIGINT)
-- also accepts start_id => and end_id =>
```

### Description

Takes a **cost matrix**, not an edges query: `matrix_sql` must expose
`start_vid`, `end_vid` and `agg_cost`. That is exactly the shape
[`duckrouting_dijkstra_cost_matrix`](/functions/dijkstra) produces, which is the
intended way to feed it.

Returns `seq`, `node`, `cost` and `agg_cost`. `start_id` chooses where the tour
begins and `end_id` which stop comes last before closing; a cycle can be entered
anywhere and walked in either direction, so neither changes the total.

An asymmetric matrix is made symmetric by taking the cheaper of the two
directions, because the algorithm needs a metric.

### Example

Because table-function arguments must be constant-foldable, materialise the
matrix first rather than nesting the call:

```sql
CREATE TABLE matrix AS SELECT * FROM duckrouting_dijkstra_cost_matrix(
  'SELECT id, source, target, cost, reverse_cost FROM edges', [5,6,10,15], false);

SELECT * FROM duckrouting_tsp('SELECT start_vid, end_vid, agg_cost FROM matrix');
-- → seq  node  cost  agg_cost
-- →   1     5   0.0       0.0
-- →   2     6   1.0       1.0
-- →   3    10   1.0       2.0
-- →   4    15   1.0       3.0
-- →   5     5   3.0       6.0
```

Four vertices, five rows: each once, plus the return to vertex 5.

```sql
SELECT * FROM duckrouting_tsp('SELECT start_vid, end_vid, agg_cost FROM matrix', 10);
-- → seq  node  cost  agg_cost
-- →   1    10   0.0       0.0
-- →   2     6   1.0       1.0
-- →   3     5   1.0       2.0
-- →   4    15   3.0       5.0
-- →   5    10   1.0       6.0
```

Same tour, entered at vertex 10, same total of 6.

## duckrouting_tsp_euclidean

### Signatures

```sql
TABLE duckrouting_tsp_euclidean (points_sql VARCHAR)
TABLE duckrouting_tsp_euclidean (points_sql VARCHAR, start_id BIGINT)
TABLE duckrouting_tsp_euclidean (points_sql VARCHAR, start_id BIGINT, end_id BIGINT)
-- also accepts start_id => and end_id =>
```

### Description

The same tour, but distances are computed from coordinates instead of being
supplied. `points_sql` must expose `id`, `x` and `y`. No graph is involved:
every point is reachable from every other in a straight line.

Use this when you want a tour over locations rather than over a road network.

### Example

The four corners of a unit square, whose optimal tour is its perimeter:

```sql
SELECT * FROM duckrouting_tsp_euclidean(
  'SELECT * FROM (VALUES (1,0.0,0.0),(2,1.0,0.0),(3,1.0,1.0),(4,0.0,1.0)) t(id,x,y)');
-- → seq  node  cost  agg_cost
-- →   1     1   0.0       0.0
-- →   2     2   1.0       1.0
-- →   3     3   1.0       2.0
-- →   4     4   1.0       3.0
-- →   5     1   1.0       4.0
```
