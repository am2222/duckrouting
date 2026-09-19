#include "duckrouting/dijkstra.hpp"

#include "duckrouting/graph.hpp"
#include "duckrouting/yen.hpp"

#include <boost/graph/dijkstra_shortest_paths.hpp>

#include <algorithm>
#include <limits>
#include <map>

namespace duckrouting {

namespace {

//! De-duplicate and sort, so results come out in pgRouting's order and a
//! repeated vertex does not produce repeated rows.
std::vector<int64_t> Normalize(const std::vector<int64_t> &vids) {
	std::vector<int64_t> result(vids);
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

//! Cheapest edge from `from` to `to`. Dijkstra relaxes parallel edges by their
//! minimum, so the cheapest one is the edge that the shortest path actually used.
template <typename Graph>
bool CheapestEdge(const Graph &graph, uint64_t from, uint64_t to, RoutingEdge &result) {
	bool found = false;
	typename boost::graph_traits<Graph>::out_edge_iterator it, end;
	for (boost::tie(it, end) = boost::out_edges(from, graph); it != end; ++it) {
		if (static_cast<uint64_t>(boost::target(*it, graph)) != to) {
			continue;
		}
		const RoutingEdge &candidate = graph[*it];
		if (!found || candidate.cost < result.cost) {
			result = candidate;
			found = true;
		}
	}
	return found;
}

//! One full single-source Dijkstra. Every caller in this file runs this once
//! per start vertex and then reads whatever it needs out of the two maps.
template <typename Graph>
void RunDijkstra(const Graph &graph, uint64_t source, std::vector<uint64_t> &predecessor,
                 std::vector<double> &distance) {
	predecessor.assign(boost::num_vertices(graph), 0);
	distance.assign(boost::num_vertices(graph), 0);
	boost::dijkstra_shortest_paths(
	    graph, source,
	    boost::predecessor_map(
	        boost::make_iterator_property_map(predecessor.begin(), boost::get(boost::vertex_index, graph)))
	        .distance_map(
	            boost::make_iterator_property_map(distance.begin(), boost::get(boost::vertex_index, graph)))
	        .weight_map(boost::get(&RoutingEdge::cost, graph))
	        // A true infinity, not Boost's default of numeric_limits<double>::max().
	        // This is what lets an edge carrying the kInfiniteCost sentinel relax:
	        // DBL_MAX < inf holds, where DBL_MAX < DBL_MAX would not.
	        .distance_inf(std::numeric_limits<double>::infinity()));
}

bool Reached(const std::vector<uint64_t> &predecessor, uint64_t source, uint64_t sink) {
	// Boost leaves a vertex as its own predecessor when it was never reached.
	return sink == source || predecessor[sink] != sink;
}

//! Turn a predecessor chain into pgRouting's per-node rows.
template <typename Graph>
std::vector<PathRow> ExtractPath(const Graph &graph, const VertexIndex &index,
                                 const std::vector<uint64_t> &predecessor, uint64_t source, uint64_t sink,
                                 int64_t start_vid, int64_t end_vid) {
	std::vector<uint64_t> path;
	for (uint64_t at = sink;; at = predecessor[at]) {
		path.push_back(at);
		if (at == source) {
			break;
		}
	}
	std::reverse(path.begin(), path.end());

	std::vector<PathRow> rows;
	rows.reserve(path.size());
	double agg_cost = 0;
	for (size_t i = 0; i < path.size(); i++) {
		PathRow row {};
		row.path_seq = static_cast<int64_t>(i) + 1;
		row.start_vid = start_vid;
		row.end_vid = end_vid;
		row.node = index.IdOf(path[i]);
		row.agg_cost = DecodeInfinity(agg_cost);
		if (i + 1 < path.size()) {
			RoutingEdge used {};
			if (!CheapestEdge(graph, path[i], path[i + 1], used)) {
				return {};
			}
			row.edge = used.id;
			row.cost = DecodeInfinity(used.cost);
			agg_cost += used.cost;
		} else {
			// pgRouting terminates a path with edge -1 and cost 0.
			row.edge = -1;
			row.cost = 0;
		}
		rows.push_back(row);
	}
	return rows;
}

//! Solved single-source state for one start vertex.
struct Solved {
	std::vector<uint64_t> predecessor;
	std::vector<double> distance;
	uint64_t source;
	bool found;
};

template <typename Graph>
Solved SolveFrom(const Graph &graph, const VertexIndex &index, int64_t start_vid) {
	Solved solved {};
	solved.found = index.Find(start_vid, solved.source);
	if (solved.found) {
		RunDijkstra(graph, solved.source, solved.predecessor, solved.distance);
	}
	return solved;
}

template <typename Graph>
std::vector<PathRow> DijkstraImpl(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                  const std::vector<int64_t> &ends) {
	VertexIndex index;
	auto graph = BuildGraph<Graph>(edges, index);
	std::vector<PathRow> rows;
	for (size_t s = 0; s < starts.size(); s++) {
		const int64_t start_vid = starts[s];
		Solved solved = SolveFrom(graph, index, start_vid);
		if (!solved.found) {
			continue;
		}
		for (size_t e = 0; e < ends.size(); e++) {
			const int64_t end_vid = ends[e];
			// pgRouting drops pairs whose endpoints coincide rather than
			// emitting a zero-length path.
			if (end_vid == start_vid) {
				continue;
			}
			uint64_t sink = 0;
			if (!index.Find(end_vid, sink) || !Reached(solved.predecessor, solved.source, sink)) {
				continue;
			}
			auto path = ExtractPath(graph, index, solved.predecessor, solved.source, sink, start_vid, end_vid);
			rows.insert(rows.end(), path.begin(), path.end());
		}
	}
	return rows;
}

template <typename Graph>
std::vector<CostRow> DijkstraCostImpl(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                      const std::vector<int64_t> &ends) {
	VertexIndex index;
	auto graph = BuildGraph<Graph>(edges, index);
	std::vector<CostRow> rows;
	for (size_t s = 0; s < starts.size(); s++) {
		const int64_t start_vid = starts[s];
		Solved solved = SolveFrom(graph, index, start_vid);
		if (!solved.found) {
			continue;
		}
		for (size_t e = 0; e < ends.size(); e++) {
			const int64_t end_vid = ends[e];
			if (end_vid == start_vid) {
				continue;
			}
			uint64_t sink = 0;
			if (!index.Find(end_vid, sink) || !Reached(solved.predecessor, solved.source, sink)) {
				continue;
			}
			rows.push_back(CostRow {start_vid, end_vid, DecodeInfinity(solved.distance[sink])});
		}
	}
	return rows;
}

template <typename Graph>
std::vector<DrivingDistanceRow> DrivingDistanceImpl(const std::vector<EdgeRow> &edges,
                                                    const std::vector<int64_t> &starts, double distance_limit) {
	VertexIndex index;
	auto graph = BuildGraph<Graph>(edges, index);
	std::vector<DrivingDistanceRow> rows;
	for (size_t s = 0; s < starts.size(); s++) {
		const int64_t start_vid = starts[s];
		Solved solved = SolveFrom(graph, index, start_vid);
		if (!solved.found) {
			continue;
		}
		for (uint64_t v = 0; v < index.Size(); v++) {
			if (!Reached(solved.predecessor, solved.source, v) || solved.distance[v] > distance_limit) {
				continue;
			}
			DrivingDistanceRow row {};
			row.start_vid = start_vid;
			row.node = index.IdOf(v);
			row.agg_cost = DecodeInfinity(solved.distance[v]);
			if (v == solved.source) {
				// pgRouting roots the tree at depth 0 with edge -1.
				row.depth = 0;
				row.pred = start_vid;
				row.edge = -1;
				row.cost = 0;
			} else {
				const uint64_t parent = solved.predecessor[v];
				row.pred = index.IdOf(parent);
				RoutingEdge used {};
				if (!CheapestEdge(graph, parent, v, used)) {
					continue;
				}
				row.edge = used.id;
				row.cost = DecodeInfinity(used.cost);
				// Depth is hop count from the root, which the predecessor
				// chain gives directly.
				int64_t depth = 0;
				for (uint64_t at = v; at != solved.source; at = solved.predecessor[at]) {
					depth++;
				}
				row.depth = depth;
			}
			rows.push_back(row);
		}
	}
	return rows;
}

} // namespace

std::vector<PathRow> Dijkstra(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                              const std::vector<int64_t> &ends, bool directed) {
	const auto s = Normalize(starts);
	const auto e = Normalize(ends);
	return directed ? DijkstraImpl<DirectedGraph>(edges, s, e) : DijkstraImpl<UndirectedGraph>(edges, s, e);
}

std::vector<CostRow> DijkstraCost(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                  const std::vector<int64_t> &ends, bool directed) {
	const auto s = Normalize(starts);
	const auto e = Normalize(ends);
	return directed ? DijkstraCostImpl<DirectedGraph>(edges, s, e) : DijkstraCostImpl<UndirectedGraph>(edges, s, e);
}

std::vector<CostRow> DijkstraCostMatrix(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &vids,
                                        bool directed) {
	return DijkstraCost(edges, vids, vids, directed);
}

std::vector<DrivingDistanceRow> DrivingDistance(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                                double distance_limit, bool directed, bool equicost) {
	const auto s = Normalize(starts);
	auto rows = directed ? DrivingDistanceImpl<DirectedGraph>(edges, s, distance_limit)
	                     : DrivingDistanceImpl<UndirectedGraph>(edges, s, distance_limit);

	if (equicost) {
		// Keep each node only for the start it is cheapest from.
		std::map<int64_t, size_t> best;
		for (size_t i = 0; i < rows.size(); i++) {
			auto entry = best.find(rows[i].node);
			if (entry == best.end() || rows[i].agg_cost < rows[entry->second].agg_cost) {
				best[rows[i].node] = i;
			}
		}
		std::vector<DrivingDistanceRow> kept;
		kept.reserve(best.size());
		for (size_t i = 0; i < rows.size(); i++) {
			if (best[rows[i].node] == i) {
				kept.push_back(rows[i]);
			}
		}
		rows = kept;
	}

	// pgRouting orders by start_vid, then depth, then node.
	std::stable_sort(rows.begin(), rows.end(), [](const DrivingDistanceRow &a, const DrivingDistanceRow &b) {
		if (a.start_vid != b.start_vid) {
			return a.start_vid < b.start_vid;
		}
		if (a.depth != b.depth) {
			return a.depth < b.depth;
		}
		return a.node < b.node;
	});
	return rows;
}

std::vector<ViaRow> DijkstraVia(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &via_vids,
                                bool directed) {
	std::vector<ViaRow> rows;
	if (via_vids.size() < 2) {
		return rows;
	}

	// Collect the legs first, so we know which one is last and therefore which
	// row carries pgRouting's -2 terminator.
	std::vector<std::vector<PathRow>> legs;
	for (size_t i = 0; i + 1 < via_vids.size(); i++) {
		std::vector<int64_t> from(1, via_vids[i]);
		std::vector<int64_t> to(1, via_vids[i + 1]);
		auto leg = Dijkstra(edges, from, to, directed);
		// An unreachable leg contributes nothing and the route carries on from
		// the next via vertex.
		if (!leg.empty()) {
			legs.push_back(leg);
		}
	}

	double route_agg_cost = 0;
	for (size_t l = 0; l < legs.size(); l++) {
		const bool last_leg = (l + 1 == legs.size());
		const std::vector<PathRow> &leg = legs[l];
		for (size_t i = 0; i < leg.size(); i++) {
			ViaRow row {};
			row.path_id = static_cast<int64_t>(l) + 1;
			row.path_seq = leg[i].path_seq;
			row.start_vid = leg[i].start_vid;
			row.end_vid = leg[i].end_vid;
			row.node = leg[i].node;
			row.edge = leg[i].edge;
			row.cost = leg[i].cost;
			row.agg_cost = leg[i].agg_cost;
			row.route_agg_cost = route_agg_cost + leg[i].agg_cost;
			if (last_leg && i + 1 == leg.size()) {
				// pgRouting closes the whole route with -2 rather than -1.
				row.edge = -2;
			}
			rows.push_back(row);
		}
		route_agg_cost += leg.back().agg_cost;
	}
	return rows;
}

namespace {

//! The `cap` cheapest pairs, ordered by cost then by the vertex ids so that
//! ties are at least deterministic.
std::vector<CostRow> NearestPairs(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                  const std::vector<int64_t> &ends, bool directed, int64_t cap) {
	auto costs = DijkstraCost(edges, starts, ends, directed);
	std::stable_sort(costs.begin(), costs.end(), [](const CostRow &a, const CostRow &b) {
		if (a.agg_cost != b.agg_cost) {
			return a.agg_cost < b.agg_cost;
		}
		if (a.start_vid != b.start_vid) {
			return a.start_vid < b.start_vid;
		}
		return a.end_vid < b.end_vid;
	});
	if (cap > 0 && static_cast<int64_t>(costs.size()) > cap) {
		costs.resize(static_cast<size_t>(cap));
	}
	return costs;
}

} // namespace

std::vector<CostRow> DijkstraNearCost(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                      const std::vector<int64_t> &ends, bool directed, int64_t cap) {
	return NearestPairs(edges, starts, ends, directed, cap);
}

std::vector<PathRow> DijkstraNear(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                  const std::vector<int64_t> &ends, bool directed, int64_t cap) {
	const auto chosen = NearestPairs(edges, starts, ends, directed, cap);
	std::vector<PathRow> rows;
	for (size_t i = 0; i < chosen.size(); i++) {
		std::vector<int64_t> from(1, chosen[i].start_vid);
		std::vector<int64_t> to(1, chosen[i].end_vid);
		auto path = Dijkstra(edges, from, to, directed);
		rows.insert(rows.end(), path.begin(), path.end());
	}
	return rows;
}

std::vector<KspRow> Ksp(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                        const std::vector<int64_t> &ends, int64_t k, bool directed, bool heap_paths) {
	const auto normalized_starts = Normalize(starts);
	const auto normalized_ends = Normalize(ends);

	VertexIndex index;
	Adjacency adjacency = BuildAdjacency(edges, index, directed);

	std::vector<KspRow> rows;
	// path_id runs across the whole result rather than restarting per pair,
	// which is what pgRouting does.
	int64_t path_id = 0;
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

			std::vector<SimplePath> selected;
			std::vector<SimplePath> rejected;
			YenKShortestPaths(adjacency, source, sink, k, selected, rejected);
			if (heap_paths) {
				selected.insert(selected.end(), rejected.begin(), rejected.end());
			}

			for (size_t p = 0; p < selected.size(); p++) {
				path_id++;
				double agg_cost = 0;
				for (size_t i = 0; i < selected[p].steps.size(); i++) {
					const PathStep &step = selected[p].steps[i];
					KspRow row {};
					row.path_id = path_id;
					row.path_seq = static_cast<int64_t>(i) + 1;
					row.start_vid = normalized_starts[s];
					row.end_vid = normalized_ends[e];
					row.node = index.IdOf(step.vertex);
					row.edge = step.edge;
					row.cost = DecodeInfinity(step.cost);
					row.agg_cost = DecodeInfinity(agg_cost);
					agg_cost += step.cost;
					rows.push_back(row);
				}
			}
		}
	}
	return rows;
}

} // namespace duckrouting
