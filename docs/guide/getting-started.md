# Getting started

duckrouting adds graph routing to DuckDB. It runs the Boost Graph Library over
any query that returns edges, and follows pgRouting's function semantics so
existing routing SQL ports with little more than a rename.

## Install

```sql
INSTALL duckrouting FROM community;
LOAD duckrouting;
```

To confirm the extension loaded, and see which Boost it was built against:

```sql
SELECT duckrouting_version('hello');
-- → Duckrouting hello, built against Boost 1_89
```

## A first route

duckrouting does not need a topology table. Any relation with an id, two
endpoints and a cost will do:

```sql
CREATE TABLE edges (
  id BIGINT, source BIGINT, target BIGINT, cost DOUBLE, reverse_cost DOUBLE
);
INSERT INTO edges VALUES
  (1, 5,  6, 1,  1), (2, 6, 10, -1, 1), (3, 10, 15, -1, 1),
  (4, 6,  7, 1,  1), (5, 10, 11, 1, -1), (6, 1,  3,  1, 1);
```

Then ask for a path:

```sql
SELECT node, edge, agg_cost
FROM duckrouting_dijkstra(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  5, 7);
-- → node  edge  agg_cost
-- →    5     1       0.0
-- →    6     4       1.0
-- →    7    -1       2.0
```

The first argument is a **string containing SQL**, not a table. That is how
pgRouting works, and keeping it means the graph can be filtered, joined or
computed on the fly without a separate materialised topology.

## Where to go next

- [The edges query](/guide/edges-query) -- the columns you must return, and how
  costs encode one-way streets.
- [Dijkstra family](/functions/dijkstra) -- the routing functions themselves.
- [pgRouting function catalog](/pgrouting-function-catalog) -- what exists
  upstream, and which parts are Boost-backed.
