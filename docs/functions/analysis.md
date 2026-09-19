# Graph analysis

Structural questions that are not about routing: colouring, planarity, cuts,
circuits and dominance.

## Colouring

### duckrouting_sequential_vertex_coloring

```sql
TABLE duckrouting_sequential_vertex_coloring (edges_sql VARCHAR)
```

Assigns each vertex a colour so that no edge joins two vertices of the same
colour. Returns `node` and `color`, with colours counted from 1.

```sql
SELECT node, color FROM duckrouting_sequential_vertex_coloring(
  'SELECT id, source, target, cost, reverse_cost FROM edges ORDER BY id')
WHERE node <= 6 ORDER BY node;
-- → node  color
-- →    1      1
-- →    2      1
-- →    3      2
-- →    4      2
-- →    5      1
-- →    6      2
```

### duckrouting_edge_coloring

```sql
TABLE duckrouting_edge_coloring (edges_sql VARCHAR)
```

Assigns each edge a colour so that edges meeting at a vertex differ. Returns
`edge` and `color`, counted from 1.

### duckrouting_bipartite

```sql
TABLE duckrouting_bipartite (edges_sql VARCHAR)
```

Splits the vertices into two sides such that every edge crosses between them.
Returns `node` and `color`, which is 0 or 1.

A graph that is not bipartite returns **no rows** rather than an error -- an odd
cycle makes the split impossible:

```sql
SELECT count(*) FROM duckrouting_bipartite(
  'SELECT * FROM (VALUES (1,1,2,1.0,1.0),(2,2,3,1.0,1.0),(3,3,1,1.0,1.0))
                 t(id,source,target,cost,reverse_cost)');
-- → 0
```

::: warning Colourings are not unique
Many valid colourings exist for most graphs. pgRouting and duckrouting agree on
every vertex colour in the sample graph, and on 17 of its 18 edge colours --
edge 1 gets colour 1 there and 3 here. Both are proper colourings. Check the
property (no clash across an edge), not a specific assignment.
:::

## Planarity

### duckrouting_is_planar

```sql
TABLE duckrouting_is_planar (edges_sql VARCHAR)
```

One row, one `BOOLEAN` column `is_planar`: whether the graph can be drawn with
no crossing edges.

```sql
SELECT * FROM duckrouting_is_planar(
  'SELECT id, source, target, cost, reverse_cost FROM edges');
-- → true
```

K5 -- five vertices all joined to each other -- is the smallest graph that
cannot:

```sql
SELECT * FROM duckrouting_is_planar(
  'SELECT * FROM (VALUES (1,1,2,1.0,1.0),(2,1,3,1.0,1.0),(3,1,4,1.0,1.0),(4,1,5,1.0,1.0),
                         (5,2,3,1.0,1.0),(6,2,4,1.0,1.0),(7,2,5,1.0,1.0),
                         (8,3,4,1.0,1.0),(9,3,5,1.0,1.0),(10,4,5,1.0,1.0))
                 t(id,source,target,cost,reverse_cost)');
-- → false
```

### duckrouting_boyer_myrvold

```sql
TABLE duckrouting_boyer_myrvold (edges_sql VARCHAR)
```

The planar embedding: for each vertex, its incident edges in rotation order.
Returns `seq`, `source` and `target`. Each edge therefore appears twice, once
from each endpoint. A non-planar graph returns no rows.

## Metrics

### duckrouting_bandwidth

```sql
TABLE duckrouting_bandwidth (edges_sql VARCHAR)
```

One row, one `BIGINT`: the largest gap between the indices of two adjacent
vertices. Reducing it is what the [ordering functions](/functions/traversal)
are for.

```sql
SELECT * FROM duckrouting_bandwidth(
  'SELECT id, source, target, cost, reverse_cost FROM edges');
-- → 5
```

### duckrouting_betweenness_centrality

```sql
TABLE duckrouting_betweenness_centrality (edges_sql VARCHAR)
TABLE duckrouting_betweenness_centrality (edges_sql VARCHAR, directed BOOLEAN)
-- also accepts directed => BOOLEAN
```

How often each vertex lies on a shortest path between two others, normalised to
0–1. Returns `vid` and `centrality`. High centrality means a bottleneck.

```sql
SELECT * FROM duckrouting_betweenness_centrality(
  'SELECT id, source, target, cost, reverse_cost FROM edges WHERE id < 5')
ORDER BY vid;
-- → vid  centrality
-- →   5        0.0
-- →   6        0.5
-- →   7        0.0
-- →  10       0.25
-- →  15        0.0
```

::: tip Normalisation
Directed results are scaled by `1/((n-1)(n-2))` and undirected by
`2/((n-1)(n-2))`, because a directed graph has twice as many ordered pairs.
Boost's own `relative_betweenness_centrality` always applies the undirected
factor, so passing `directed => true` through it would double every score.
:::

## Cuts and circuits

### duckrouting_stoer_wagner

```sql
TABLE duckrouting_stoer_wagner (edges_sql VARCHAR)
```

The cheapest set of edges whose removal disconnects the graph. Returns `seq`,
`edge`, `cost` and `mincut`, one row per edge crossing the cut; `mincut` is the
total and repeats on every row.

```sql
SELECT * FROM duckrouting_stoer_wagner(
  'SELECT id, source, target, cost, reverse_cost FROM edges WHERE id < 17');
-- → seq  edge  cost  mincut
-- →   1    14   1.0     1.0
```

::: warning One edge, one weight
A minimum cut sums edge weights, so `cost` and `reverse_cost` are collapsed to a
single edge of the cheaper weight. Routing functions deliberately keep both as
parallel edges; counting both here would double any cut that crosses such an
edge.
:::

### duckrouting_hawick_circuits

```sql
TABLE duckrouting_hawick_circuits (edges_sql VARCHAR)
```

Every circuit -- closed path -- in the directed graph. Returns `seq`,
`path_id`, `path_seq`, `start_vid`, `end_vid`, `node`, `edge`, `cost` and
`agg_cost`.

`path_seq` counts from **0**, not 1, matching pgRouting. Each circuit ends by
returning to its start vertex with `edge` `-1`.

```sql
SELECT path_id, path_seq, node, edge, agg_cost
FROM duckrouting_hawick_circuits(
  'SELECT id, source, target, cost, reverse_cost FROM edges')
WHERE path_id = 1;
-- → path_id  path_seq  node  edge  agg_cost
-- →       1         0     5     1       0.0
-- →       1         1     6     1       1.0
-- →       1         2     5    -1       2.0
```

::: warning This can explode
The number of circuits grows very quickly with graph density. The 18-edge
sample graph already has 20 of them.
:::

## duckrouting_dominator_tree

```sql
TABLE duckrouting_dominator_tree (edges_sql VARCHAR, root BIGINT)
```

For each vertex, the immediate dominator relative to `root`: the last vertex
every path from the root must pass through before reaching it. Returns
`vertex_id` and `idom`, where `idom` is 0 for the root itself and for anything
the root cannot reach.

```sql
SELECT vertex_id, idom FROM duckrouting_dominator_tree(
  'SELECT id, source, target, cost, reverse_cost FROM edges', 5)
WHERE vertex_id IN (1, 3, 5) ORDER BY vertex_id;
-- → vertex_id  idom
-- →         1     3
-- →         3     7
-- →         5     0
```

Reading that: from vertex 5, every route to vertex 1 goes through vertex 3, and
every route to vertex 3 goes through vertex 7.

::: warning Not checked against pgRouting
pgRouting's published example for this function reports values we could not
reconcile with the sample graph -- it shows vertex 3 dominating itself. These
results are verified against the graph directly instead.
:::
