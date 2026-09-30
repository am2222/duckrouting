# Map matching

Snapping a noisy GPS trajectory onto the roads it was actually driven along.

::: info Not a pgRouting function
pgRouting has no map matching. These two functions implement
[Fast Map Matching](https://github.com/cyang-kth/fmm) (Yang & Gidófalvi,
2018), a hidden Markov model over candidate edges, on top of the same graph
machinery as everything else here. FMM is Apache-2.0; duckrouting follows its
algorithm and is checked against its published sample results, but shares
none of its source.
:::

## How it works

Each GPS fix is given a few **candidate** edges: the nearest ones within some
radius, each with the fraction along the edge where the fix projects and the
distance from the fix to that point. Matching then picks one candidate per fix
so that the whole sequence is the likeliest:

- The **emission probability** of a candidate is a Gaussian on its distance,
  `exp(-0.5 * (distance / gps_error)^2)`. Closer edges are likelier.
- The **transition probability** between candidates of consecutive fixes is
  the straight-line distance between the fixes divided by the shortest-path
  distance between the candidates, capped at 1. A route that has to wander far
  from the straight line is unlikely; one with no route at all is impossible.
- Viterbi finds the sequence that maximises the product, and the shortest
  paths between the chosen candidates are stitched into the complete route.

The shortest-path distances are what make this a graph problem. A candidate is
a point partway along an edge, exactly as the `with_points` family understands
one, so a partial edge costs `fraction * cost` and the distance between two
candidates is an ordinary shortest path over the split graph. No geometry is
involved on the graph side at all, which is also why `cost` should be a
length: the transition probability compares it to a straight-line distance.

## The candidates query

Both functions take an edges query and a **candidates** query. The candidates
query has one row per (fix, candidate edge):

| Column | Type | |
| --- | --- | --- |
| `pid` | `BIGINT` | required — identifies the fix and orders fixes within a trajectory |
| `edge_id` | `BIGINT` | required — an `id` from the edges query |
| `fraction` | `DOUBLE` | required — how far along the edge the fix projects, 0 to 1 |
| `distance` | `DOUBLE` | required — from the fix to that point on the edge |
| `x`, `y` | `DOUBLE` | required — the fix itself |
| `traj_id` | `BIGINT` | optional — groups fixes into trajectories; defaults to 1 |

`pid`, `edge_id`, `fraction` and `distance` are exactly what
`duckrouting_find_close_edges` returns, so with the spatial extension loaded
the candidates query is a join between that and the fixes:

```sql
CREATE TABLE candidates AS
SELECT f.traj_id, f.pid, c.edge_id, c.fraction, c.distance, ST_X(f.geom) AS x, ST_Y(f.geom) AS y
FROM duckrouting_find_close_edges('edges', 'fixes', tolerance => 0.4, cap => 4) c
JOIN fixes f USING (pid);
```

`tolerance` is FMM's search radius and `cap` its `k`. A fix with no candidate
within the radius simply has no rows, and is not part of the trajectory.

## duckrouting_map_match

### Signatures

```sql
TABLE duckrouting_map_match (edges_sql VARCHAR, candidates_sql VARCHAR)
TABLE duckrouting_map_match (edges_sql VARCHAR, candidates_sql VARCHAR, gps_error DOUBLE)
-- also accepts gps_error => DOUBLE, delta => DOUBLE,
-- reverse_tolerance => DOUBLE and directed => BOOLEAN
```

### Description

Returns one row per fix: `seq`, `traj_id`, `pid`, the matched `edge` and
`fraction`, the candidate's `distance`, its emission probability `ep`, and the
transition probability `tp` and shortest-path distance `sp_dist` from the fix
before it. The first fix of a trajectory has `tp` and `sp_dist` of 0.

| Parameter | Default | |
| --- | --- | --- |
| `gps_error` | 50 | standard deviation of the GPS noise, in the units of `distance` |
| `delta` | unbounded | upper bound on the shortest-path search between consecutive fixes, in cost units; FMM's UBODT bound |
| `reverse_tolerance` | 0 | how far back along the same edge a fix may step, as a fraction of the edge, before it counts as a trip round the block rather than jitter |
| `directed` | true | |

A trajectory is **unmatched** when some fix cannot be reached from the one
before it — because no path exists, or none within `delta`. As in FMM, an
unmatched trajectory produces no rows rather than a partial match.

### Example

FMM's sample network is a unit grid with two-way streets written as separate
edges in each direction. Its first sample trip runs up one street, across and
up again:

```sql
SELECT pid, edge, round(fraction, 3) AS fraction, round(ep, 3) AS ep, round(tp, 3) AS tp
FROM duckrouting_map_match(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  'SELECT traj_id, pid, edge_id, fraction, distance, x, y FROM candidates WHERE traj_id = 1',
  0.5);
-- → pid  edge  fraction     ep     tp
-- →   1     2     0.251  0.792  0.000
-- →   2     2     0.702  0.788  1.000
-- →   3    13     0.493  0.896  0.756
-- →   4    14     0.549  0.975  1.000
-- →   5    23     0.458  0.966  0.896
```

Fix 3 has `tp` below 1 because getting there from fix 2 means driving to the
end of edge 2, along edge 5 and partway into edge 13, further than the
straight line between the two fixes.

## duckrouting_map_match_path

### Signatures

```sql
TABLE duckrouting_map_match_path (edges_sql VARCHAR, candidates_sql VARCHAR)
TABLE duckrouting_map_match_path (edges_sql VARCHAR, candidates_sql VARCHAR, gps_error DOUBLE)
-- also accepts the same named parameters plus details => BOOLEAN
```

### Description

The complete route through the network for each matched trajectory — FMM's
`cpath` — in the k-shortest-paths shape: `seq`, `path_id`, `path_seq`,
`start_vid`, `end_vid`, `node`, `edge`, `cost`, `agg_cost`. `path_id` is the
trajectory, and the route starts at the first fix and ends at the last, which
appear as vertices `-pid` the way the `with_points` family reports points.

Without `details` the fixes in between are hidden and their segments merged,
so each edge appears once with its whole cost. With `details => true` every
fix is a vertex, splitting the edge it sits on at its fraction.

### Example

```sql
SELECT path_seq, node, edge, round(cost, 3) AS cost, round(agg_cost, 3) AS agg_cost
FROM duckrouting_map_match_path(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  'SELECT traj_id, pid, edge_id, fraction, distance, x, y FROM candidates WHERE traj_id = 1',
  0.5);
-- → path_seq  node  edge   cost  agg_cost
-- →        1    -1     2  0.749     0.000
-- →        2     1     5  1.000     0.749
-- →        3     5    13  1.000     1.749
-- →        4     6    14  1.000     2.749
-- →        5     9    23  0.458     3.749
-- →        6    -5    -1  0.000     4.207
```

The edge sequence, `2, 5, 13, 14, 23`, is exactly what FMM reports for this
trip, and 4.207 is the length of its matched geometry. Collapsing to the
edges is usually what you want:

```sql
SELECT path_id, list(edge ORDER BY path_seq) FILTER (WHERE edge <> -1) AS cpath
FROM duckrouting_map_match_path(
  'SELECT id, source, target, cost, reverse_cost FROM edges',
  'SELECT traj_id, pid, edge_id, fraction, distance, x, y FROM candidates', 0.5)
GROUP BY path_id ORDER BY path_id;
-- → path_id  cpath
-- →       1  [2, 5, 13, 14, 23]
-- →       2  [25, 4, 3, 5, 17, 19]
-- →       3  [8, 11, 13, 18, 20, 24]
```

To rebuild the matched geometry, join `details => true` rows back to the edge
geometries and take `ST_LineSubstring` from the matched fraction wherever a
row starts or ends at a fix; `test/sql/map_match_spatial.test` does exactly
this.

::: warning Where this differs from FMM
- FMM measures along an edge in units of its geometry's length. Here it is
  `fraction * cost`, so `cost` must be a length for the transition probability
  to mean what it should. `reverse_cost` and one-way streets work as they do
  everywhere else in duckrouting; FMM has no equivalent.
- FMM proper precomputes an origin–destination table and looks paths up; this
  runs a bounded search per candidate, which is its STMATCH variant. The
  results are the same; `delta` plays the role of the table's upper bound.
- FMM breaks off a trajectory at a fix with no candidates. Here such a fix
  simply has no rows in the candidates query and the trajectory continues
  across it.
:::
