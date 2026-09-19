#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace duckrouting {

//! Replaces every edge carrying points with the chain of segments between
//! them. This follows pgRouting's rules in src/withPoints/withPoints.cpp: a
//! point becomes a vertex numbered -pid, and which of the two directions it
//! connects to depends on the driving side.
std::vector<EdgeRow> SplitEdgesAtPoints(const std::vector<EdgeRow> &edges, const std::vector<PointOnEdge> &points,
                                        char driving_side) {
	// Group the points by the edge they sit on, in order along that edge.
	std::map<int64_t, std::vector<PointOnEdge>> by_edge;
	for (size_t i = 0; i < points.size(); i++) {
		by_edge[points[i].edge_id].push_back(points[i]);
	}
	for (auto entry = by_edge.begin(); entry != by_edge.end(); ++entry) {
		std::sort(entry->second.begin(), entry->second.end(), [](const PointOnEdge &a, const PointOnEdge &b) {
			if (a.fraction != b.fraction) {
				return a.fraction < b.fraction;
			}
			return a.pid < b.pid;
		});
	}

	std::vector<EdgeRow> result;
	for (size_t i = 0; i < edges.size(); i++) {
		const EdgeRow &edge = edges[i];
		auto entry = by_edge.find(edge.id);
		if (entry == by_edge.end()) {
			// No points here, so the edge survives unchanged.
			result.push_back(edge);
			continue;
		}

		const std::vector<PointOnEdge> &on_edge = entry->second;
		const bool one_way = !IsTraversable(edge.cost) || !IsTraversable(edge.reverse_cost);

		int64_t forward_from = edge.source;
		int64_t reverse_from = edge.source;
		double forward_fraction = 0;
		double reverse_fraction = 0;
		double forward_used = 0;
		double reverse_used = 0;

		for (size_t p = 0; p < on_edge.size(); p++) {
			const PointOnEdge &point = on_edge[p];
			const int64_t vertex = -point.pid;

			// A point sitting exactly on an endpoint is that endpoint, joined
			// by a free edge rather than splitting anything.
			if (point.fraction == 0) {
				result.push_back(EdgeRow {edge.id, edge.source, vertex, 0, 0});
				continue;
			}
			if (point.fraction == 1) {
				result.push_back(EdgeRow {edge.id, edge.target, vertex, 0, 0});
				continue;
			}

			const double forward_delta = point.fraction - forward_fraction;
			const double reverse_delta = point.fraction - reverse_fraction;

			// On a one-way street, or when either side is unspecified, the
			// point is reachable from both directions.
			if (one_way || driving_side == 'b' || point.side == 'b') {
				if (IsTraversable(edge.cost)) {
					result.push_back(EdgeRow {edge.id, forward_from, vertex, forward_delta * edge.cost, -1});
					forward_used += forward_delta * edge.cost;
				}
				if (IsTraversable(edge.reverse_cost)) {
					result.push_back(EdgeRow {edge.id, reverse_from, vertex, -1, reverse_delta * edge.reverse_cost});
					reverse_used += reverse_delta * edge.reverse_cost;
				}
				forward_from = vertex;
				forward_fraction = point.fraction;
				reverse_from = vertex;
				reverse_fraction = point.fraction;
				continue;
			}

			// Two-way street with a definite side: the point is only reachable
			// from the direction that passes it on the driving side.
			if (driving_side == point.side) {
				result.push_back(EdgeRow {edge.id, forward_from, vertex, forward_delta * edge.cost, -1});
				forward_used += forward_delta * edge.cost;
				forward_from = vertex;
				forward_fraction = point.fraction;
			} else {
				result.push_back(EdgeRow {edge.id, reverse_from, vertex, -1, reverse_delta * edge.reverse_cost});
				reverse_used += reverse_delta * edge.reverse_cost;
				reverse_from = vertex;
				reverse_fraction = point.fraction;
			}
		}

		// Whatever is left of the edge after the last point.
		if (IsTraversable(edge.cost)) {
			result.push_back(EdgeRow {edge.id, forward_from, edge.target, edge.cost - forward_used, -1});
		}
		if (IsTraversable(edge.reverse_cost)) {
			result.push_back(EdgeRow {edge.id, reverse_from, edge.target, -1, edge.reverse_cost - reverse_used});
		}
	}
	return result;
}

std::vector<PathRow> HidePoints(const std::vector<PathRow> &rows, const std::vector<int64_t> &visible) {
	std::set<int64_t> keep(visible.begin(), visible.end());
	std::vector<PathRow> kept;
	for (size_t i = 0; i < rows.size(); i++) {
		if (rows[i].node < 0 && !keep.count(rows[i].node)) {
			continue;
		}
		kept.push_back(rows[i]);
	}
	return kept;
}

std::vector<DrivingDistanceRow> WithPointsDrivingDistance(const std::vector<EdgeRow> &edges,
                                                          const std::vector<PointOnEdge> &points,
                                                          const std::vector<int64_t> &starts, double distance,
                                                          char driving_side, bool directed, bool details) {
	const auto split = SplitEdgesAtPoints(edges, points, driving_side);
	auto rows = DrivingDistance(split, starts, distance, directed, false);

	if (details) {
		return rows;
	}

	// Without details the point vertices are hidden, except where one is the
	// start of the search and so has to stay.
	std::set<int64_t> visible_starts(starts.begin(), starts.end());
	std::vector<DrivingDistanceRow> kept;
	for (size_t i = 0; i < rows.size(); i++) {
		if (rows[i].node < 0 && !visible_starts.count(rows[i].node)) {
			continue;
		}
		kept.push_back(rows[i]);
	}
	return kept;
}

} // namespace duckrouting
