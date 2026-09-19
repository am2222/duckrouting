# Contraction and points

Two functions that change the graph itself before routing over it.

## duckrouting_contraction_hierarchies

### Signatures

```sql
TABLE duckrouting_contraction_hierarchies (edges_sql VARCHAR)
TABLE duckrouting_contraction_hierarchies (edges_sql VARCHAR, directed BOOLEAN)
-- also accepts directed => BOOLEAN and forbidden => BIGINT[]
```

### Description

Preprocesses a graph for fast routing. Vertices are removed one at a time, and
wherever removing one would lengthen a shortest path, a **shortcut** edge is
added in its place. The result preserves every shortest-path distance while
being much cheaper to search.

This function returns the hierarchy, not a route. Rows come in two kinds,
distinguished by `type`:

| `type` | Meaning | Columns used |
| --- | --- | --- |
| `v` | a vertex | `id`, `metric`, `vertex_order` |
| `e` | a shortcut | `id` (negative), `contracted_vertices`, `source`, `target`, `cost` |

`vertex_order` is the position at which a vertex was contracted, and `metric`
is its edge difference — shortcuts added minus edges removed — at that moment.
`contracted_vertices` lists which vertices a shortcut stands in for, so it can
be unpacked back into real edges.

`forbidden` keeps vertices out of the hierarchy entirely: they are never
contracted and never appear inside a shortcut. Use it to protect vertices you
intend to route from.

### Example

```sql
SELECT id, contracted_vertices, source, target, cost
FROM duckrouting_contraction_hierarchies(
  'SELECT id, source, target, cost FROM edges', false)
WHERE type = 'e';
-- → id  contracted_vertices  source  target  cost
-- → -1  [12]                      8      17   2.0
-- → -2  [11]                      7      16   2.0
-- → -3  [11, 7]                  16       8   3.0
```

Shortcut `-3` replaces the two-hop route from 16 to 8 through vertices 11 and 7,
at the same total cost of 3.

```sql
SELECT id, vertex_order FROM duckrouting_contraction_hierarchies(
  'SELECT id, source, target, cost FROM edges', false, forbidden => [7, 8])
WHERE type = 'v' AND id IN (7, 8);
-- → id  vertex_order
-- →  7            -1
-- →  8            -1
```

::: warning The hierarchy is not unique
Which shortcuts appear depends entirely on the order vertices are contracted
in, and pgRouting does not document its priority function. duckrouting
contracts greedily by edge difference; on the sample graph pgRouting produces
four shortcuts and duckrouting three. Both are valid hierarchies preserving the
same distances. Treat `metric` and `vertex_order` as implementation-defined,
and check shortcuts by their cost rather than their identity.
:::

## duckrouting_with_points_dd

### Signatures

```sql
TABLE duckrouting_with_points_dd (edges_sql VARCHAR, points_sql VARCHAR, start_vid BIGINT|BIGINT[], distance DOUBLE)
TABLE duckrouting_with_points_dd (edges_sql VARCHAR, points_sql VARCHAR, start_vid BIGINT|BIGINT[], distance DOUBLE, driving_side VARCHAR)
-- also accepts driving_side =>, directed => and details =>
```

### Description

[`duckrouting_driving_distance`](/functions/dijkstra), but you can start from —
or pass through — a point that sits **partway along an edge** rather than at a
vertex. Think of a house halfway down a street.

`points_sql` must expose:

| Column | Type | Meaning |
| --- | --- | --- |
| `pid` | `BIGINT` | point identifier |
| `edge_id` | `BIGINT` | which edge it lies on |
| `fraction` | `DOUBLE` | how far along, between 0 and 1 |
| `side` | `VARCHAR` | optional — `'r'`, `'l'`, or `'b'` for both |

Each point becomes a vertex numbered **`-pid`**, splitting its edge into
segments whose costs are proportional to the fractions. Returns the
`driving_distance` shape: `seq`, `depth`, `start_vid`, `pred`, `node`, `edge`,
`cost`, `agg_cost`.

`details` controls whether *other* points appear in the result; the starting
point always does.

### Which side of the road

`driving_side` says which side traffic drives on, and it interacts with each
point's own `side`:

- On a **one-way** street, or when either side is `'b'`, the point is reachable
  from both directions.
- On a **two-way** street with both sides definite, the point is only reachable
  from the direction that passes it on the driving side.

That second rule is what makes the example below take the long way round.

### Example

Point 1 sits 40% along edge 1 — which runs north from vertex 5 to vertex 6 — on
the left. Driving on the right, you cannot reach it travelling north, so
vertex 6 is reached by going back to vertex 5 first:

```sql
SELECT depth, pred, node, edge, cost, agg_cost FROM duckrouting_with_points_dd(
  'SELECT id, source, target, cost, reverse_cost FROM edges ORDER BY id',
  'SELECT pid, edge_id, fraction, side FROM poi',
  -1, 3.3, 'r', details => true);
-- → depth  pred  node  edge  cost  agg_cost
-- →     0    -1    -1    -1   0.0       0.0
-- →     1    -1     5     1   0.4       0.4
-- →     2     5     6     1   1.0       1.4
-- →     3     6    -6     4   0.7       2.1
-- →     4    -6     7     4   0.3       2.4
```

With `driving_side => 'b'` the side stops mattering and vertex 6 is reached
directly, at the remaining 0.6 of edge 1:

```sql
SELECT depth, node, cost, agg_cost FROM duckrouting_with_points_dd(
  'SELECT id, source, target, cost, reverse_cost FROM edges ORDER BY id',
  'SELECT pid, edge_id, fraction, side FROM poi',
  -1, 1.0, 'b', details => true);
-- → depth  node  cost  agg_cost
-- →     0    -1   0.0       0.0
-- →     1     5   0.4       0.4
-- →     1     6   0.6       0.6
```

0.4 one way and 0.6 the other — the two halves of edge 1, which costs 1.
