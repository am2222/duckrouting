#include "duckrouting/function_docs.hpp"

#include "duckrouting/compat.hpp"

#include "duckdb/common/exception.hpp"

#include <cstddef>
#include <string>

namespace duckrouting {

namespace {

using duckdb::FunctionDescription;
using duckdb::InternalException;

//! The longest positional argument list any function here takes, plus the
//! terminating null.
const size_t kMaxParameters = 7;
const size_t kMaxCategories = 3;

//! Documentation for one registered function.
//!
//! `parameters` names the *positional* arguments of the longest overload.
//! Shorter overloads take a prefix of it, which is correct because every
//! optional argument in this extension is appended rather than inserted --
//! pgRouting spells its options positionally in that order.
//!
//! Named parameters are deliberately absent. `duckdb_functions()` reports them
//! after the positional ones, in whatever order the function's own map yields,
//! so they are read back off the function itself rather than written down here
//! where the two could disagree.
struct FunctionDoc {
	const char *name;
	const char *parameters[kMaxParameters];
	const char *description;
	//! One runnable call. Table functions get a full statement, since using one
	//! as a bare expression is a binder error; the scalar function gets the
	//! bare expression that core DuckDB documents its own built-ins with.
	const char *example;
	const char *categories[kMaxCategories];
};

//! The queries the examples route over. `edges` is the 18-edge graph from
//! pgRouting's own documentation, which the tests and the docs site both use.
#define EDGES               "'SELECT id, source, target, cost, reverse_cost FROM edges'"
#define XY_EDGES            "'SELECT id, source, target, cost, reverse_cost, x1, y1, x2, y2 FROM edges'"
#define CAPACITY_EDGES      "'SELECT id, source, target, capacity, reverse_capacity FROM edges'"
#define COST_CAPACITY_EDGES "'SELECT id, source, target, capacity, reverse_capacity, cost, reverse_cost FROM edges'"
#define POINTS              "'SELECT pid, edge_id, fraction, side FROM poi'"
#define RESTRICTIONS        "'SELECT path, cost FROM restrictions'"

const FunctionDoc kFunctionDocs[] = {
    // --- metadata -----------------------------------------------------------
    {"duckrouting_version",
     {"name"},
     "Greets `name` and reports the Boost version duckrouting was built against.",
     "duckrouting_version('Sam')",
     {"metadata"}},
    {"duckrouting_full_version",
     {},
     "The versions duckrouting was built from; returns one row of version, boost, compiler and build_type.",
     "SELECT * FROM duckrouting_full_version();",
     {"metadata"}},

    // --- dijkstra -----------------------------------------------------------
    {"duckrouting_dijkstra",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "Shortest path for every combination of start and end vertex, one row per node along each path; returns seq, "
     "path_seq, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_dijkstra(" EDGES ", 6, 10); -- six rows, 6 to 10 for agg_cost 5.0",
     {"routing", "shortest path"}},
    {"duckrouting_dijkstra_cost",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "The total cost of each shortest path without the per-node detail; returns start_vid, end_vid and agg_cost, one "
     "row per reachable pair.",
     "SELECT * FROM duckrouting_dijkstra_cost(" EDGES ", 6, 10); -- 6, 10, 5.0",
     {"routing", "shortest path"}},
    {"duckrouting_dijkstra_cost_matrix",
     {"edges_sql", "vids", "directed"},
     "Routes one list of vertices against itself, giving the cost of every ordered pair; returns start_vid, end_vid "
     "and agg_cost.",
     "SELECT * FROM duckrouting_dijkstra_cost_matrix(" EDGES ", [5, 6, 10], false); -- six rows, one per ordered pair",
     {"routing", "cost matrix"}},
    {"duckrouting_driving_distance",
     {"edges_sql", "start_vid", "distance", "directed"},
     "Every vertex reachable from start_vid within a total cost of distance -- the service area around a point -- as "
     "a shortest-path tree; returns seq, depth, start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_driving_distance(" EDGES ", 11, 2.0); -- nine rows out to cost 2.0",
     {"routing", "isochrone"}},
    {"duckrouting_dijkstra_via",
     {"edges_sql", "via_vids", "directed", "strict", "u_turn_on_edge"},
     "Routes through a sequence of vertices in order, one leg per consecutive pair; returns seq, path_id, path_seq, "
     "start_vid, end_vid, node, edge, cost, agg_cost and route_agg_cost, with edge -2 marking the end of the route.",
     "SELECT * FROM duckrouting_dijkstra_via(" EDGES ", [5, 1, 8]); -- two legs, route_agg_cost 7.0 at the end",
     {"routing", "shortest path"}},
    {"duckrouting_dijkstra_near",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "Of all the combinations of start and end vertex, the cap cheapest as full paths -- which of these is closest, "
     "and how do I get there; same columns as duckrouting_dijkstra, and cap defaults to 1.",
     "SELECT * FROM duckrouting_dijkstra_near(" EDGES ", 6, [10, 11, 1]); -- vertex 11, the nearest of the three",
     {"routing", "shortest path"}},
    {"duckrouting_dijkstra_near_cost",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "The same selection as duckrouting_dijkstra_near without the per-node detail; returns start_vid, end_vid and "
     "agg_cost.",
     "SELECT * FROM duckrouting_dijkstra_near_cost(" EDGES ", [10, 11, 1], 6, cap => 2); -- the two cheapest pairs",
     {"routing", "shortest path"}},
    {"duckrouting_ksp",
     {"edges_sql", "start_vid", "end_vid", "k", "directed"},
     "Up to k shortest loopless paths for every combination of start and end vertex, cheapest first, by Yen's "
     "algorithm; returns seq, path_id, path_seq, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_ksp(" EDGES ", 6, 17, 2); -- two routes, both costing 4.0",
     {"routing", "alternatives"}},

    // --- all pairs and components -------------------------------------------
    {"duckrouting_floyd_warshall",
     {"edges_sql", "directed"},
     "All-pairs shortest path costs via boost::floyd_warshall_all_pairs_shortest_paths, omitting self pairs and "
     "unreachable pairs; returns start_vid, end_vid and agg_cost.",
     "SELECT * FROM duckrouting_floyd_warshall(" EDGES ");",
     {"routing", "all pairs"}},
    {"duckrouting_johnson",
     {"edges_sql", "directed"},
     "All-pairs shortest path costs via boost::johnson_all_pairs_shortest_paths, which reweights the graph and then "
     "runs Dijkstra from every vertex; returns start_vid, end_vid and agg_cost.",
     "SELECT * FROM duckrouting_johnson('SELECT source, target, cost FROM edges');",
     {"routing", "all pairs"}},
    {"duckrouting_connected_components",
     {"edges_sql"},
     "Splits the undirected graph into connected components, each labelled by the smallest node id it contains; "
     "returns seq, component and node.",
     "SELECT * FROM duckrouting_connected_components(" EDGES ");",
     {"graph", "components"}},
    {"duckrouting_strong_components",
     {"edges_sql"},
     "Strongly connected components of the directed graph -- sets of vertices that can all reach one another "
     "following edge direction; returns seq, component and node.",
     "SELECT * FROM duckrouting_strong_components(" EDGES ");",
     {"graph", "components"}},
    {"duckrouting_biconnected_components",
     {"edges_sql"},
     "Partitions the edges into biconnected components, maximal sets in which no single vertex removal disconnects "
     "anything; returns seq, component and edge.",
     "SELECT * FROM duckrouting_biconnected_components(" EDGES ");",
     {"graph", "components"}},
    {"duckrouting_articulation_points",
     {"edges_sql"},
     "The vertices whose removal would increase the number of connected components -- the single points of failure; "
     "returns one node column.",
     "SELECT * FROM duckrouting_articulation_points(" EDGES "); -- nodes 3, 6, 7 and 8",
     {"graph", "components"}},
    {"duckrouting_bridges",
     {"edges_sql"},
     "The edges whose removal would disconnect the graph, being the biconnected components that hold a single edge; "
     "returns one edge column.",
     "SELECT * FROM duckrouting_bridges(" EDGES "); -- edges 1, 6, 7, 14, 17 and 18",
     {"graph", "components"}},
    {"duckrouting_make_connected",
     {"edges_sql"},
     "The vertex pairs that would have to be joined to make the graph connected, one fewer than the number of "
     "components; returns seq, start_vid and end_vid.",
     "SELECT * FROM duckrouting_make_connected(" EDGES "); -- two pairs, for three components",
     {"graph", "components"}},

    // --- spanning trees -----------------------------------------------------
    {"duckrouting_kruskal",
     {"edges_sql"},
     "The minimum spanning forest of the undirected graph via boost::kruskal_minimum_spanning_tree, one row per "
     "chosen edge; returns edge and cost.",
     "SELECT * FROM duckrouting_kruskal(" EDGES "); -- 14 edges, for 17 vertices in 3 components",
     {"graph", "spanning tree"}},
    {"duckrouting_prim",
     {"edges_sql"},
     "The same minimum spanning forest via boost::prim_minimum_spanning_tree, grown outwards from a root in each "
     "component; returns edge and cost.",
     "SELECT * FROM duckrouting_prim(" EDGES ");",
     {"graph", "spanning tree"}},
    {"duckrouting_kruskal_bfs",
     {"edges_sql", "root", "max_depth"},
     "Walks the Kruskal spanning forest breadth first from each root, stopping at max_depth; returns seq, depth, "
     "start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_kruskal_bfs(" EDGES ", 6, 2);",
     {"graph", "spanning tree"}},
    {"duckrouting_kruskal_dfs",
     {"edges_sql", "root", "max_depth"},
     "Walks the Kruskal spanning forest depth first from each root, stopping at max_depth, so depth rises and falls "
     "as the walk backtracks; returns seq, depth, start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_kruskal_dfs(" EDGES ", 6, 2);",
     {"graph", "spanning tree"}},
    {"duckrouting_kruskal_dd",
     {"edges_sql", "root", "distance"},
     "Walks the Kruskal spanning forest depth first from each root, stopping at an accumulated cost of distance; "
     "returns seq, depth, start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_kruskal_dd(" EDGES ", 6, 3.5);",
     {"graph", "spanning tree"}},
    {"duckrouting_prim_bfs",
     {"edges_sql", "root", "max_depth"},
     "Walks the Prim spanning forest breadth first from each root, stopping at max_depth; returns seq, depth, "
     "start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_prim_bfs(" EDGES ", [6, 13]); -- one walk per root, under its own start_vid",
     {"graph", "spanning tree"}},
    {"duckrouting_prim_dfs",
     {"edges_sql", "root", "max_depth"},
     "Walks the Prim spanning forest depth first from each root, stopping at max_depth; returns seq, depth, "
     "start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_prim_dfs(" EDGES ", 6, 2);",
     {"graph", "spanning tree"}},
    {"duckrouting_prim_dd",
     {"edges_sql", "root", "distance"},
     "Walks the Prim spanning forest depth first from each root, stopping at an accumulated cost of distance; returns "
     "seq, depth, start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_prim_dd(" EDGES ", 6, 3.5);",
     {"graph", "spanning tree"}},

    // --- traversal and ordering ---------------------------------------------
    {"duckrouting_breadth_first_search",
     {"edges_sql", "root", "max_depth"},
     "Visits every vertex reachable from root, nearest first, so depth never decreases as seq advances; returns seq, "
     "depth, start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_breadth_first_search(" EDGES ", 6, 2);",
     {"graph", "traversal"}},
    {"duckrouting_depth_first_search",
     {"edges_sql", "root", "max_depth"},
     "Visits every vertex reachable from root, following each branch to its end before backtracking; returns seq, "
     "depth, start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_depth_first_search(" EDGES ", 6);",
     {"graph", "traversal"}},
    {"duckrouting_bellman_ford",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "Shortest path via boost::bellman_ford_shortest_paths, slower than Dijkstra but tolerant of negative edge "
     "weights; returns seq, path_seq, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_bellman_ford(" EDGES ", 6, 10, true);",
     {"routing", "shortest path"}},
    {"duckrouting_dag_shortest_path",
     {"edges_sql", "start_vid", "end_vid"},
     "Shortest path over a directed acyclic graph via boost::dag_shortest_paths, linear in the size of the graph and "
     "an error if the graph cycles; returns seq, path_seq, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_dag_shortest_path('SELECT id, source, target, cost FROM edges', 5, 11);",
     {"routing", "shortest path"}},
    {"duckrouting_transitive_closure",
     {"edges_sql"},
     "Every vertex reachable from every vertex; returns node and targets, an ascending BIGINT[] of everything "
     "reachable from it.",
     "SELECT * FROM duckrouting_transitive_closure(" EDGES ");",
     {"graph", "traversal"}},
    {"duckrouting_cuthill_mckee_ordering",
     {"edges_sql"},
     "A bandwidth-reducing permutation of the vertices via boost::cuthill_mckee_ordering; returns seq and node.",
     "SELECT * FROM duckrouting_cuthill_mckee_ordering(" EDGES ");",
     {"graph", "ordering"}},
    {"duckrouting_king_ordering",
     {"edges_sql"},
     "A bandwidth-reducing permutation of the vertices via boost::king_ordering; returns seq and node.",
     "SELECT * FROM duckrouting_king_ordering(" EDGES ");",
     {"graph", "ordering"}},
    {"duckrouting_sloan_ordering",
     {"edges_sql"},
     "A bandwidth-reducing permutation via boost::sloan_ordering, which starts from a pseudo-peripheral pair and so "
     "covers one connected component; returns seq and node.",
     "SELECT * FROM duckrouting_sloan_ordering(" EDGES ");",
     {"graph", "ordering"}},
    {"duckrouting_topological_sort",
     {"edges_sql"},
     "Orders a directed acyclic graph so that every edge runs forwards, raising an error on a cyclic graph; returns "
     "seq and node.",
     "SELECT * FROM duckrouting_topological_sort('SELECT id, source, target, cost FROM edges');",
     {"graph", "ordering"}},

    // --- analysis -----------------------------------------------------------
    {"duckrouting_sequential_vertex_coloring",
     {"edges_sql"},
     "Assigns each vertex a colour so that no edge joins two vertices of the same colour; returns node and color, "
     "counted from 1.",
     "SELECT * FROM duckrouting_sequential_vertex_coloring(" EDGES ");",
     {"graph", "analysis"}},
    {"duckrouting_edge_coloring",
     {"edges_sql"},
     "Assigns each edge a colour so that edges meeting at a vertex differ; returns edge and color, counted from 1.",
     "SELECT * FROM duckrouting_edge_coloring(" EDGES ");",
     {"graph", "analysis"}},
    {"duckrouting_bipartite",
     {"edges_sql"},
     "Splits the vertices into two sides such that every edge crosses between them, returning no rows when the graph "
     "is not bipartite; returns node and color, which is 0 or 1.",
     "SELECT * FROM duckrouting_bipartite(" EDGES ");",
     {"graph", "analysis"}},
    {"duckrouting_is_planar",
     {"edges_sql"},
     "Whether the graph can be drawn with no crossing edges; returns one row of one BOOLEAN column, is_planar.",
     "SELECT * FROM duckrouting_is_planar(" EDGES "); -- true",
     {"graph", "analysis"}},
    {"duckrouting_boyer_myrvold",
     {"edges_sql"},
     "The planar embedding: each vertex's incident edges in rotation order, so every edge appears twice, once from "
     "each endpoint; returns seq, source and target, and nothing at all for a non-planar graph.",
     "SELECT * FROM duckrouting_boyer_myrvold(" EDGES ");",
     {"graph", "analysis"}},
    {"duckrouting_bandwidth",
     {"edges_sql"},
     "The largest gap between the indices of two adjacent vertices, which the ordering functions exist to reduce; "
     "returns one row of one BIGINT column, bandwidth.",
     "SELECT * FROM duckrouting_bandwidth(" EDGES "); -- 5",
     {"graph", "analysis"}},
    {"duckrouting_betweenness_centrality",
     {"edges_sql", "directed"},
     "How often each vertex lies on a shortest path between two others, normalised to 0-1, so that a high score "
     "marks a bottleneck; returns vid and centrality.",
     "SELECT * FROM duckrouting_betweenness_centrality(" EDGES ");",
     {"graph", "analysis"}},
    {"duckrouting_stoer_wagner",
     {"edges_sql"},
     "The cheapest set of edges whose removal disconnects the undirected graph, one row per edge crossing the cut; "
     "returns seq, edge, cost and mincut, where mincut is the total and repeats on every row.",
     "SELECT * FROM duckrouting_stoer_wagner(" EDGES ");",
     {"graph", "analysis"}},
    {"duckrouting_hawick_circuits",
     {"edges_sql"},
     "Every circuit -- closed path -- in the directed graph, of which there can be very many; returns seq, path_id, "
     "path_seq counting from 0, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_hawick_circuits(" EDGES ");",
     {"graph", "analysis"}},
    {"duckrouting_dominator_tree",
     {"edges_sql", "root"},
     "For each vertex the immediate dominator relative to root, the last vertex every path from the root must pass "
     "through before reaching it; returns vertex_id and idom, which is 0 for the root and for anything unreachable.",
     "SELECT * FROM duckrouting_dominator_tree(" EDGES ", 5);",
     {"graph", "analysis"}},

    // --- flow ---------------------------------------------------------------
    {"duckrouting_max_flow",
     {"edges_sql", "source", "sink"},
     "The maximum total flow from source to sink, reading capacity and reverse_capacity rather than costs; returns "
     "one row of one DOUBLE column, flow.",
     "SELECT * FROM duckrouting_max_flow(" CAPACITY_EDGES ", 11, 12); -- 230.0",
     {"graph", "flow"}},
    {"duckrouting_push_relabel",
     {"edges_sql", "source", "sink"},
     "The maximum flow from source to sink broken down edge by edge, via boost::push_relabel_max_flow; returns seq, "
     "edge, start_vid, end_vid, flow and residual_capacity for the edges that carry flow.",
     "SELECT * FROM duckrouting_push_relabel(" CAPACITY_EDGES ", 11, 12);",
     {"graph", "flow"}},
    {"duckrouting_edmonds_karp",
     {"edges_sql", "source", "sink"},
     "The maximum flow from source to sink broken down edge by edge, via boost::edmonds_karp_max_flow; returns seq, "
     "edge, start_vid, end_vid, flow and residual_capacity for the edges that carry flow.",
     "SELECT * FROM duckrouting_edmonds_karp(" CAPACITY_EDGES ", 11, 12);",
     {"graph", "flow"}},
    {"duckrouting_boykov_kolmogorov",
     {"edges_sql", "source", "sink"},
     "The maximum flow from source to sink broken down edge by edge, via boost::boykov_kolmogorov_max_flow; returns "
     "seq, edge, start_vid, end_vid, flow and residual_capacity for the edges that carry flow.",
     "SELECT * FROM duckrouting_boykov_kolmogorov(" CAPACITY_EDGES ", 11, 12);",
     {"graph", "flow"}},
    {"duckrouting_max_flow_min_cost",
     {"edges_sql", "source", "sink"},
     "Of all the ways to achieve the maximum flow the cheapest one, from an edges query carrying costs as well as "
     "capacities; returns seq, edge, source, target, flow, residual_capacity, cost and agg_cost.",
     "SELECT * FROM duckrouting_max_flow_min_cost(" COST_CAPACITY_EDGES ", 11, 12);",
     {"graph", "flow"}},
    {"duckrouting_max_flow_min_cost_cost",
     {"edges_sql", "source", "sink"},
     "The total cost of the cheapest maximum flow; returns one row of one DOUBLE column, cost.",
     "SELECT * FROM duckrouting_max_flow_min_cost_cost(" COST_CAPACITY_EDGES ", 11, 12); -- 430.0",
     {"graph", "flow"}},
    {"duckrouting_edge_disjoint_paths",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "As many routes between the endpoints as exist that share no edge, found by giving every usable direction "
     "capacity 1 and decomposing the flow back into paths; returns seq, path_id, path_seq, start_vid, end_vid, node, "
     "edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_edge_disjoint_paths(" EDGES ", 11, 12); -- two independent routes",
     {"graph", "flow"}},
    {"duckrouting_max_cardinality_match",
     {"edges_sql"},
     "The largest set of edges no two of which share a vertex; returns one edge column.",
     "SELECT * FROM duckrouting_max_cardinality_match(" EDGES ");",
     {"graph", "flow"}},

    // --- A* -----------------------------------------------------------------
    {"duckrouting_astar",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "Shortest path guided by a heuristic over the x1, y1, x2 and y2 columns the edges query must also expose, giving "
     "Dijkstra's answer for less exploration; returns seq, path_seq, start_vid, end_vid, node, edge, cost and "
     "agg_cost.",
     "SELECT * FROM duckrouting_astar(" XY_EDGES ", 6, 10, heuristic => 3, factor => 3.5);",
     {"routing", "shortest path"}},
    {"duckrouting_astar_cost",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "The total cost of each A* shortest path; returns start_vid, end_vid and agg_cost, one row per reachable pair.",
     "SELECT * FROM duckrouting_astar_cost(" XY_EDGES ", 6, [10, 12]);",
     {"routing", "shortest path"}},
    {"duckrouting_astar_cost_matrix",
     {"edges_sql", "vids", "directed"},
     "Routes one list of vertices against itself with A*, giving the cost of every ordered pair; returns start_vid, "
     "end_vid and agg_cost.",
     "SELECT * FROM duckrouting_astar_cost_matrix(" XY_EDGES ", [5, 6, 10], false);",
     {"routing", "cost matrix"}},

    // --- travelling salesman ------------------------------------------------
    {"duckrouting_tsp",
     {"matrix_sql", "start_id", "end_id"},
     "An approximate shortest tour visiting every vertex once and returning to the start, from a cost matrix "
     "exposing start_vid, end_vid and agg_cost; returns seq, node, cost and agg_cost, where cost is the cost of "
     "arriving rather than of leaving.",
     "SELECT * FROM duckrouting_tsp('SELECT start_vid, end_vid, agg_cost FROM matrix');",
     {"routing", "tsp"}},
    {"duckrouting_tsp_euclidean",
     {"points_sql", "start_id", "end_id"},
     "The same approximate tour with distances computed from a points query exposing id, x and y rather than from a "
     "graph; returns seq, node, cost and agg_cost.",
     "SELECT * FROM duckrouting_tsp_euclidean('SELECT * FROM (VALUES (1,0.0,0.0),(2,1.0,0.0),(3,1.0,1.0),(4,0.0,1.0))"
     " t(id,x,y)'); -- the perimeter of the unit square, agg_cost 4.0",
     {"routing", "tsp"}},

    // --- contraction --------------------------------------------------------
    {"duckrouting_contraction_hierarchies",
     {"edges_sql", "directed"},
     "Preprocesses the graph for fast routing, contracting vertices one at a time and adding a shortcut edge "
     "wherever that would otherwise lengthen a shortest path; returns type ('v' for a vertex, 'e' for a shortcut), "
     "id, contracted_vertices, source, target, cost, metric and vertex_order.",
     "SELECT * FROM duckrouting_contraction_hierarchies('SELECT id, source, target, cost FROM edges', false);",
     {"graph", "contraction"}},
    {"duckrouting_contraction",
     {"edges_sql", "directed"},
     "Simplifies the graph by absorbing dead ends and then collapsing linear chains, repeating the pair for cycles "
     "rounds; returns type ('v' or 'e'), id, contracted_vertices, source, target and cost.",
     "SELECT * FROM duckrouting_contraction(" EDGES ", false, cycles => 2);",
     {"graph", "contraction"}},
    {"duckrouting_dead_end_contraction",
     {"edges_sql", "directed"},
     "Simplifies the graph by absorbing dead-end vertices into the neighbour that holds them; returns type ('v' or "
     "'e'), id, contracted_vertices, source, target and cost.",
     "SELECT * FROM duckrouting_dead_end_contraction(" EDGES ", false);",
     {"graph", "contraction"}},
    {"duckrouting_linear_contraction",
     {"edges_sql", "directed"},
     "Simplifies the graph by collapsing chains of degree-two vertices into a single edge; returns type ('v' or "
     "'e'), id, contracted_vertices, source, target and cost.",
     "SELECT * FROM duckrouting_linear_contraction(" EDGES ", false);",
     {"graph", "contraction"}},

    // --- bidirectional and other shortest paths -----------------------------
    {"duckrouting_bd_dijkstra",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "Bidirectional Dijkstra: searches forward from the start and backward from the end until the two frontiers "
     "meet, giving Dijkstra's answer for less exploration; returns seq, path_seq, start_vid, end_vid, node, edge, "
     "cost and agg_cost.",
     "SELECT * FROM duckrouting_bd_dijkstra(" EDGES ", 6, 10);",
     {"routing", "shortest path"}},
    {"duckrouting_bd_dijkstra_cost",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "The total cost of each bidirectional Dijkstra path; returns start_vid, end_vid and agg_cost.",
     "SELECT * FROM duckrouting_bd_dijkstra_cost(" EDGES ", 6, 10);",
     {"routing", "shortest path"}},
    {"duckrouting_bd_dijkstra_cost_matrix",
     {"edges_sql", "vids", "directed"},
     "Routes one list of vertices against itself with bidirectional Dijkstra, giving every ordered pair; returns "
     "start_vid, end_vid and agg_cost.",
     "SELECT * FROM duckrouting_bd_dijkstra_cost_matrix(" EDGES ", [5, 6, 10], false);",
     {"routing", "cost matrix"}},
    {"duckrouting_bd_astar",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "Bidirectional A*: both frontiers guided by a heuristic over the x1, y1, x2 and y2 columns the edges query must "
     "also expose; returns seq, path_seq, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_bd_astar(" XY_EDGES ", 6, 10);",
     {"routing", "shortest path"}},
    {"duckrouting_bd_astar_cost",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "The total cost of each bidirectional A* path; returns start_vid, end_vid and agg_cost.",
     "SELECT * FROM duckrouting_bd_astar_cost(" XY_EDGES ", 6, 10);",
     {"routing", "shortest path"}},
    {"duckrouting_bd_astar_cost_matrix",
     {"edges_sql", "vids", "directed"},
     "Routes one list of vertices against itself with bidirectional A*, giving every ordered pair; returns "
     "start_vid, end_vid and agg_cost.",
     "SELECT * FROM duckrouting_bd_astar_cost_matrix(" XY_EDGES ", [5, 6, 10], false);",
     {"routing", "cost matrix"}},
    {"duckrouting_edward_moore",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "Shortest path by Edward Moore's algorithm, better known as SPFA: a queue-based refinement of Bellman-Ford; "
     "returns seq, path_seq, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_edward_moore(" EDGES ", 6, 10, true);",
     {"routing", "shortest path"}},
    {"duckrouting_binary_breadth_first_search",
     {"edges_sql", "start_vid", "end_vid", "directed"},
     "Shortest path by 0-1 BFS, a deque-based search for graphs whose edges all cost 0 or 1; returns seq, path_seq, "
     "start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_binary_breadth_first_search(" EDGES ", 6, 10);",
     {"routing", "shortest path"}},

    // --- graph metadata -----------------------------------------------------
    {"duckrouting_extract_vertices",
     {"edges_sql"},
     "Derives the vertex table implied by an edges query, every vertex appearing exactly once; returns id, in_edges "
     "and out_edges.",
     "SELECT * FROM duckrouting_extract_vertices('SELECT id, source, target FROM edges');",
     {"graph", "metadata"}},
    {"duckrouting_degree",
     {"edges_sql"},
     "Counts the edges incident on each vertex; returns node and degree.",
     "SELECT * FROM duckrouting_degree('SELECT id, source, target FROM edges');",
     {"graph", "metadata"}},

    // --- withPoints ---------------------------------------------------------
    {"duckrouting_with_points",
     {"edges_sql", "points_sql", "start_vid", "end_vid", "driving_side"},
     "Shortest path that may start at, end at or pass through a point sitting partway along an edge, read from a "
     "points query exposing pid, edge_id, fraction and an optional side; each point becomes a vertex numbered -pid. "
     "Returns seq, path_seq, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_with_points(" EDGES ", " POINTS ", -1, -3, 'b', details => true);",
     {"routing", "points"}},
    {"duckrouting_with_points_cost",
     {"edges_sql", "points_sql", "start_vid", "end_vid", "driving_side"},
     "The total cost of each withPoints shortest path; returns start_vid, end_vid and agg_cost.",
     "SELECT * FROM duckrouting_with_points_cost(" EDGES ", " POINTS ", -1, -3, 'b');",
     {"routing", "points"}},
    {"duckrouting_with_points_cost_matrix",
     {"edges_sql", "points_sql", "vids", "driving_side"},
     "Routes one list of vertices and points against itself, giving the cost of every ordered pair; returns "
     "start_vid, end_vid and agg_cost.",
     "SELECT * FROM duckrouting_with_points_cost_matrix(" EDGES ", " POINTS ", [-1, -3, 6], 'b');",
     {"routing", "points"}},
    {"duckrouting_with_points_via",
     {"edges_sql", "points_sql", "via_vids", "driving_side"},
     "Routes through a sequence of vertices and points in order, one leg per consecutive pair; returns seq, path_id, "
     "path_seq, start_vid, end_vid, node, edge, cost, agg_cost and route_agg_cost.",
     "SELECT * FROM duckrouting_with_points_via(" EDGES ", " POINTS ", [-1, 6, -3], 'b');",
     {"routing", "points"}},
    {"duckrouting_with_points_ksp",
     {"edges_sql", "points_sql", "start_vid", "end_vid", "k", "driving_side"},
     "Up to k shortest loopless paths between vertices or points sitting partway along an edge, cheapest first; "
     "returns seq, path_id, path_seq, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_with_points_ksp(" EDGES ", " POINTS ", -1, -3, 2, 'b');",
     {"routing", "points"}},
    {"duckrouting_with_points_dd",
     {"edges_sql", "points_sql", "start_vid", "distance", "driving_side"},
     "Driving distance from a vertex or from a point sitting partway along an edge -- a house halfway down a street "
     "-- within a total cost of distance; returns seq, depth, start_vid, pred, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_with_points_dd(" EDGES ", " POINTS ", -1, 3.3, 'r', details => true);",
     {"routing", "isochrone"}},

    // --- turn restrictions --------------------------------------------------
    {"duckrouting_trsp",
     {"edges_sql", "restrictions_sql", "start_vid", "end_vid", "directed"},
     "Shortest path that respects turn restrictions, read from a query exposing path (a BIGINT[] of edge ids) and "
     "cost; following that exact sequence costs extra rather than being forbidden, so a large cost is an effective "
     "ban. Returns seq, path_seq, start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_trsp(" EDGES ", " RESTRICTIONS ", 6, 10, false);",
     {"routing", "turn restrictions"}},
    {"duckrouting_trsp_via",
     {"edges_sql", "restrictions_sql", "via_vids", "directed"},
     "Turn-restricted routing through a sequence of vertices in order, one leg per consecutive pair; returns seq, "
     "path_id, path_seq, start_vid, end_vid, node, edge, cost, agg_cost and route_agg_cost.",
     "SELECT * FROM duckrouting_trsp_via(" EDGES ", " RESTRICTIONS ", [5, 1, 8], false);",
     {"routing", "turn restrictions"}},
    {"duckrouting_trsp_with_points",
     {"edges_sql", "restrictions_sql", "points_sql", "start_vid", "end_vid", "directed"},
     "Turn-restricted routing between vertices or points sitting partway along an edge; returns seq, path_seq, "
     "start_vid, end_vid, node, edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_trsp_with_points(" EDGES ", " RESTRICTIONS ", " POINTS ", -1, -3, false);",
     {"routing", "turn restrictions"}},
    {"duckrouting_trsp_via_with_points",
     {"edges_sql", "restrictions_sql", "points_sql", "via_vids", "directed"},
     "Turn-restricted routing through a sequence of vertices and points in order; returns seq, path_id, path_seq, "
     "start_vid, end_vid, node, edge, cost, agg_cost and route_agg_cost.",
     "SELECT * FROM duckrouting_trsp_via_with_points(" EDGES ", " RESTRICTIONS ", " POINTS ", [-1, 6, -3], false);",
     {"routing", "turn restrictions"}},

    // --- transforms ---------------------------------------------------------
    {"duckrouting_line_graph",
     {"edges_sql", "directed"},
     "The line graph: each edge of the input becomes a vertex, and two such vertices are joined when the edges they "
     "stand for share an endpoint; returns seq, source, target, cost and reverse_cost.",
     "SELECT * FROM duckrouting_line_graph(" EDGES ", false);",
     {"graph", "transform"}},
    {"duckrouting_line_graph_full",
     {"edges_sql"},
     "The full line graph, which additionally splits each vertex into one node per incident half-edge so that turn "
     "costs can be attached; returns seq, source, target, cost and edge.",
     "SELECT * FROM duckrouting_line_graph_full(" EDGES ");",
     {"graph", "transform"}},
    {"duckrouting_chinese_postman",
     {"edges_sql", "directed"},
     "A closed walk traversing every edge at least once, repeating as few of them as possible; returns seq, node, "
     "edge, cost and agg_cost.",
     "SELECT * FROM duckrouting_chinese_postman(" EDGES ", false);",
     {"routing", "chinese postman"}},
    {"duckrouting_chinese_postman_cost",
     {"edges_sql", "directed"},
     "The total cost of the closed walk traversing every edge at least once; returns one row of one DOUBLE column, "
     "cost.",
     "SELECT * FROM duckrouting_chinese_postman_cost(" EDGES ", false);",
     {"routing", "chinese postman"}},

    // --- geometry macros ----------------------------------------------------
    // Unlike every function above, these three name a *table* rather than
    // carrying a query: they reach it through query_table(), which takes a
    // name. The parameter is still called edges_sql, so the description has to
    // say so.
    {"duckrouting_separate_crossing",
     {},
     "Splits every edge at the points where it crosses another edge; edges_sql names a table of id and geom rather "
     "than holding a query. Returns seq, id, sub_id and geom. Needs the spatial extension loaded.",
     "SELECT * FROM duckrouting_separate_crossing('edges');",
     {"geometry", "topology"}},
    {"duckrouting_separate_touching",
     {},
     "Splits every edge where another edge's endpoint touches its interior -- a junction that was never noded; "
     "edges_sql names a table of id and geom rather than holding a query. Returns seq, id, sub_id and geom. Needs "
     "the spatial extension loaded.",
     "SELECT * FROM duckrouting_separate_touching('edges');",
     {"geometry", "topology"}},
    {"duckrouting_find_close_edges",
     {},
     "For each point, the cap nearest edges within tolerance, reported in the shape the withPoints family expects; "
     "edges_sql and points_sql name tables of id and geom, and of pid and geom, rather than holding queries. Returns "
     "seq, pid, edge_id, fraction, distance, geom and edge. Needs the spatial extension loaded.",
     "SELECT * FROM duckrouting_find_close_edges('edges', 'points', 0.5);",
     {"geometry", "topology"}},
};

#undef EDGES
#undef XY_EDGES
#undef CAPACITY_EDGES
#undef COST_CAPACITY_EDGES
#undef POINTS
#undef RESTRICTIONS

const FunctionDoc &Require(const std::string &name) {
	for (size_t i = 0; i < sizeof(kFunctionDocs) / sizeof(kFunctionDocs[0]); i++) {
		if (name == kFunctionDocs[i].name) {
			return kFunctionDocs[i];
		}
	}
	// Registering something undocumented is a bug in this extension, not a
	// condition a caller can reach, so it fails the load rather than quietly
	// producing another `col0` function.
	throw InternalException("duckrouting: no documentation entry for '%s'", name);
}

//! Everything a description carries that does not depend on the overload.
FunctionDescription Prose(const FunctionDoc &doc) {
	FunctionDescription description;
	description.description = doc.description;
	description.examples.push_back(doc.example);
	for (size_t i = 0; i < kMaxCategories && doc.categories[i] != nullptr; i++) {
		description.categories.push_back(doc.categories[i]);
	}
	return description;
}

//! Names the positional arguments of one overload from the documented list.
void NamePositionalArguments(const FunctionDoc &doc, const duckdb::vector<duckdb::LogicalType> &arguments,
                             FunctionDescription &description) {
	for (size_t i = 0; i < arguments.size(); i++) {
		if (i >= kMaxParameters || doc.parameters[i] == nullptr) {
			throw InternalException("duckrouting: '%s' has an overload taking more arguments than are documented",
			                        doc.name);
		}
		description.parameter_names.push_back(doc.parameters[i]);
	}
}

} // namespace

duckdb::CreateTableFunctionInfo Documented(duckdb::TableFunctionSet set) {
	const FunctionDoc &doc = Require(NameText(set.name));
	duckdb::CreateTableFunctionInfo info(std::move(set));
	// What the bare RegisterFunction(TableFunctionSet) overload does before
	// handing the info to the catalog; CreateInfo itself defaults to
	// ERROR_ON_CONFLICT.
	info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
	for (size_t i = 0; i < info.functions.functions.size(); i++) {
		const duckdb::TableFunction &function = Overload(info.functions.functions[i]);
		const duckdb::vector<duckdb::LogicalType> &arguments = ArgumentTypes(function);
		FunctionDescription description = Prose(doc);
		description.parameter_types = arguments;
		NamePositionalArguments(doc, arguments, description);
		// duckdb_functions() lists named parameters after the positional ones,
		// in the order the function's own map yields them, and falls back to
		// `colN` for any name the description does not reach. Reading them back
		// off the function is the only way to keep the two in step.
		for (auto &named : function.named_parameters) {
			description.parameter_names.push_back(NameText(named.first));
		}
		info.descriptions.push_back(description);
	}
	return info;
}

duckdb::CreateScalarFunctionInfo Documented(duckdb::ScalarFunction function) {
	const FunctionDoc &doc = Require(NameText(function.name));
	duckdb::ScalarFunctionSet set(function.name);
	set.AddFunction(std::move(function));
	duckdb::CreateScalarFunctionInfo info(std::move(set));
	info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
	// There is only ever one overload here, so a lone description with no
	// parameter types attaches to it whatever its arity, and a name past the
	// last argument is simply never read. That is worth the small asymmetry
	// with the table functions above: it avoids reaching for the argument
	// types, which v2 keeps inside a FunctionSignature rather than on the
	// function itself.
	FunctionDescription description = Prose(doc);
	for (size_t i = 0; i < kMaxParameters && doc.parameters[i] != nullptr; i++) {
		description.parameter_names.push_back(doc.parameters[i]);
	}
	info.descriptions.push_back(description);
	return info;
}

void Document(duckdb::CreateMacroInfo &info) {
	// A macro already reports the parameter names it was declared with, and a
	// lone description with no parameter types attaches to every overload
	// without displacing them.
	info.descriptions.push_back(Prose(Require(NameText(InfoName(info)))));
}

} // namespace duckrouting
