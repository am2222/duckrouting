#pragma once

#include "duckdb.hpp"
#include "duckrouting/edges.hpp"

#include <cstdint>
#include <vector>

namespace duckrouting {

//! One row of pgRouting's shortest-path result shape.
struct PathRow {
	int64_t path_seq;
	int64_t start_vid;
	int64_t end_vid;
	int64_t node;
	int64_t edge;
	double cost;
	double agg_cost;
};

//! One row of the aggregated-cost shape shared by dijkstraCost and
//! dijkstraCostMatrix.
struct CostRow {
	int64_t start_vid;
	int64_t end_vid;
	double agg_cost;
};

//! One row of the drivingDistance shape.
struct DrivingDistanceRow {
	int64_t depth;
	int64_t start_vid;
	int64_t pred;
	int64_t node;
	int64_t edge;
	double cost;
	double agg_cost;
};

//! Shortest paths for every (start, end) combination. Start and end lists are
//! de-duplicated and sorted, pairs whose endpoints coincide are dropped, and
//! unreachable pairs contribute no rows -- all matching pgRouting. Results are
//! ordered by start_vid, then end_vid, then path order.
std::vector<PathRow> Dijkstra(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                              const std::vector<int64_t> &ends, bool directed);

//! Total cost per reachable (start, end) pair; no per-node rows.
std::vector<CostRow> DijkstraCost(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                  const std::vector<int64_t> &ends, bool directed);

//! Every ordered pair drawn from `vids`, as a cost matrix.
std::vector<CostRow> DijkstraCostMatrix(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &vids,
                                        bool directed);

//! Every node reachable from `starts` within `distance`. With `equicost`, a
//! node is reported only for the start it is cheapest from.
std::vector<DrivingDistanceRow> DrivingDistance(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                                double distance, bool directed, bool equicost);

//! One row of the dijkstraVia shape. `route_agg_cost` accumulates across the
//! whole route, while `agg_cost` restarts on every leg.
struct ViaRow {
	int64_t path_id;
	int64_t path_seq;
	int64_t start_vid;
	int64_t end_vid;
	int64_t node;
	int64_t edge;
	double cost;
	double agg_cost;
	double route_agg_cost;
};

//! One row of the KSP shape.
struct KspRow {
	int64_t path_id;
	int64_t path_seq;
	int64_t start_vid;
	int64_t end_vid;
	int64_t node;
	int64_t edge;
	double cost;
	double agg_cost;
};

//! Routes through `via_vids` in order, one leg per consecutive pair. The final
//! leg's last row is terminated with edge -2 rather than -1.
//!
//! With `strict`, a leg that cannot be routed abandons the whole route and
//! returns nothing; otherwise that leg emits no rows, still consumes a
//! path_id, and the route continues from the next via vertex.
//!
//! With `u_turn_on_edge` false, a leg may not leave a via vertex by the edge
//! the previous leg arrived on, unless the vertex is a dead end or the
//! restriction would make the next vertex unreachable.
std::vector<ViaRow> DijkstraVia(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &via_vids,
                                bool directed, bool strict, bool u_turn_on_edge);

//! The `cap` cheapest (start, end) pairs, as full paths.
std::vector<PathRow> DijkstraNear(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                  const std::vector<int64_t> &ends, bool directed, int64_t cap);

//! The `cap` cheapest (start, end) pairs, as costs only.
std::vector<CostRow> DijkstraNearCost(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                      const std::vector<int64_t> &ends, bool directed, int64_t cap);

//! K shortest loopless paths (Yen, 1971) for every (start, end) combination.
//! With `heap_paths`, the candidates generated but not selected are returned
//! as well. Unlike the rest of this family this is not a Boost algorithm --
//! Boost has no k-shortest-paths routine -- so it is implemented here directly
//! on top of repeated constrained shortest-path solves.
std::vector<KspRow> Ksp(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                        const std::vector<int64_t> &ends, int64_t k, bool directed, bool heap_paths);

duckdb::TableFunctionSet GetDijkstraFunction();
duckdb::TableFunctionSet GetDijkstraCostFunction();
duckdb::TableFunctionSet GetDijkstraCostMatrixFunction();
duckdb::TableFunctionSet GetDrivingDistanceFunction();
duckdb::TableFunctionSet GetDijkstraViaFunction();
duckdb::TableFunctionSet GetDijkstraNearFunction();
duckdb::TableFunctionSet GetDijkstraNearCostFunction();
duckdb::TableFunctionSet GetKspFunction();

} // namespace duckrouting
