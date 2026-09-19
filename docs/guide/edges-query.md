# The edges query

Every routing function takes its graph as a string of SQL. duckrouting runs
that query and reads four required columns plus one optional one.

## Required columns

| Column | Type | Meaning |
| --- | --- | --- |
| `id` | `BIGINT` | Identifier reported back in the `edge` output column |
| `source` | `BIGINT` | Start vertex |
| `target` | `BIGINT` | End vertex |
| `cost` | `DOUBLE` | Weight of traversing `source` → `target` |

## Optional column

| Column | Type | Meaning |
| --- | --- | --- |
| `reverse_cost` | `DOUBLE` | Weight of traversing `target` → `source` |

Column names are matched case-insensitively, and any numeric type is accepted
and cast. Missing required columns raise a binder error; a `NULL` in `id`,
`source`, `target` or `cost` raises an input error.

## How costs encode direction

This is the part that surprises people, and it is pgRouting's convention:

- **A negative cost means the edge does not exist in that direction.** It is
  not a cheap edge, and it is not an error.
- A missing or `NULL` `reverse_cost` is treated exactly as `-1`.

So with `cost = -1, reverse_cost = 1` the edge is traversable only from
`target` to `source`. Omit `reverse_cost` entirely and every edge becomes
strictly one-way.

In an **undirected** query (`directed => false`) both values still apply, but
direction stops mattering: the edge is usable from either end as long as at
least one of the two costs is non-negative.

::: warning Infinite costs
`cost = 'Infinity'` marks an edge as usable but unboundedly expensive. Routes
that must cross it are returned with `agg_cost` of `Infinity`, matching
pgRouting. Internally the value is stored as a large finite sentinel, because
Boost will not relax an edge whose weight is a true infinity. Note that
`-Infinity` is also treated as that sentinel rather than as "no edge" -- again
matching pgRouting, which tests for infinity before it tests for a negative
cost.
:::

## Example

A one-way street network where `cost` is travel time and closures are excluded
before routing:

```sql
SELECT * FROM duckrouting_dijkstra(
  $$
  SELECT id, source, target,
         CASE WHEN oneway THEN travel_time ELSE travel_time END AS cost,
         CASE WHEN oneway THEN -1          ELSE travel_time END AS reverse_cost
  FROM streets
  WHERE NOT closed
  $$,
  100, 250);
```

Using a dollar-quoted string keeps the inner SQL readable and avoids escaping
single quotes.
