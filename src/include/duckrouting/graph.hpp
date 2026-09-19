#pragma once

#include "duckrouting/edges.hpp"

#include <boost/graph/adjacency_list.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace duckrouting {

//! Bundled edge property: the original edge id plus the weight used for this
//! direction. Parallel edges are kept rather than collapsed, so the edge id
//! reported back to the user is always one that really exists in `edges_sql`.
struct RoutingEdge {
	int64_t id;
	double cost;
};

using DirectedGraph =
    boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, boost::no_property, RoutingEdge>;
using UndirectedGraph =
    boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS, boost::no_property, RoutingEdge>;

//! Maps the user's arbitrary BIGINT vertex ids onto the contiguous descriptors
//! Boost needs, and back again for output.
class VertexIndex {
public:
	//! Descriptor for `id`, allocating one if this is the first sighting.
	uint64_t GetOrCreate(int64_t id) {
		auto entry = lookup.find(id);
		if (entry != lookup.end()) {
			return entry->second;
		}
		uint64_t descriptor = ids.size();
		lookup.emplace(id, descriptor);
		ids.push_back(id);
		return descriptor;
	}

	//! Descriptor for `id`, or false when the vertex never appeared in the graph.
	bool Find(int64_t id, uint64_t &result) const {
		auto entry = lookup.find(id);
		if (entry == lookup.end()) {
			return false;
		}
		result = entry->second;
		return true;
	}

	int64_t IdOf(uint64_t descriptor) const {
		return ids[descriptor];
	}

	uint64_t Size() const {
		return ids.size();
	}

private:
	std::unordered_map<int64_t, uint64_t> lookup;
	std::vector<int64_t> ids;
};

//! Builds a Boost graph from `edges`, honouring pgRouting's cost conventions:
//! `cost` drives source->target, `reverse_cost` drives target->source, and a
//! negative value in either suppresses that direction.
template <typename Graph>
Graph BuildGraph(const std::vector<EdgeRow> &edges, VertexIndex &index) {
	// Register every endpoint first so vertex descriptors stay dense.
	for (auto &edge : edges) {
		index.GetOrCreate(edge.source);
		index.GetOrCreate(edge.target);
	}

	Graph graph(index.Size());
	for (auto &edge : edges) {
		auto source = index.GetOrCreate(edge.source);
		auto target = index.GetOrCreate(edge.target);
		if (IsTraversable(edge.cost)) {
			boost::add_edge(source, target, RoutingEdge {edge.id, edge.cost}, graph);
		}
		if (IsTraversable(edge.reverse_cost)) {
			boost::add_edge(target, source, RoutingEdge {edge.id, edge.reverse_cost}, graph);
		}
	}
	return graph;
}

} // namespace duckrouting
