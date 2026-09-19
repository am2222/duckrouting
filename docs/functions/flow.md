# Maximum flow

How much can be pushed through a network, and along which edges.

::: warning A different edges query
These functions read **capacities**, not costs:

| Column | Type | Meaning |
| --- | --- | --- |
| `id` | `BIGINT` | required |
| `source`, `target` | `BIGINT` | required |
| `capacity` | numeric | required — how much `source` → `target` carries |
| `reverse_capacity` | numeric | optional — how much `target` → `source` carries |

A missing or non-positive capacity means that direction carries nothing,
mirroring how `cost` works elsewhere. `duckrouting_max_flow_min_cost` wants
`cost` and `reverse_cost` as well; `duckrouting_edge_disjoint_paths` and
`duckrouting_max_cardinality_match` use `cost`/`reverse_cost` only, treating
every usable direction as capacity 1.
:::

All of them accept a single vertex or a `BIGINT[]` for source and sink. Several
sources or sinks are handled by attaching an unbounded super-source and
super-sink, which is how a many-to-many question becomes one flow problem.

## duckrouting_max_flow

```sql
TABLE duckrouting_max_flow (edges_sql VARCHAR, source BIGINT|BIGINT[], sink BIGINT|BIGINT[])
```

One row, one `DOUBLE`: the maximum total flow.

```sql
SELECT * FROM duckrouting_max_flow(
  'SELECT id, source, target, capacity, reverse_capacity FROM edges', 11, 12);
-- → 230.0

SELECT * FROM duckrouting_max_flow(
  'SELECT id, source, target, capacity, reverse_capacity FROM edges', 11, [5, 10, 12]);
-- → 340.0
```

## duckrouting_push_relabel, duckrouting_edmonds_karp, duckrouting_boykov_kolmogorov

```sql
TABLE duckrouting_push_relabel (edges_sql VARCHAR, source BIGINT|BIGINT[], sink BIGINT|BIGINT[])
-- identically for duckrouting_edmonds_karp and duckrouting_boykov_kolmogorov
```

The same maximum, broken down edge by edge. Returns `seq`, `edge`,
`start_vid`, `end_vid`, `flow` and `residual_capacity`. Only edges actually
carrying flow appear, and `flow + residual_capacity` is that direction's
capacity.

The three differ in algorithm, not in answer — though on a network with slack
they may distribute the same total differently.

```sql
SELECT edge, start_vid, end_vid, flow, residual_capacity
FROM duckrouting_push_relabel(
  'SELECT id, source, target, capacity, reverse_capacity FROM edges', 11, 12)
ORDER BY edge;
-- → edge  start_vid  end_vid   flow  residual_capacity
-- →    8         11        7  100.0               30.0
-- →   10          7        8  100.0               30.0
-- →   11         11       12  130.0                0.0
-- →   12          8       12  100.0                0.0
```

Reading that: 130 units go straight from 11 to 12, and another 100 detour via
7 and 8 — 230 in total.

## duckrouting_max_flow_min_cost and duckrouting_max_flow_min_cost_cost

```sql
TABLE duckrouting_max_flow_min_cost      (edges_sql VARCHAR, source BIGINT|BIGINT[], sink BIGINT|BIGINT[])
TABLE duckrouting_max_flow_min_cost_cost (edges_sql VARCHAR, source BIGINT|BIGINT[], sink BIGINT|BIGINT[])
```

Of all the ways to achieve the maximum flow, the cheapest. The first returns
`seq`, `edge`, `source`, `target`, `flow`, `residual_capacity`, `cost` and
`agg_cost`, where `cost` is `flow × the edge's cost`. The second returns that
total alone.

The edges query needs `cost` and `reverse_cost` in addition to the capacities.

```sql
SELECT edge, source, target, flow, cost, agg_cost
FROM duckrouting_max_flow_min_cost(
  'SELECT id, source, target, capacity, reverse_capacity, cost, reverse_cost FROM edges',
  11, 12);
-- → edge  source  target   flow   cost  agg_cost
-- →    8      11       7  100.0  100.0     100.0
-- →   10       7       8  100.0  100.0     200.0
-- →   11      11      12  130.0  130.0     330.0
-- →   12       8      12  100.0  100.0     430.0

SELECT * FROM duckrouting_max_flow_min_cost_cost(
  'SELECT id, source, target, capacity, reverse_capacity, cost, reverse_cost FROM edges',
  11, 12);
-- → 430.0
```

## duckrouting_edge_disjoint_paths

```sql
TABLE duckrouting_edge_disjoint_paths (edges_sql VARCHAR, start_vid BIGINT|BIGINT[], end_vid BIGINT|BIGINT[])
TABLE duckrouting_edge_disjoint_paths (edges_sql VARCHAR, start_vid BIGINT|BIGINT[], end_vid BIGINT|BIGINT[], directed BOOLEAN)
-- also accepts directed => BOOLEAN
```

As many routes as exist that share no edge — how many independent ways there
are between two points. Uses `cost`/`reverse_cost`, giving every usable
direction capacity 1, then decomposes the resulting flow back into paths.

Returns pgRouting's shortest-path shape: `seq`, `path_id`, `path_seq`,
`start_vid`, `end_vid`, `node`, `edge`, `cost`, `agg_cost`.

```sql
SELECT path_id, path_seq, node, edge FROM duckrouting_edge_disjoint_paths(
  'SELECT id, source, target, cost, reverse_cost FROM edges', 11, 12);
-- → path_id  path_seq  node  edge
-- →       1         1    11     8
-- →       1         2     7    10
-- →       1         3     8    12
-- →       1         4    12    -1
-- →       2         1    11    11
-- →       2         2    12    -1
```

Two independent routes from 11 to 12, sharing no edge.

## duckrouting_max_cardinality_match

```sql
TABLE duckrouting_max_cardinality_match (edges_sql VARCHAR)
```

The largest set of edges no two of which share a vertex. Returns one `edge`
column. Structural, so only `id`, `source` and `target` really matter.

```sql
SELECT * FROM duckrouting_max_cardinality_match(
  'SELECT id, source, target, cost, reverse_cost FROM edges');
-- → edge
-- →    1
-- →    5
-- →    6
-- →   13
-- →   14
-- →   16
-- →   17
-- →   18
```
