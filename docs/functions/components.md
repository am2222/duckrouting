# Components

Structural questions about the graph: which parts hang together, and which
single vertex or edge holds them together.

Apart from `strong_components`, which needs edge direction, these all treat the
graph as undirected -- connectivity is a symmetric notion.

## duckrouting_connected_components

### Signatures

```sql
TABLE duckrouting_connected_components (edges_sql VARCHAR)
```

### Description

Splits the undirected graph into connected components. Returns `seq`,
`component` and `node`, ordered by component then node.

`component` is the smallest node id in the component, not an arbitrary index,
so the label is stable across runs and meaningful to read.

### Example

```sql
SELECT component, count(*) AS members
FROM duckrouting_connected_components(
  'SELECT id, source, target, cost, reverse_cost FROM edges')
GROUP BY component ORDER BY component;
-- → component  members
-- →         1       13
-- →         2        2
-- →        13        2
```

## duckrouting_strong_components

### Signatures

```sql
TABLE duckrouting_strong_components (edges_sql VARCHAR)
```

### Description

Strongly connected components of the **directed** graph: sets of vertices that
can all reach one another following edge direction. Same column shape as
`connected_components`.

Comparing the two tells you where one-way edges genuinely trap you.

### Example

```sql
SELECT component, count(*) AS members
FROM duckrouting_strong_components(
  'SELECT id, source, target, cost, reverse_cost FROM edges')
GROUP BY component ORDER BY component;
-- → component  members
-- →         1       13
-- →         2        2
-- →        13        2
```

## duckrouting_biconnected_components

### Signatures

```sql
TABLE duckrouting_biconnected_components (edges_sql VARCHAR)
```

### Description

Partitions the **edges** into biconnected components -- maximal sets in which
no single vertex removal disconnects anything. Returns `seq`, `component` and
`edge`, with `component` labelled by its smallest edge id.

### Example

```sql
SELECT component, count(*) AS edges
FROM duckrouting_biconnected_components(
  'SELECT id, source, target, cost, reverse_cost FROM edges')
GROUP BY component ORDER BY component;
-- → component  edges
-- →         1      1
-- →         2     12
-- →         6      1
-- →         7      1
-- →        14      1
-- →        17      1
-- →        18      1
```

The components holding a single edge are exactly the bridges.

## duckrouting_articulation_points

### Signatures

```sql
TABLE duckrouting_articulation_points (edges_sql VARCHAR)
```

### Description

Vertices whose removal would increase the number of connected components --
the single points of failure. Returns one `node` column.

### Example

```sql
SELECT * FROM duckrouting_articulation_points(
  'SELECT id, source, target, cost, reverse_cost FROM edges');
-- → node
-- →    3
-- →    6
-- →    7
-- →    8
```

## duckrouting_bridges

### Signatures

```sql
TABLE duckrouting_bridges (edges_sql VARCHAR)
```

### Description

Edges whose removal would disconnect the graph. Returns one `edge` column.

Equivalently: the biconnected components consisting of exactly one edge, which
is how this is computed.

### Example

```sql
SELECT * FROM duckrouting_bridges(
  'SELECT id, source, target, cost, reverse_cost FROM edges');
-- → edge
-- →    1
-- →    6
-- →    7
-- →   14
-- →   17
-- →   18
```

## duckrouting_make_connected

### Signatures

```sql
TABLE duckrouting_make_connected (edges_sql VARCHAR)
```

### Description

The vertex pairs that would have to be joined to make the graph connected.
Returns `seq`, `start_vid` and `end_vid`. Joining *n* components takes *n-1*
pairs, so an already-connected graph returns nothing.

### Example

```sql
SELECT * FROM duckrouting_make_connected(
  'SELECT id, source, target, cost, reverse_cost FROM edges');
-- → seq  start_vid  end_vid
-- →   1          9        2
-- →   2          4       13
```

::: warning Which vertex represents a component
The *number* of pairs is determined, but which vertex stands for each component
is Boost's choice and follows vertex ordering. pgRouting reports `(5,2)` here
instead of `(9,2)`. Both genuinely connect the graph -- do not assert on the
specific vertices.
:::
