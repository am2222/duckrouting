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

//! Shortest path from `start_vid` to `end_vid`. Returns an empty vector when
//! either endpoint is absent from the graph, when the target is unreachable,
//! or when the two endpoints are equal -- all of which pgRouting reports as
//! zero rows rather than as an error.
std::vector<PathRow> Dijkstra(const std::vector<EdgeRow> &edges, int64_t start_vid, int64_t end_vid, bool directed);

duckdb::TableFunctionSet GetDijkstraFunction();

} // namespace duckrouting
