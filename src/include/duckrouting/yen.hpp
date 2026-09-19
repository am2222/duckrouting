#pragma once

#include "duckrouting/edges.hpp"
#include "duckrouting/graph.hpp"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace duckrouting {

//! One step of a path: the node, and the edge taken to leave it. The final step
//! of a path carries edge -1 and cost 0, matching pgRouting's output.
struct PathStep {
	uint64_t vertex;
	int64_t edge;
	double cost;
};

//! A loopless path together with its total cost.
struct SimplePath {
	std::vector<PathStep> steps;
	double cost = 0;

	//! Node sequence, used to compare paths for equality.
	std::vector<uint64_t> Vertices() const {
		std::vector<uint64_t> result;
		result.reserve(steps.size());
		for (size_t i = 0; i < steps.size(); i++) {
			result.push_back(steps[i].vertex);
		}
		return result;
	}
};

//! A plain adjacency list. Yen's algorithm repeatedly solves shortest paths on
//! a graph with some nodes and arcs withdrawn; expressing that directly is far
//! simpler than layering filtered views over a Boost graph, and Boost has no
//! k-shortest-paths algorithm to build on in any case.
struct Arc {
	uint64_t to;
	int64_t edge;
	double cost;
};

using Adjacency = std::vector<std::vector<Arc>>;

//! An arc is identified by where it leaves from plus its edge id, because in an
//! undirected graph the same edge id appears in both directions.
using ArcKey = std::pair<uint64_t, int64_t>;

//! Builds the adjacency list using the same cost conventions as BuildGraph.
Adjacency BuildAdjacency(const std::vector<EdgeRow> &edges, VertexIndex &index, bool directed);

//! Shortest path from `source` to `sink`, ignoring any vertex in
//! `banned_vertices` and any arc in `banned_arcs`. Returns false when no such
//! path exists.
bool ConstrainedShortestPath(const Adjacency &adjacency, uint64_t source, uint64_t sink,
                             const std::set<uint64_t> &banned_vertices, const std::set<ArcKey> &banned_arcs,
                             SimplePath &result);

//! Yen's algorithm. `selected` receives up to `k` paths in increasing cost
//! order; `rejected` receives the candidates that were generated but never
//! selected, which is what pgRouting exposes as heap_paths.
void YenKShortestPaths(const Adjacency &adjacency, uint64_t source, uint64_t sink, int64_t k,
                       std::vector<SimplePath> &selected, std::vector<SimplePath> &rejected);

} // namespace duckrouting
