#pragma once

#include "duckdb.hpp"
#include "duckrouting/dijkstra.hpp"
#include "duckrouting/edges.hpp"

#include <cstdint>
#include <vector>

namespace duckrouting {

//! A component membership row. `component` is the smallest identifier in the
//! component, which is how pgRouting labels them; `member` is a node id for
//! vertex-based components and an edge id for biconnected components.
struct ComponentRow {
	int64_t component;
	int64_t member;
};

//! A single identifier: a node for articulationPoints, an edge for bridges.
struct IdentifierRow {
	int64_t id;
};

//! A vertex pair, as returned by makeConnected.
struct PairRow {
	int64_t start_vid;
	int64_t end_vid;
};

//! All-pairs shortest path costs. Self pairs and unreachable pairs are omitted,
//! matching pgRouting. Results are ordered by start_vid then end_vid.
std::vector<CostRow> FloydWarshall(const std::vector<EdgeRow> &edges, bool directed);
std::vector<CostRow> Johnson(const std::vector<EdgeRow> &edges, bool directed);

//! Connected components of the undirected graph, ordered by component then node.
std::vector<ComponentRow> ConnectedComponents(const std::vector<EdgeRow> &edges);

//! Strongly connected components of the directed graph.
std::vector<ComponentRow> StrongComponents(const std::vector<EdgeRow> &edges);

//! Biconnected components, labelled by their smallest edge id and listing edges.
std::vector<ComponentRow> BiconnectedComponents(const std::vector<EdgeRow> &edges);

//! Vertices whose removal disconnects the graph.
std::vector<IdentifierRow> ArticulationPoints(const std::vector<EdgeRow> &edges);

//! Edges whose removal disconnects the graph -- the biconnected components that
//! consist of a single edge.
std::vector<IdentifierRow> Bridges(const std::vector<EdgeRow> &edges);

//! The vertex pairs Boost would join to make the graph connected.
std::vector<PairRow> MakeConnected(const std::vector<EdgeRow> &edges);

//! One edge of a minimum spanning forest.
struct SpanningEdgeRow {
	int64_t edge;
	double cost;
};

//! How the spanning tree is walked once it has been built.
enum class Traversal {
	Bfs,       //!< breadth first; depth is non-decreasing
	DfsDepth,  //!< depth first, cut off by hop count
	DfsCost    //!< depth first, cut off by accumulated cost (the DD variants)
};

//! Minimum spanning forest of the undirected graph, by edge id.
std::vector<SpanningEdgeRow> Kruskal(const std::vector<EdgeRow> &edges);
std::vector<SpanningEdgeRow> Prim(const std::vector<EdgeRow> &edges);

//! Walks the spanning forest from each root. `limit` is a maximum depth for
//! Bfs/DfsDepth and a maximum accumulated cost for DfsCost.
std::vector<DrivingDistanceRow> SpanningTraversal(const std::vector<EdgeRow> &edges, bool use_prim,
                                                  const std::vector<int64_t> &roots, Traversal traversal,
                                                  double limit);

duckdb::TableFunctionSet GetKruskalFunction();
duckdb::TableFunctionSet GetPrimFunction();
duckdb::TableFunctionSet GetKruskalBFSFunction();
duckdb::TableFunctionSet GetKruskalDFSFunction();
duckdb::TableFunctionSet GetKruskalDDFunction();
duckdb::TableFunctionSet GetPrimBFSFunction();
duckdb::TableFunctionSet GetPrimDFSFunction();
duckdb::TableFunctionSet GetPrimDDFunction();

duckdb::TableFunctionSet GetFloydWarshallFunction();
duckdb::TableFunctionSet GetJohnsonFunction();
duckdb::TableFunctionSet GetConnectedComponentsFunction();
duckdb::TableFunctionSet GetStrongComponentsFunction();
duckdb::TableFunctionSet GetBiconnectedComponentsFunction();
duckdb::TableFunctionSet GetArticulationPointsFunction();
duckdb::TableFunctionSet GetBridgesFunction();
duckdb::TableFunctionSet GetMakeConnectedFunction();

} // namespace duckrouting
