#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"
#include "duckrouting/yen.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <vector>

namespace duckrouting {

namespace {

//! A search state is a vertex plus the edges most recently travelled, because
//! whether a turn is restricted depends on how you arrived, not just where you
//! are. The history is only as long as the longest restriction needs.
struct TurnState {
	uint64_t vertex;
	std::vector<int64_t> history;

	bool operator<(const TurnState &other) const {
		if (vertex != other.vertex) {
			return vertex < other.vertex;
		}
		return history < other.history;
	}
};

//! The extra cost of having just travelled `history` -- any restriction whose
//! path is a suffix of it applies.
double RestrictionPenalty(const std::vector<Restriction> &restrictions, const std::vector<int64_t> &history) {
	double penalty = 0;
	for (size_t r = 0; r < restrictions.size(); r++) {
		const std::vector<int64_t> &path = restrictions[r].path;
		if (path.size() > history.size()) {
			continue;
		}
		bool matches = true;
		for (size_t i = 0; i < path.size(); i++) {
			if (history[history.size() - path.size() + i] != path[i]) {
				matches = false;
				break;
			}
		}
		if (matches) {
			penalty += restrictions[r].cost;
		}
	}
	return penalty;
}

struct SearchResult {
	std::vector<int64_t> nodes;
	std::vector<int64_t> edges;
	std::vector<double> costs;
	double total = 0;
	bool found = false;
};

//! Dijkstra over (vertex, history) states.
SearchResult SolveWithRestrictions(const Adjacency &adjacency, const VertexIndex &index,
                                   const std::vector<Restriction> &restrictions, size_t history_length,
                                   uint64_t source, uint64_t sink) {
	SearchResult result;
	if (source == sink) {
		return result;
	}

	typedef std::pair<double, TurnState> Entry;
	struct Compare {
		bool operator()(const Entry &a, const Entry &b) const {
			return a.first > b.first;
		}
	};

	std::map<TurnState, double> distance;
	std::map<TurnState, TurnState> parent;
	std::map<TurnState, int64_t> parent_edge;
	std::map<TurnState, double> parent_cost;
	std::priority_queue<Entry, std::vector<Entry>, Compare> queue;

	TurnState start {};
	start.vertex = source;
	distance[start] = 0;
	queue.push(std::make_pair(0.0, start));

	TurnState arrival {};
	bool arrived = false;
	while (!queue.empty()) {
		const double at_cost = queue.top().first;
		const TurnState at = queue.top().second;
		queue.pop();
		auto known = distance.find(at);
		if (known == distance.end() || at_cost > known->second) {
			continue;
		}
		if (at.vertex == sink) {
			arrival = at;
			arrived = true;
			break;
		}

		for (size_t i = 0; i < adjacency[at.vertex].size(); i++) {
			const Arc &arc = adjacency[at.vertex][i];

			TurnState next {};
			next.vertex = arc.to;
			next.history = at.history;
			next.history.push_back(arc.edge);
			// Only keep as much history as the longest restriction can use.
			if (next.history.size() > history_length) {
				next.history.erase(next.history.begin(),
				                   next.history.begin() +
				                       static_cast<long>(next.history.size() - history_length));
			}

			const double step = arc.cost + RestrictionPenalty(restrictions, next.history);
			const double candidate = at_cost + step;
			auto seen = distance.find(next);
			if (seen != distance.end() && candidate >= seen->second) {
				continue;
			}
			distance[next] = candidate;
			parent[next] = at;
			parent_edge[next] = arc.edge;
			parent_cost[next] = step;
			queue.push(std::make_pair(candidate, next));
		}
	}

	if (!arrived) {
		return result;
	}

	// Walk the state chain back to the start.
	std::vector<int64_t> nodes;
	std::vector<int64_t> edges;
	std::vector<double> costs;
	TurnState at = arrival;
	while (true) {
		nodes.push_back(index.IdOf(at.vertex));
		auto previous = parent.find(at);
		if (previous == parent.end()) {
			break;
		}
		edges.push_back(parent_edge[at]);
		costs.push_back(parent_cost[at]);
		at = previous->second;
	}
	std::reverse(nodes.begin(), nodes.end());
	std::reverse(edges.begin(), edges.end());
	std::reverse(costs.begin(), costs.end());

	result.nodes = nodes;
	result.edges = edges;
	result.costs = costs;
	result.total = distance[arrival];
	result.found = true;
	return result;
}

std::vector<int64_t> Normalized(const std::vector<int64_t> &values) {
	std::vector<int64_t> result(values);
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

size_t HistoryLength(const std::vector<Restriction> &restrictions) {
	size_t longest = 1;
	for (size_t i = 0; i < restrictions.size(); i++) {
		longest = (std::max)(longest, restrictions[i].path.size());
	}
	return longest;
}

std::vector<PathRow> ToRows(const SearchResult &result, int64_t start_vid, int64_t end_vid) {
	std::vector<PathRow> rows;
	double agg_cost = 0;
	for (size_t i = 0; i < result.nodes.size(); i++) {
		PathRow row {};
		row.path_seq = static_cast<int64_t>(i) + 1;
		row.start_vid = start_vid;
		row.end_vid = end_vid;
		row.node = result.nodes[i];
		row.agg_cost = DecodeInfinity(agg_cost);
		if (i < result.edges.size()) {
			row.edge = result.edges[i];
			row.cost = DecodeInfinity(result.costs[i]);
			agg_cost += result.costs[i];
		} else {
			row.edge = -1;
			row.cost = 0;
		}
		rows.push_back(row);
	}
	return rows;
}

} // namespace

std::vector<PathRow> Trsp(const std::vector<EdgeRow> &edges, const std::vector<Restriction> &restrictions,
                          const std::vector<int64_t> &starts, const std::vector<int64_t> &ends, bool directed) {
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(),
	                 [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });

	VertexIndex index;
	const Adjacency adjacency = BuildAdjacency(ordered, index, directed);
	const size_t history = HistoryLength(restrictions);

	const auto normalized_starts = Normalized(starts);
	const auto normalized_ends = Normalized(ends);

	std::vector<PathRow> rows;
	for (size_t s = 0; s < normalized_starts.size(); s++) {
		uint64_t source = 0;
		if (!index.Find(normalized_starts[s], source)) {
			continue;
		}
		for (size_t e = 0; e < normalized_ends.size(); e++) {
			if (normalized_ends[e] == normalized_starts[s]) {
				continue;
			}
			uint64_t sink = 0;
			if (!index.Find(normalized_ends[e], sink)) {
				continue;
			}
			const SearchResult result =
			    SolveWithRestrictions(adjacency, index, restrictions, history, source, sink);
			if (!result.found) {
				continue;
			}
			auto path = ToRows(result, normalized_starts[s], normalized_ends[e]);
			rows.insert(rows.end(), path.begin(), path.end());
		}
	}
	return rows;
}

std::vector<ViaRow> TrspVia(const std::vector<EdgeRow> &edges, const std::vector<Restriction> &restrictions,
                            const std::vector<int64_t> &via, bool directed, bool strict, bool) {
	std::vector<ViaRow> rows;
	if (via.size() < 2) {
		return rows;
	}

	// Each leg is its own restricted search; an unreachable leg still consumes
	// a path_id so the numbering lines up with the via sequence.
	std::vector<std::vector<PathRow>> legs(via.size() - 1);
	std::vector<bool> found(via.size() - 1, false);
	for (size_t leg = 0; leg + 1 < via.size(); leg++) {
		std::vector<int64_t> from(1, via[leg]);
		std::vector<int64_t> to(1, via[leg + 1]);
		auto path = Trsp(edges, restrictions, from, to, directed);
		if (path.empty()) {
			if (strict) {
				return rows;
			}
			continue;
		}
		legs[leg] = path;
		found[leg] = true;
	}

	size_t last_found = 0;
	bool any = false;
	for (size_t leg = 0; leg < found.size(); leg++) {
		if (found[leg]) {
			last_found = leg;
			any = true;
		}
	}
	if (!any) {
		return rows;
	}

	double route_agg_cost = 0;
	for (size_t leg = 0; leg < legs.size(); leg++) {
		if (!found[leg]) {
			continue;
		}
		const std::vector<PathRow> &path = legs[leg];
		for (size_t i = 0; i < path.size(); i++) {
			ViaRow row {};
			row.path_id = static_cast<int64_t>(leg) + 1;
			row.path_seq = path[i].path_seq;
			row.start_vid = path[i].start_vid;
			row.end_vid = path[i].end_vid;
			row.node = path[i].node;
			row.edge = path[i].edge;
			row.cost = path[i].cost;
			row.agg_cost = path[i].agg_cost;
			row.route_agg_cost = route_agg_cost + path[i].agg_cost;
			if (leg == last_found && i + 1 == path.size()) {
				row.edge = -2;
			}
			rows.push_back(row);
		}
		route_agg_cost += path.back().agg_cost;
	}
	return rows;
}

} // namespace duckrouting
