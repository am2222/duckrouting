#include "duckrouting/yen.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>

namespace duckrouting {

Adjacency BuildAdjacency(const std::vector<EdgeRow> &edges, VertexIndex &index, bool directed) {
	for (size_t i = 0; i < edges.size(); i++) {
		index.GetOrCreate(edges[i].source);
		index.GetOrCreate(edges[i].target);
	}

	Adjacency adjacency(index.Size());
	for (size_t i = 0; i < edges.size(); i++) {
		const EdgeRow &edge = edges[i];
		uint64_t source = 0;
		uint64_t target = 0;
		index.Find(edge.source, source);
		index.Find(edge.target, target);
		if (IsTraversable(edge.cost)) {
			adjacency[source].push_back(Arc {target, edge.id, edge.cost});
			if (!directed) {
				adjacency[target].push_back(Arc {source, edge.id, edge.cost});
			}
		}
		if (IsTraversable(edge.reverse_cost)) {
			adjacency[target].push_back(Arc {source, edge.id, edge.reverse_cost});
			if (!directed) {
				adjacency[source].push_back(Arc {target, edge.id, edge.reverse_cost});
			}
		}
	}
	return adjacency;
}

bool ConstrainedShortestPath(const Adjacency &adjacency, uint64_t source, uint64_t sink,
                             const std::set<uint64_t> &banned_vertices, const std::set<ArcKey> &banned_arcs,
                             SimplePath &result) {
	if (source >= adjacency.size() || sink >= adjacency.size()) {
		return false;
	}
	if (banned_vertices.count(source) || banned_vertices.count(sink)) {
		return false;
	}

	const double unreachable = std::numeric_limits<double>::infinity();
	std::vector<double> distance(adjacency.size(), unreachable);
	std::vector<uint64_t> parent(adjacency.size(), 0);
	std::vector<int64_t> parent_edge(adjacency.size(), -1);
	std::vector<double> parent_cost(adjacency.size(), 0);
	std::vector<bool> settled(adjacency.size(), false);

	typedef std::pair<double, uint64_t> Entry;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry> > queue;
	distance[source] = 0;
	parent[source] = source;
	queue.push(Entry(0, source));

	while (!queue.empty()) {
		const uint64_t current = queue.top().second;
		queue.pop();
		if (settled[current]) {
			continue;
		}
		settled[current] = true;
		if (current == sink) {
			break;
		}
		for (size_t i = 0; i < adjacency[current].size(); i++) {
			const Arc &arc = adjacency[current][i];
			if (banned_vertices.count(arc.to) || banned_arcs.count(ArcKey(current, arc.edge))) {
				continue;
			}
			const double candidate = distance[current] + arc.cost;
			if (candidate < distance[arc.to]) {
				distance[arc.to] = candidate;
				parent[arc.to] = current;
				parent_edge[arc.to] = arc.edge;
				parent_cost[arc.to] = arc.cost;
				queue.push(Entry(candidate, arc.to));
			}
		}
	}

	if (distance[sink] == unreachable) {
		return false;
	}

	// Walk back to the source, then flip.
	std::vector<uint64_t> vertices;
	for (uint64_t at = sink;; at = parent[at]) {
		vertices.push_back(at);
		if (at == source) {
			break;
		}
	}
	std::reverse(vertices.begin(), vertices.end());

	result.steps.clear();
	result.cost = distance[sink];
	for (size_t i = 0; i < vertices.size(); i++) {
		PathStep step {};
		step.vertex = vertices[i];
		if (i + 1 < vertices.size()) {
			step.edge = parent_edge[vertices[i + 1]];
			step.cost = parent_cost[vertices[i + 1]];
		} else {
			step.edge = -1;
			step.cost = 0;
		}
		result.steps.push_back(step);
	}
	return true;
}

namespace {

bool SamePath(const SimplePath &a, const SimplePath &b) {
	return a.Vertices() == b.Vertices();
}

bool Contains(const std::vector<SimplePath> &paths, const SimplePath &path) {
	for (size_t i = 0; i < paths.size(); i++) {
		if (SamePath(paths[i], path)) {
			return true;
		}
	}
	return false;
}

} // namespace

void YenKShortestPaths(const Adjacency &adjacency, uint64_t source, uint64_t sink, int64_t k,
                       std::vector<SimplePath> &selected, std::vector<SimplePath> &rejected) {
	selected.clear();
	rejected.clear();
	if (k <= 0 || source == sink) {
		return;
	}

	SimplePath shortest;
	if (!ConstrainedShortestPath(adjacency, source, sink, std::set<uint64_t>(), std::set<ArcKey>(), shortest)) {
		return;
	}
	selected.push_back(shortest);

	std::vector<SimplePath> candidates;
	while (static_cast<int64_t>(selected.size()) < k) {
		const SimplePath &previous = selected.back();

		// Every node of the previous path except its last is a possible place
		// to branch away from it.
		for (size_t i = 0; i + 1 < previous.steps.size(); i++) {
			const uint64_t spur = previous.steps[i].vertex;

			std::set<ArcKey> banned_arcs;
			std::set<uint64_t> banned_vertices;

			// Withdraw the arc each already-known path used to leave the spur,
			// so the search cannot simply reproduce one of them.
			for (size_t p = 0; p < selected.size(); p++) {
				const SimplePath &known = selected[p];
				if (known.steps.size() <= i + 1) {
					continue;
				}
				bool shares_root = true;
				for (size_t j = 0; j <= i; j++) {
					if (known.steps[j].vertex != previous.steps[j].vertex) {
						shares_root = false;
						break;
					}
				}
				if (shares_root) {
					banned_arcs.insert(ArcKey(known.steps[i].vertex, known.steps[i].edge));
				}
			}

			// Withdraw the root path's own nodes, which keeps the result loopless.
			double root_cost = 0;
			for (size_t j = 0; j < i; j++) {
				banned_vertices.insert(previous.steps[j].vertex);
				root_cost += previous.steps[j].cost;
			}

			SimplePath spur_path;
			if (!ConstrainedShortestPath(adjacency, spur, sink, banned_vertices, banned_arcs, spur_path)) {
				continue;
			}

			SimplePath total;
			total.steps.assign(previous.steps.begin(), previous.steps.begin() + static_cast<long>(i));
			total.steps.insert(total.steps.end(), spur_path.steps.begin(), spur_path.steps.end());
			total.cost = root_cost + spur_path.cost;

			if (!Contains(selected, total) && !Contains(candidates, total)) {
				candidates.push_back(total);
			}
		}

		if (candidates.empty()) {
			break;
		}

		// Take the cheapest candidate; ties keep discovery order.
		size_t best = 0;
		for (size_t i = 1; i < candidates.size(); i++) {
			if (candidates[i].cost < candidates[best].cost) {
				best = i;
			}
		}
		selected.push_back(candidates[best]);
		candidates.erase(candidates.begin() + static_cast<long>(best));
	}

	// Whatever is left over is what pgRouting reports as the heap paths.
	std::stable_sort(candidates.begin(), candidates.end(),
	                 [](const SimplePath &a, const SimplePath &b) { return a.cost < b.cost; });
	rejected = candidates;
}

} // namespace duckrouting
