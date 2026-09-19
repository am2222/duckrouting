#pragma once

#include "duckdb.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace duckrouting {

//! pgRouting stores an infinite edge cost as a large *finite* sentinel, because
//! Boost's relaxation test is `dist[u] + w < dist[v]` -- with a true infinity
//! that reduces to `inf < inf`, which is false, so the edge would never be
//! relaxed and the route would silently disappear. Substituting DBL_MAX keeps
//! the edge usable (DBL_MAX < inf holds, given distance_inf is a true infinity)
//! and the value is mapped back to Infinity on output.
//! See pgRouting src/cpp_common/pgdata_fetchers.cpp and to_postgres.cpp.
constexpr double kInfiniteCost = (std::numeric_limits<double>::max)();

//! Applied when reading `edges_sql`.
inline double EncodeInfinity(double cost) {
	return std::isinf(cost) ? kInfiniteCost : cost;
}

//! Applied to every cost and agg_cost on the way out.
inline double DecodeInfinity(double value) {
	return std::fabs(value - kInfiniteCost) < 1 ? std::numeric_limits<double>::infinity() : value;
}

//! One row of a pgRouting-style `edges_sql` result.
//! A negative cost means the edge does not exist in that direction, which is
//! how pgRouting encodes one-way streets. Missing/NULL reverse_cost becomes -1.
struct EdgeRow {
	int64_t id;
	int64_t source;
	int64_t target;
	double cost;
	double reverse_cost;
};

//! True when a cost value describes a traversable edge.
inline bool IsTraversable(double cost) {
	return cost >= 0 && !std::isnan(cost);
}

//! Runs `edges_sql` and projects it onto the columns pgRouting requires:
//! id, source, target, cost, and the optional reverse_cost.
//!
//! A few functions never report an edge back -- pgRouting's johnson and
//! floydWarshall are documented with `SELECT source, target, cost` -- so `id`
//! can be made optional, in which case row numbers stand in for it.
std::vector<EdgeRow> LoadEdges(duckdb::ClientContext &context, const std::string &edges_sql,
                               bool require_id = true);

//! One row of a flow query. The flow functions want capacities rather than
//! costs, and the min-cost variant wants both. A negative or missing value
//! means that direction is unusable, exactly as with cost.
struct FlowEdgeRow {
	int64_t id;
	int64_t source;
	int64_t target;
	double capacity;
	double reverse_capacity;
	double cost;
	double reverse_cost;
};

//! Runs `edges_sql` and reads id, source, target plus whichever of capacity,
//! reverse_capacity, cost and reverse_cost it exposes. Absent columns come back
//! as -1. `require_capacity` and `require_cost` make those columns mandatory.
std::vector<FlowEdgeRow> LoadFlowEdges(duckdb::ClientContext &context, const std::string &edges_sql,
                                       bool require_capacity, bool require_cost);

//! One row of an A* query: an ordinary edge plus the coordinates of its two
//! endpoints, which the heuristic needs to estimate remaining distance.
struct CoordinateEdgeRow {
	EdgeRow edge;
	double x1;
	double y1;
	double x2;
	double y2;
};

//! Runs `edges_sql` and reads the usual columns plus x1, y1, x2, y2.
std::vector<CoordinateEdgeRow> LoadCoordinateEdges(duckdb::ClientContext &context,
                                                   const std::string &edges_sql);

//! One cell of a cost matrix, as produced by the *CostMatrix functions.
struct MatrixCell {
	int64_t start_vid;
	int64_t end_vid;
	double agg_cost;
};

//! Reads a query exposing start_vid, end_vid and agg_cost.
std::vector<MatrixCell> LoadCostMatrix(duckdb::ClientContext &context, const std::string &matrix_sql);

//! A placed vertex, for the Euclidean travelling-salesman variant.
struct PlacedPoint {
	int64_t id;
	double x;
	double y;
};

//! Reads a query exposing id, x and y.
std::vector<PlacedPoint> LoadPoints(duckdb::ClientContext &context, const std::string &points_sql);

//! A point sitting partway along an edge. `fraction` is how far from the
//! edge's source it lies, and `side` is which side of the road it is on
//! ('r', 'l' or 'b' for both).
struct PointOnEdge {
	int64_t pid;
	int64_t edge_id;
	double fraction;
	char side;
};

//! Reads a query exposing pid, edge_id, fraction and the optional side.
std::vector<PointOnEdge> LoadPointsOnEdges(duckdb::ClientContext &context, const std::string &points_sql);

} // namespace duckrouting
