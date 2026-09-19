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

//! One stop on a travelling-salesman tour. `cost` is the cost of *arriving*
//! here from the previous stop, which is the opposite convention from the
//! path functions, and is what pgRouting reports.
struct TourRow {
	int64_t node;
	double cost;
	double agg_cost;
};

//! An approximate shortest tour visiting every vertex once and returning to the
//! start, from a cost matrix. Uses Boost's metric_tsp_approx, which guarantees
//! a tour at most twice the optimum when the costs obey the triangle
//! inequality.
std::vector<TourRow> Tsp(const std::vector<MatrixCell> &matrix, int64_t start_id, int64_t end_id);

//! The same, with distances computed from coordinates instead.
std::vector<TourRow> TspEuclidean(const std::vector<PlacedPoint> &points, int64_t start_id, int64_t end_id);

//! One row of the contraction-hierarchies result: either a vertex ("v") with
//! its contraction order, or a shortcut edge ("e") with the vertices it
//! bypasses.
struct ContractionRow {
	bool is_vertex;
	int64_t id;
	std::vector<int64_t> contracted_vertices;
	int64_t source;
	int64_t target;
	double cost;
	int64_t metric;
	int64_t vertex_order;
};

//! Contracts the graph, replacing each removed vertex with shortcut edges that
//! preserve shortest-path distances. Vertices listed in `forbidden` are never
//! contracted.
std::vector<ContractionRow> ContractionHierarchies(const std::vector<EdgeRow> &edges, bool directed,
                                                   const std::vector<int64_t> &forbidden);

//! Driving distance from a point partway along an edge. Points become vertices
//! with id -pid. With `details` the point vertices appear in the result;
//! without it only real vertices do.
std::vector<DrivingDistanceRow> WithPointsDrivingDistance(const std::vector<EdgeRow> &edges,
                                                          const std::vector<PointOnEdge> &points,
                                                          const std::vector<int64_t> &starts, double distance,
                                                          char driving_side, bool directed, bool details);

//! A vertex with the edges entering and leaving it.
struct VertexEdgesRow {
	int64_t id;
	std::vector<int64_t> in_edges;
	std::vector<int64_t> out_edges;
};

//! A vertex and how many edges touch it.
struct DegreeRow {
	int64_t node;
	int64_t degree;
};

//! Derives the vertex table implied by an edges query.
std::vector<VertexEdgesRow> ExtractVertices(const std::vector<EdgeRow> &edges);

//! Counts the edges incident on each vertex.
std::vector<DegreeRow> Degree(const std::vector<EdgeRow> &edges);

//! Bidirectional Dijkstra: searches forward from the start and backward from
//! the end at the same time, stopping when the two frontiers meet. Same answer
//! as Dijkstra, reached by exploring less of the graph. Not a Boost algorithm.
std::vector<PathRow> BidirectionalDijkstra(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                           const std::vector<int64_t> &ends, bool directed);
std::vector<CostRow> BidirectionalDijkstraCost(const std::vector<EdgeRow> &edges,
                                               const std::vector<int64_t> &starts,
                                               const std::vector<int64_t> &ends, bool directed);

//! The same, with each frontier guided by a heuristic.
std::vector<PathRow> BidirectionalAStar(const std::vector<CoordinateEdgeRow> &edges,
                                        const std::vector<int64_t> &starts, const std::vector<int64_t> &ends,
                                        const AStarOptions &options);
std::vector<CostRow> BidirectionalAStarCost(const std::vector<CoordinateEdgeRow> &edges,
                                            const std::vector<int64_t> &starts,
                                            const std::vector<int64_t> &ends, const AStarOptions &options);

//! Edward Moore's shortest path, better known as SPFA: a queue-based
//! Bellman-Ford refinement. Not a Boost algorithm.
std::vector<PathRow> EdwardMoore(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                 const std::vector<int64_t> &ends, bool directed);

//! 0-1 BFS: a deque-based shortest path for graphs whose edges cost 0 or 1.
std::vector<PathRow> BinaryBreadthFirstSearch(const std::vector<EdgeRow> &edges,
                                              const std::vector<int64_t> &starts,
                                              const std::vector<int64_t> &ends, bool directed);

duckdb::TableFunctionSet GetExtractVerticesFunction();
duckdb::TableFunctionSet GetDegreeFunction();
duckdb::TableFunctionSet GetBdDijkstraFunction();
duckdb::TableFunctionSet GetBdDijkstraCostFunction();
duckdb::TableFunctionSet GetBdDijkstraCostMatrixFunction();
duckdb::TableFunctionSet GetBdAStarFunction();
duckdb::TableFunctionSet GetBdAStarCostFunction();
duckdb::TableFunctionSet GetBdAStarCostMatrixFunction();
duckdb::TableFunctionSet GetEdwardMooreFunction();
duckdb::TableFunctionSet GetBinaryBreadthFirstSearchFunction();
duckdb::TableFunctionSet GetFullVersionFunction();

//! Replaces each edge carrying points with the chain of segments between
//! them. A point becomes a vertex numbered -pid; which direction it connects
//! to depends on the driving side. Shared by the whole withPoints family.
std::vector<EdgeRow> SplitEdgesAtPoints(const std::vector<EdgeRow> &edges, const std::vector<PointOnEdge> &points,
                                        char driving_side);

//! Drops point vertices that the caller did not ask about, which is what
//! `details => false` means.
std::vector<PathRow> HidePoints(const std::vector<PathRow> &rows, const std::vector<int64_t> &visible);

//! Which contraction operators to apply, in order.
enum class ContractionMethod { DeadEnd = 1, Linear = 2 };

//! Simplifies the graph by absorbing dead ends and collapsing chains.
//! `cycles` is how many times to run the chosen methods round, since each pass
//! can expose new opportunities for the other.
std::vector<ContractionRow> Contract(const std::vector<EdgeRow> &edges, bool directed,
                                     const std::vector<ContractionMethod> &methods, int64_t cycles,
                                     const std::vector<int64_t> &forbidden);

//! Registers the three geometry helpers, which are SQL macros over DuckDB's
//! spatial extension rather than C++ functions.
//! One edge of a transformed graph.
struct TransformedEdgeRow {
	int64_t source;
	int64_t target;
	double cost;
	double reverse_cost;
	int64_t edge;
};

//! The line graph: each edge of the input becomes a vertex, and two such
//! vertices are joined when the edges they stand for share an endpoint.
std::vector<TransformedEdgeRow> LineGraph(const std::vector<EdgeRow> &edges, bool directed);

//! The full line graph, which additionally splits each vertex into one node
//! per incident half-edge so that turn costs can be attached.
std::vector<TransformedEdgeRow> LineGraphFull(const std::vector<EdgeRow> &edges);

//! A closed walk traversing every edge at least once, and its total cost.
std::vector<PathRow> ChinesePostman(const std::vector<EdgeRow> &edges, bool directed);
double ChinesePostmanCost(const std::vector<EdgeRow> &edges, bool directed);

duckdb::TableFunctionSet GetLineGraphFunction();
duckdb::TableFunctionSet GetLineGraphFullFunction();
duckdb::TableFunctionSet GetChinesePostmanFunction();
duckdb::TableFunctionSet GetChinesePostmanCostFunction();

void RegisterGeometryMacros(duckdb::ExtensionLoader &loader);

duckdb::TableFunctionSet GetContractionFunction();
duckdb::TableFunctionSet GetDeadEndContractionFunction();
duckdb::TableFunctionSet GetLinearContractionFunction();

duckdb::TableFunctionSet GetWithPointsFunction();
duckdb::TableFunctionSet GetWithPointsCostFunction();
duckdb::TableFunctionSet GetWithPointsCostMatrixFunction();
duckdb::TableFunctionSet GetWithPointsViaFunction();
duckdb::TableFunctionSet GetWithPointsKspFunction();

duckdb::TableFunctionSet GetContractionHierarchiesFunction();
duckdb::TableFunctionSet GetWithPointsDDFunction();

duckdb::TableFunctionSet GetTspFunction();
duckdb::TableFunctionSet GetTspEuclideanFunction();

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
