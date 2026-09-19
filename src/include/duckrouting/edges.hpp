#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckrouting {

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
	return cost >= 0 && !duckdb::Value::IsNan(cost);
}

//! Runs `edges_sql` and projects it onto the columns pgRouting requires:
//! id, source, target, cost, and the optional reverse_cost.
std::vector<EdgeRow> LoadEdges(duckdb::ClientContext &context, const std::string &edges_sql);

} // namespace duckrouting
