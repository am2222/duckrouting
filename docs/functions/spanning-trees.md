# Spanning trees

The cheapest set of edges that keeps the graph connected, and ways of walking
the result.

Every function here treats the graph as undirected -- a spanning tree has no
notion of direction -- and produces a spanning *forest* when the graph is
disconnected, one tree per component.

::: warning Spanning trees are not unique
When several edges tie on cost, more than one minimum spanning tree exists, and
which one you get depends on the algorithm. pgRouting's own `pgr_kruskal` and
`pgr_prim` return different edge sets from each other on the same graph. What is
determined is the *number* of edges and the *total* weight, not the selection.
Assert on those, not on a specific edge list.
:::

## duckrouting_kruskal

### Signatures

```sql
TABLE duckrouting_kruskal (edges_sql VARCHAR)
```

### Description

The minimum spanning forest, via `boost::kruskal_minimum_spanning_tree`.
Returns `edge` and `cost`, one row per chosen edge.

A forest over *V* vertices in *C* components always has exactly *V - C* edges.

### Example

```sql
SELECT count(*) AS edges, sum(cost) AS total
FROM duckrouting_kruskal(
  'SELECT id, source, target, cost, reverse_cost FROM edges');
-- → edges  total
-- →    14   14.0
```

17 vertices in 3 components gives 14 edges.

## duckrouting_prim

### Signatures

```sql
TABLE duckrouting_prim (edges_sql VARCHAR)
```

### Description

The same minimum spanning forest, via `boost::prim_minimum_spanning_tree`.
Prim grows outwards from a root, so it is run once per connected component to
cover the whole forest.

Prim and Kruskal may choose different edges, but must agree on edge count and
total weight.

### Example

```sql
SELECT count(*) AS edges, sum(cost) AS total
FROM duckrouting_prim(
  'SELECT id, source, target, cost, reverse_cost FROM edges WHERE id < 14');
-- → edges  total
-- →    11   11.0
```

## Traversals

Six functions walk a spanning forest from a root. They share pgRouting's
`drivingDistance` column shape: `seq`, `depth`, `start_vid`, `pred`, `node`,
`edge`, `cost`, `agg_cost`.

The root appears at `depth` 0 as its own predecessor with `edge` `-1`. A root
that is not in the graph still yields that single row rather than an error.

| Function | Tree | Walk | Cut-off |
| --- | --- | --- | --- |
| `duckrouting_kruskal_bfs` | Kruskal | breadth first | `max_depth` |
| `duckrouting_kruskal_dfs` | Kruskal | depth first | `max_depth` |
| `duckrouting_kruskal_dd` | Kruskal | depth first | `distance` |
| `duckrouting_prim_bfs` | Prim | breadth first | `max_depth` |
| `duckrouting_prim_dfs` | Prim | depth first | `max_depth` |
| `duckrouting_prim_dd` | Prim | depth first | `distance` |

### Signatures

```sql
-- BFS and DFS variants
TABLE duckrouting_kruskal_bfs (edges_sql VARCHAR, root BIGINT)
TABLE duckrouting_kruskal_bfs (edges_sql VARCHAR, root BIGINT[])
TABLE duckrouting_kruskal_bfs (edges_sql VARCHAR, root BIGINT,   max_depth BIGINT)
TABLE duckrouting_kruskal_bfs (edges_sql VARCHAR, root BIGINT[], max_depth BIGINT)
-- also accepts max_depth => BIGINT

-- DD variants take a cost radius instead
TABLE duckrouting_prim_dd (edges_sql VARCHAR, root BIGINT,   distance DOUBLE)
TABLE duckrouting_prim_dd (edges_sql VARCHAR, root BIGINT[], distance DOUBLE)
```

### Breadth first vs depth first

The difference shows up in `depth`. Breadth first never goes back up:

```sql
SELECT depth, pred, node, edge FROM duckrouting_kruskal_bfs(
  'SELECT id, source, target, cost, reverse_cost FROM edges ORDER BY id', 6, 2);
-- → depth  pred  node  edge
-- →     0     6     6    -1
-- →     1     6     5     1
-- →     1     6     7     4
-- →     2     7     3     7
-- →     2     7     8    10
```

Depth first descends and backtracks, so `depth` rises and falls:

```sql
SELECT seq, depth, pred, node, edge, agg_cost FROM duckrouting_prim_dd(
  'SELECT id, source, target, cost, reverse_cost FROM edges ORDER BY id', 6, 3.5);
-- → seq  depth  pred  node  edge  agg_cost
-- →   1      0     6     6    -1       0.0
-- →   2      1     6     5     1       1.0
-- →   3      1     6    10     2       1.0
-- →   4      2    10    15     3       2.0
-- →   5      2    10    11     5       2.0
-- →   6      3    11    16     9       3.0
-- →   7      3    11    12    11       3.0
-- →   8      1     6     7     4       1.0
-- →   9      2     7     3     7       2.0
-- →  10      3     3     1     6       3.0
-- →  11      2     7     8    10       2.0
-- →  12      3     8     9    14       3.0
```

Notice `seq` 8 dropping back to `depth` 1: the walk has finished vertex 10's
subtree and returned to the root to start on vertex 7.

### Several roots

Pass a list to walk more than one tree in a single call. Each root is walked
separately and reported under its own `start_vid`:

```sql
SELECT start_vid, count(*) AS nodes FROM duckrouting_prim_bfs(
  'SELECT id, source, target, cost, reverse_cost FROM edges ORDER BY id', [6, 13])
GROUP BY start_vid ORDER BY start_vid;
-- → start_vid  nodes
-- →         6     13
-- →        13      2
```

A traversal only reaches the root's own component, which is why vertex 13
returns two nodes and vertex 6 returns thirteen.
