#pragma once

#include "duckdb.hpp"
#include "duckrouting/dijkstra.hpp"
#include "duckrouting/edges.hpp"
#include "duckrouting/yen.hpp"

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

//! Walks `adjacency` from each root, emitting pgRouting's drivingDistance
//! shape. Shared by the spanning-tree traversals and by breadth/depth first
//! search over the whole graph -- the only difference is which adjacency is
//! handed in.
std::vector<DrivingDistanceRow> WalkGraph(const Adjacency &adjacency, const VertexIndex &index,
                                          const std::vector<int64_t> &roots, Traversal traversal, double limit);

//! Breadth or depth first search over the whole graph.
std::vector<DrivingDistanceRow> GraphTraversal(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &roots,
                                               Traversal traversal, double limit, bool directed);

//! Shortest path via Bellman-Ford, which tolerates negative edge weights.
std::vector<PathRow> BellmanFord(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                 const std::vector<int64_t> &ends, bool directed);

//! Shortest path over a directed acyclic graph. Throws when the graph cycles.
std::vector<PathRow> DagShortestPath(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                     const std::vector<int64_t> &ends);

//! Every vertex reachable from each vertex.
struct ClosureRow {
	int64_t node;
	std::vector<int64_t> targets;
};
std::vector<ClosureRow> TransitiveClosure(const std::vector<EdgeRow> &edges);

//! Vertex orderings. `node` is the vertex, `seq` its position.
enum class Ordering { CuthillMckee, King, Sloan, Topological };
std::vector<IdentifierRow> VertexOrdering(const std::vector<EdgeRow> &edges, Ordering ordering);

//! Minimum spanning forest of the undirected graph, by edge id.
std::vector<SpanningEdgeRow> Kruskal(const std::vector<EdgeRow> &edges);
std::vector<SpanningEdgeRow> Prim(const std::vector<EdgeRow> &edges);

//! Walks the spanning forest from each root. `limit` is a maximum depth for
//! Bfs/DfsDepth and a maximum accumulated cost for DfsCost.
std::vector<DrivingDistanceRow> SpanningTraversal(const std::vector<EdgeRow> &edges, bool use_prim,
                                                  const std::vector<int64_t> &roots, Traversal traversal,
                                                  double limit);

//! An identifier paired with a colour class.
struct ColorRow {
	int64_t id;
	int64_t color;
};

//! Betweenness centrality per vertex.
struct CentralityRow {
	int64_t vid;
	double centrality;
};

//! One edge crossing the minimum cut.
struct MinCutRow {
	int64_t edge;
	double cost;
	double mincut;
};

//! One step of a circuit; `path_seq` starts at 0, as pgRouting's does.
struct CircuitRow {
	int64_t path_id;
	int64_t path_seq;
	int64_t start_vid;
	int64_t end_vid;
	int64_t node;
	int64_t edge;
	double cost;
	double agg_cost;
};

//! A vertex and its immediate dominator; 0 when it has none.
struct DominatorRow {
	int64_t vertex_id;
	int64_t idom;
};

//! Which colour class each vertex or edge falls into. Colours are 1-based for
//! the two colouring functions and 0/1 for the bipartite partition, matching
//! pgRouting.
std::vector<ColorRow> SequentialVertexColoring(const std::vector<EdgeRow> &edges);
std::vector<ColorRow> EdgeColoring(const std::vector<EdgeRow> &edges);
//! Empty when the graph is not bipartite, which is how pgRouting reports it.
std::vector<ColorRow> Bipartite(const std::vector<EdgeRow> &edges);

//! Whether the graph can be drawn without crossing edges.
bool IsPlanar(const std::vector<EdgeRow> &edges);
//! The planar embedding, as the edges around each vertex in rotation order.
std::vector<PairRow> BoyerMyrvold(const std::vector<EdgeRow> &edges);

//! Largest difference between the indices of two adjacent vertices.
int64_t Bandwidth(const std::vector<EdgeRow> &edges);
//! Relative betweenness centrality per vertex.
std::vector<CentralityRow> BetweennessCentrality(const std::vector<EdgeRow> &edges, bool directed);

//! The edges crossing a minimum cut of the undirected graph.
std::vector<MinCutRow> StoerWagner(const std::vector<EdgeRow> &edges);

//! Every circuit (closed path) in the directed graph.
std::vector<CircuitRow> HawickCircuits(const std::vector<EdgeRow> &edges);

//! Immediate dominator of each vertex, relative to `root`.
std::vector<DominatorRow> DominatorTree(const std::vector<EdgeRow> &edges, int64_t root);

//! One edge carrying flow.
struct FlowRow {
	int64_t edge;
	int64_t start_vid;
	int64_t end_vid;
	double flow;
	double residual_capacity;
	double cost;
	double agg_cost;
};

//! Which max-flow algorithm to run. They compute the same maximum, but not
//! necessarily the same distribution of flow across the edges.
enum class FlowAlgorithm { PushRelabel, EdmondsKarp, BoykovKolmogorov, MinCost };

//! Maximum flow from `sources` to `sinks`, edge by edge. Only edges actually
//! carrying flow are reported.
std::vector<FlowRow> MaxFlow(const std::vector<FlowEdgeRow> &edges, const std::vector<int64_t> &sources,
                             const std::vector<int64_t> &sinks, FlowAlgorithm algorithm);

//! The total flow value alone.
double MaxFlowValue(const std::vector<FlowEdgeRow> &edges, const std::vector<int64_t> &sources,
                    const std::vector<int64_t> &sinks, FlowAlgorithm algorithm);

//! Edge-disjoint paths between the given vertices, found by running a unit
//! capacity flow and decomposing it back into paths.
std::vector<PathRow> EdgeDisjointPaths(const std::vector<FlowEdgeRow> &edges, const std::vector<int64_t> &starts,
                                       const std::vector<int64_t> &ends, bool directed);

//! A maximum matching: as many edges as possible, no two sharing a vertex.
std::vector<IdentifierRow> MaxCardinalityMatch(const std::vector<FlowEdgeRow> &edges);

//! How A* estimates the remaining distance to a goal. These are pgRouting's
//! numbering; 0 makes A* behave exactly like Dijkstra.
enum class Heuristic {
	None = 0,      //!< 0
	MaxDelta = 1,  //!< |max(dx, dy)|
	MinDelta = 2,  //!< |min(dx, dy)|
	SquaredEuclidean = 3,
	Euclidean = 4,
	Manhattan = 5 //!< |dx| + |dy|, the default
};

//! Options shared by the three A* functions.
struct AStarOptions {
	bool directed = true;
	Heuristic heuristic = Heuristic::Manhattan;
	double factor = 1.0;
	double epsilon = 1.0;
};

//! Shortest paths found with A*. Same result as Dijkstra given an admissible
//! heuristic; the heuristic only changes how much of the graph is explored.
std::vector<PathRow> AStar(const std::vector<CoordinateEdgeRow> &edges, const std::vector<int64_t> &starts,
                           const std::vector<int64_t> &ends, const AStarOptions &options);

//! Total cost per reachable pair.
std::vector<CostRow> AStarCost(const std::vector<CoordinateEdgeRow> &edges, const std::vector<int64_t> &starts,
                               const std::vector<int64_t> &ends, const AStarOptions &options);

duckdb::TableFunctionSet GetAStarFunction();
duckdb::TableFunctionSet GetAStarCostFunction();
duckdb::TableFunctionSet GetAStarCostMatrixFunction();

duckdb::TableFunctionSet GetMaxFlowFunction();
duckdb::TableFunctionSet GetPushRelabelFunction();
duckdb::TableFunctionSet GetEdmondsKarpFunction();
duckdb::TableFunctionSet GetBoykovKolmogorovFunction();
duckdb::TableFunctionSet GetMaxFlowMinCostFunction();
duckdb::TableFunctionSet GetMaxFlowMinCostCostFunction();
duckdb::TableFunctionSet GetEdgeDisjointPathsFunction();
duckdb::TableFunctionSet GetMaxCardinalityMatchFunction();

duckdb::TableFunctionSet GetSequentialVertexColoringFunction();
duckdb::TableFunctionSet GetEdgeColoringFunction();
duckdb::TableFunctionSet GetBipartiteFunction();
duckdb::TableFunctionSet GetIsPlanarFunction();
duckdb::TableFunctionSet GetBoyerMyrvoldFunction();
duckdb::TableFunctionSet GetBandwidthFunction();
duckdb::TableFunctionSet GetBetweennessCentralityFunction();
duckdb::TableFunctionSet GetStoerWagnerFunction();
duckdb::TableFunctionSet GetHawickCircuitsFunction();
duckdb::TableFunctionSet GetDominatorTreeFunction();

duckdb::TableFunctionSet GetBreadthFirstSearchFunction();
duckdb::TableFunctionSet GetDepthFirstSearchFunction();
duckdb::TableFunctionSet GetBellmanFordFunction();
duckdb::TableFunctionSet GetDagShortestPathFunction();
duckdb::TableFunctionSet GetTransitiveClosureFunction();
duckdb::TableFunctionSet GetCuthillMckeeOrderingFunction();
duckdb::TableFunctionSet GetKingOrderingFunction();
duckdb::TableFunctionSet GetSloanOrderingFunction();
duckdb::TableFunctionSet GetTopologicalSortFunction();

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
