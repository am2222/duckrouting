#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"
#include "duckrouting/yen.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <queue>

namespace duckrouting {

namespace {

//! Where each vertex sits, for the heuristics. Only the A* variants use it.
struct Placement {
	double x = 0;
	double y = 0;
};

//! Reverses an adjacency list, so the backward search can walk arcs the wrong
//! way without rebuilding the graph.
Adjacency Reverse(const Adjacency &adjacency) {
	Adjacency reversed(adjacency.size());
	for (size_t from = 0; from < adjacency.size(); from++) {
		for (size_t i = 0; i < adjacency[from].size(); i++) {
			const Arc &arc = adjacency[from][i];
			reversed[arc.to].push_back(Arc {static_cast<uint64_t>(from), arc.edge, arc.cost});
		}
	}
	return reversed;
}

//! pgRouting's heuristics, as in the A* family.
double Estimate(const std::vector<Placement> &placement, uint64_t from, uint64_t to, Heuristic heuristic,
                double factor) {
	if (heuristic == Heuristic::None) {
		return 0;
	}
	const double dx = placement[to].x - placement[from].x;
	const double dy = placement[to].y - placement[from].y;
	switch (heuristic) {
	case Heuristic::MaxDelta:
		return std::fabs((std::max)(dx, dy)) * factor;
	case Heuristic::MinDelta:
		return std::fabs((std::min)(dx, dy)) * factor;
	case Heuristic::SquaredEuclidean:
		return (dx * dx + dy * dy) * factor * factor;
	case Heuristic::Euclidean:
		return std::sqrt(dx * dx + dy * dy) * factor;
	case Heuristic::Manhattan:
		return (std::fabs(dx) + std::fabs(dy)) * factor;
	default:
		return 0;
	}
}

//! One side of a bidirectional search.
struct Frontier {
	std::vector<double> distance;
	std::vector<uint64_t> parent;
	std::vector<int64_t> parent_edge;
	std::vector<double> parent_cost;
	std::vector<bool> settled;
	std::priority_queue<std::pair<double, uint64_t>, std::vector<std::pair<double, uint64_t>>,
	                    std::greater<std::pair<double, uint64_t>>>
	    queue;

	void Init(size_t n, uint64_t root) {
		distance.assign(n, std::numeric_limits<double>::infinity());
		parent.assign(n, 0);
		parent_edge.assign(n, -1);
		parent_cost.assign(n, 0);
		settled.assign(n, false);
		distance[root] = 0;
		parent[root] = root;
		queue.push(std::make_pair(0.0, root));
	}
};

//! Advances one frontier by a single vertex. Returns the vertex settled, or
//! false when that side is exhausted.
bool Step(Frontier &frontier, const Adjacency &adjacency, const std::vector<Placement> &placement, Heuristic heuristic,
          double factor, uint64_t goal, uint64_t &settled_vertex) {
	while (!frontier.queue.empty()) {
		const uint64_t at = frontier.queue.top().second;
		frontier.queue.pop();
		if (frontier.settled[at]) {
			continue;
		}
		frontier.settled[at] = true;
		settled_vertex = at;

		for (size_t i = 0; i < adjacency[at].size(); i++) {
			const Arc &arc = adjacency[at][i];
			const double candidate = frontier.distance[at] + arc.cost;
			if (candidate < frontier.distance[arc.to]) {
				frontier.distance[arc.to] = candidate;
				frontier.parent[arc.to] = at;
				frontier.parent_edge[arc.to] = arc.edge;
				frontier.parent_cost[arc.to] = arc.cost;
				// The heuristic only orders the queue; the stored distance
				// stays the true cost so the meeting test remains correct.
				frontier.queue.push(
				    std::make_pair(candidate + Estimate(placement, arc.to, goal, heuristic, factor), arc.to));
			}
		}
		return true;
	}
	return false;
}

//! Runs both frontiers until they meet, then stitches the two halves into one
//! path. `placement` and the heuristic are only consulted by the A* variants.
bool MeetInTheMiddle(const Adjacency &forward_adjacency, const Adjacency &backward_adjacency,
                     const std::vector<Placement> &placement, Heuristic heuristic, double factor, uint64_t source,
                     uint64_t sink, std::vector<uint64_t> &path, std::vector<int64_t> &edges,
                     std::vector<double> &costs, double &total) {
	const size_t n = forward_adjacency.size();
	if (source >= n || sink >= n) {
		return false;
	}
	if (source == sink) {
		return false;
	}

	Frontier forward, backward;
	forward.Init(n, source);
	backward.Init(n, sink);

	double best = std::numeric_limits<double>::infinity();
	uint64_t meeting = 0;
	bool found = false;

	while (true) {
		uint64_t settled = 0;
		bool advanced = false;

		// Alternate sides so neither runs far ahead of the other.
		if (Step(forward, forward_adjacency, placement, heuristic, factor, sink, settled)) {
			advanced = true;
			if (backward.distance[settled] < std::numeric_limits<double>::infinity()) {
				const double through = forward.distance[settled] + backward.distance[settled];
				if (through < best) {
					best = through;
					meeting = settled;
					found = true;
				}
			}
			if (forward.settled[settled] && backward.settled[settled]) {
				break;
			}
		}
		if (Step(backward, backward_adjacency, placement, heuristic, factor, source, settled)) {
			advanced = true;
			if (forward.distance[settled] < std::numeric_limits<double>::infinity()) {
				const double through = forward.distance[settled] + backward.distance[settled];
				if (through < best) {
					best = through;
					meeting = settled;
					found = true;
				}
			}
			if (forward.settled[settled] && backward.settled[settled]) {
				break;
			}
		}
		if (!advanced) {
			break;
		}
	}

	if (!found) {
		return false;
	}

	// Forward half: walk parents back from the meeting point to the source.
	std::vector<uint64_t> head;
	std::vector<int64_t> head_edges;
	std::vector<double> head_costs;
	for (uint64_t at = meeting; at != source; at = forward.parent[at]) {
		head.push_back(at);
		head_edges.push_back(forward.parent_edge[at]);
		head_costs.push_back(forward.parent_cost[at]);
	}
	head.push_back(source);
	std::reverse(head.begin(), head.end());
	std::reverse(head_edges.begin(), head_edges.end());
	std::reverse(head_costs.begin(), head_costs.end());

	path = head;
	edges = head_edges;
	costs = head_costs;

	// Backward half: parents already point towards the sink.
	for (uint64_t at = meeting; at != sink;) {
		const uint64_t next = backward.parent[at];
		edges.push_back(backward.parent_edge[at]);
		costs.push_back(backward.parent_cost[at]);
		path.push_back(next);
		at = next;
	}

	total = best;
	return true;
}

std::vector<int64_t> Normalized(const std::vector<int64_t> &values) {
	std::vector<int64_t> result(values);
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

std::vector<PathRow> ToPathRows(const std::vector<uint64_t> &path, const std::vector<int64_t> &edges,
                                const std::vector<double> &costs, const VertexIndex &index, int64_t start_vid,
                                int64_t end_vid) {
	std::vector<PathRow> rows;
	double agg_cost = 0;
	for (size_t i = 0; i < path.size(); i++) {
		PathRow row {};
		row.path_seq = static_cast<int64_t>(i) + 1;
		row.start_vid = start_vid;
		row.end_vid = end_vid;
		row.node = index.IdOf(path[i]);
		row.agg_cost = DecodeInfinity(agg_cost);
		if (i < edges.size()) {
			row.edge = edges[i];
			row.cost = DecodeInfinity(costs[i]);
			agg_cost += costs[i];
		} else {
			row.edge = -1;
			row.cost = 0;
		}
		rows.push_back(row);
	}
	return rows;
}

//! Shared driver for both bidirectional variants.
void RunBidirectional(const std::vector<EdgeRow> &edges, const std::vector<Placement> &placement,
                      const std::vector<int64_t> &starts, const std::vector<int64_t> &ends, bool directed,
                      Heuristic heuristic, double factor, std::vector<PathRow> *paths,
                      std::vector<CostRow> *costs_out) {
	VertexIndex index;
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(), [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });
	const Adjacency forward = BuildAdjacency(ordered, index, directed);
	const Adjacency backward = Reverse(forward);

	std::vector<Placement> placed(placement);
	placed.resize(index.Size());

	for (size_t s = 0; s < starts.size(); s++) {
		uint64_t source = 0;
		if (!index.Find(starts[s], source)) {
			continue;
		}
		for (size_t e = 0; e < ends.size(); e++) {
			if (ends[e] == starts[s]) {
				continue;
			}
			uint64_t sink = 0;
			if (!index.Find(ends[e], sink)) {
				continue;
			}
			std::vector<uint64_t> path;
			std::vector<int64_t> path_edges;
			std::vector<double> path_costs;
			double total = 0;
			if (!MeetInTheMiddle(forward, backward, placed, heuristic, factor, source, sink, path, path_edges,
			                     path_costs, total)) {
				continue;
			}
			if (costs_out) {
				costs_out->push_back(CostRow {starts[s], ends[e], DecodeInfinity(total)});
			} else {
				auto rows = ToPathRows(path, path_edges, path_costs, index, starts[s], ends[e]);
				paths->insert(paths->end(), rows.begin(), rows.end());
			}
		}
	}
}

//! Places vertices from an A* edges query.
std::vector<Placement> PlacementsFrom(const std::vector<CoordinateEdgeRow> &edges, std::vector<EdgeRow> &plain) {
	VertexIndex index;
	for (size_t i = 0; i < edges.size(); i++) {
		index.GetOrCreate(edges[i].edge.source);
		index.GetOrCreate(edges[i].edge.target);
	}
	std::vector<Placement> placement(index.Size());
	std::vector<bool> placed(index.Size(), false);
	plain.clear();
	for (size_t i = 0; i < edges.size(); i++) {
		uint64_t source = 0;
		uint64_t target = 0;
		index.Find(edges[i].edge.source, source);
		index.Find(edges[i].edge.target, target);
		if (!placed[source]) {
			placement[source].x = edges[i].x1;
			placement[source].y = edges[i].y1;
			placed[source] = true;
		}
		if (!placed[target]) {
			placement[target].x = edges[i].x2;
			placement[target].y = edges[i].y2;
			placed[target] = true;
		}
		plain.push_back(edges[i].edge);
	}
	return placement;
}

} // namespace

std::vector<PathRow> BidirectionalDijkstra(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                           const std::vector<int64_t> &ends, bool directed) {
	std::vector<PathRow> rows;
	RunBidirectional(edges, std::vector<Placement>(), Normalized(starts), Normalized(ends), directed, Heuristic::None,
	                 1.0, &rows, nullptr);
	return rows;
}

std::vector<CostRow> BidirectionalDijkstraCost(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                               const std::vector<int64_t> &ends, bool directed) {
	std::vector<CostRow> rows;
	RunBidirectional(edges, std::vector<Placement>(), Normalized(starts), Normalized(ends), directed, Heuristic::None,
	                 1.0, nullptr, &rows);
	return rows;
}

std::vector<PathRow> BidirectionalAStar(const std::vector<CoordinateEdgeRow> &edges, const std::vector<int64_t> &starts,
                                        const std::vector<int64_t> &ends, const AStarOptions &options) {
	std::vector<EdgeRow> plain;
	const auto placement = PlacementsFrom(edges, plain);
	std::vector<PathRow> rows;
	RunBidirectional(plain, placement, Normalized(starts), Normalized(ends), options.directed, options.heuristic,
	                 options.factor * options.epsilon, &rows, nullptr);
	return rows;
}

std::vector<CostRow> BidirectionalAStarCost(const std::vector<CoordinateEdgeRow> &edges,
                                            const std::vector<int64_t> &starts, const std::vector<int64_t> &ends,
                                            const AStarOptions &options) {
	std::vector<EdgeRow> plain;
	const auto placement = PlacementsFrom(edges, plain);
	std::vector<CostRow> rows;
	RunBidirectional(plain, placement, Normalized(starts), Normalized(ends), options.directed, options.heuristic,
	                 options.factor * options.epsilon, nullptr, &rows);
	return rows;
}

} // namespace duckrouting
