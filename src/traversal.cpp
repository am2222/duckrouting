#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <boost/graph/bellman_ford_shortest_paths.hpp>
#include <boost/graph/dag_shortest_paths.hpp>

#include <algorithm>
#include <deque>
#include <limits>

namespace duckrouting {

std::vector<DrivingDistanceRow> WalkGraph(const Adjacency &adjacency, const VertexIndex &index,
                                          const std::vector<int64_t> &roots, Traversal traversal, double limit) {
	std::vector<int64_t> ordered_roots(roots);
	std::sort(ordered_roots.begin(), ordered_roots.end());
	ordered_roots.erase(std::unique(ordered_roots.begin(), ordered_roots.end()), ordered_roots.end());

	// Each pending entry carries everything the output row needs.
	struct Pending {
		uint64_t vertex;
		int64_t pred;
		int64_t edge;
		double cost;
		int64_t depth;
		double agg_cost;
	};

	std::vector<DrivingDistanceRow> rows;
	for (size_t r = 0; r < ordered_roots.size(); r++) {
		const int64_t root_id = ordered_roots[r];
		uint64_t root = 0;
		if (!index.Find(root_id, root)) {
			// pgRouting still reports a lone row for a root outside the graph.
			rows.push_back(DrivingDistanceRow {0, root_id, root_id, root_id, -1, 0, 0});
			continue;
		}

		std::vector<bool> seen(index.Size(), false);
		std::deque<Pending> queue;
		queue.push_back(Pending {root, root_id, -1, 0, 0, 0});

		while (!queue.empty()) {
			Pending current;
			if (traversal == Traversal::Bfs) {
				current = queue.front();
				queue.pop_front();
			} else {
				current = queue.back();
				queue.pop_back();
			}
			if (seen[current.vertex]) {
				continue;
			}
			seen[current.vertex] = true;

			rows.push_back(DrivingDistanceRow {current.depth, root_id, current.pred, index.IdOf(current.vertex),
			                                   current.edge, DecodeInfinity(current.cost),
			                                   DecodeInfinity(current.agg_cost)});

			// Neighbours are taken in edge-id order. A stack hands them back
			// reversed, so push them reversed to preserve that order.
			const std::vector<Arc> &neighbours = adjacency[current.vertex];
			for (size_t i = 0; i < neighbours.size(); i++) {
				const Arc &arc =
				    traversal == Traversal::Bfs ? neighbours[i] : neighbours[neighbours.size() - 1 - i];
				if (seen[arc.to]) {
					continue;
				}
				const int64_t depth = current.depth + 1;
				const double agg_cost = current.agg_cost + arc.cost;
				if (traversal == Traversal::DfsCost) {
					if (agg_cost > limit) {
						continue;
					}
				} else if (static_cast<double>(depth) > limit) {
					continue;
				}
				queue.push_back(
				    Pending {arc.to, index.IdOf(current.vertex), arc.edge, arc.cost, depth, agg_cost});
			}
		}
	}
	return rows;
}

std::vector<DrivingDistanceRow> GraphTraversal(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &roots,
                                               Traversal traversal, double limit, bool directed) {
	// Sorting by edge id makes the neighbour order deterministic regardless of
	// how the user's query happened to order its rows.
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(),
	                 [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });

	VertexIndex index;
	const Adjacency adjacency = BuildAdjacency(ordered, index, directed);
	return WalkGraph(adjacency, index, roots, traversal, limit);
}

namespace {

//! Turns a Boost predecessor map into pgRouting's per-node path rows.
template <typename Graph>
std::vector<PathRow> BuildPath(const Graph &graph, const VertexIndex &index,
                               const std::vector<uint64_t> &predecessor, uint64_t source, uint64_t sink,
                               int64_t start_vid, int64_t end_vid) {
	if (sink != source && predecessor[sink] == sink) {
		return {};
	}
	std::vector<uint64_t> path;
	for (uint64_t at = sink;; at = predecessor[at]) {
		path.push_back(at);
		if (at == source) {
			break;
		}
	}
	std::reverse(path.begin(), path.end());

	std::vector<PathRow> rows;
	double agg_cost = 0;
	for (size_t i = 0; i < path.size(); i++) {
		PathRow row {};
		row.path_seq = static_cast<int64_t>(i) + 1;
		row.start_vid = start_vid;
		row.end_vid = end_vid;
		row.node = index.IdOf(path[i]);
		row.agg_cost = DecodeInfinity(agg_cost);
		if (i + 1 < path.size()) {
			// Parallel edges are relaxed by their minimum, so the cheapest one
			// is the edge the path actually used.
			bool found = false;
			RoutingEdge best {};
			typename boost::graph_traits<Graph>::out_edge_iterator it, end;
			for (boost::tie(it, end) = boost::out_edges(path[i], graph); it != end; ++it) {
				if (static_cast<uint64_t>(boost::target(*it, graph)) != path[i + 1]) {
					continue;
				}
				if (!found || graph[*it].cost < best.cost) {
					best = graph[*it];
					found = true;
				}
			}
			if (!found) {
				return {};
			}
			row.edge = best.id;
			row.cost = DecodeInfinity(best.cost);
			agg_cost += best.cost;
		} else {
			row.edge = -1;
			row.cost = 0;
		}
		rows.push_back(row);
	}
	return rows;
}

std::vector<int64_t> Normalized(const std::vector<int64_t> &values) {
	std::vector<int64_t> result(values);
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

template <typename Graph>
std::vector<PathRow> BellmanFordOn(const Graph &graph, const VertexIndex &index,
                                   const std::vector<int64_t> &starts, const std::vector<int64_t> &ends) {
	const size_t n = boost::num_vertices(graph);
	std::vector<PathRow> rows;
	for (size_t s = 0; s < starts.size(); s++) {
		uint64_t source = 0;
		if (!index.Find(starts[s], source)) {
			continue;
		}
		std::vector<double> distance(n, std::numeric_limits<double>::infinity());
		std::vector<uint64_t> predecessor(n);
		for (size_t i = 0; i < n; i++) {
			predecessor[i] = i;
		}
		distance[source] = 0;

		boost::bellman_ford_shortest_paths(graph, static_cast<int>(n),
		                                   boost::weight_map(boost::get(&RoutingEdge::cost, graph))
		                                       .distance_map(&distance[0])
		                                       .predecessor_map(&predecessor[0]));

		for (size_t e = 0; e < ends.size(); e++) {
			if (ends[e] == starts[s]) {
				continue;
			}
			uint64_t sink = 0;
			if (!index.Find(ends[e], sink)) {
				continue;
			}
			auto path = BuildPath(graph, index, predecessor, source, sink, starts[s], ends[e]);
			rows.insert(rows.end(), path.begin(), path.end());
		}
	}
	return rows;
}

} // namespace

std::vector<PathRow> BellmanFord(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                 const std::vector<int64_t> &ends, bool directed) {
	const auto s = Normalized(starts);
	const auto e = Normalized(ends);
	VertexIndex index;
	if (directed) {
		auto graph = BuildGraph<DirectedGraph>(edges, index);
		return BellmanFordOn(graph, index, s, e);
	}
	auto graph = BuildGraph<UndirectedGraph>(edges, index);
	return BellmanFordOn(graph, index, s, e);
}

std::vector<PathRow> DagShortestPath(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                     const std::vector<int64_t> &ends) {
	const auto normalized_starts = Normalized(starts);
	const auto normalized_ends = Normalized(ends);

	VertexIndex index;
	auto graph = BuildGraph<DirectedGraph>(edges, index);
	const size_t n = boost::num_vertices(graph);

	std::vector<PathRow> rows;
	for (size_t s = 0; s < normalized_starts.size(); s++) {
		uint64_t source = 0;
		if (!index.Find(normalized_starts[s], source)) {
			continue;
		}
		std::vector<double> distance(n, std::numeric_limits<double>::infinity());
		std::vector<uint64_t> predecessor(n);
		for (size_t i = 0; i < n; i++) {
			predecessor[i] = i;
		}

		// dag_shortest_paths topologically sorts first, so a cycle is fatal.
		try {
			boost::dag_shortest_paths(graph, source,
			                          boost::weight_map(boost::get(&RoutingEdge::cost, graph))
			                              .distance_map(&distance[0])
			                              .predecessor_map(&predecessor[0]));
		} catch (const boost::not_a_dag &) {
			throw duckdb::InvalidInputException(
			    "duckrouting_dag_shortest_path: the graph contains a cycle, so it is not a DAG");
		}

		for (size_t e = 0; e < normalized_ends.size(); e++) {
			if (normalized_ends[e] == normalized_starts[s]) {
				continue;
			}
			uint64_t sink = 0;
			if (!index.Find(normalized_ends[e], sink)) {
				continue;
			}
			auto path = BuildPath(graph, index, predecessor, source, sink, normalized_starts[s],
			                      normalized_ends[e]);
			rows.insert(rows.end(), path.begin(), path.end());
		}
	}
	return rows;
}


// ---------------------------------------------------------------------------
// DuckDB table function bindings
// ---------------------------------------------------------------------------

namespace {

using duckdb::BinderException;
using duckdb::ClientContext;
using duckdb::DataChunk;
using duckdb::FunctionData;
using duckdb::GlobalTableFunctionState;
using duckdb::idx_t;
using duckdb::LogicalType;
using duckdb::LogicalTypeId;
using duckdb::TableFunction;
using duckdb::TableFunctionBindInput;
using duckdb::TableFunctionInitInput;
using duckdb::TableFunctionInput;
using duckdb::TableFunctionSet;
using duckdb::Value;

struct SearchBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::vector<int64_t> roots;
	std::vector<int64_t> ends;
	bool directed = true;
	Traversal traversal = Traversal::Bfs;
	double limit = 0;
};

template <typename Row>
struct SearchGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

std::vector<int64_t> VertexIds(const Value &value, const char *what) {
	if (value.IsNull()) {
		throw BinderException("duckrouting: %s must not be NULL", what);
	}
	if (value.type().id() != LogicalTypeId::LIST) {
		return std::vector<int64_t>(1, value.GetValue<int64_t>());
	}
	std::vector<int64_t> ids;
	for (auto &child : duckdb::ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw BinderException("duckrouting: %s must not contain NULL", what);
		}
		ids.push_back(child.GetValue<int64_t>());
	}
	return ids;
}

bool NamedBool(TableFunctionBindInput &input, const char *name, bool fallback) {
	for (auto &parameter : input.named_parameters) {
		if (duckdb::StringUtil::CIEquals(parameter.first, name)) {
			if (parameter.second.IsNull()) {
				throw BinderException("duckrouting: '%s' must not be NULL", name);
			}
			return parameter.second.GetValue<bool>();
		}
	}
	return fallback;
}

template <Traversal Mode>
duckdb::unique_ptr<FunctionData> SearchBind(ClientContext &, TableFunctionBindInput &input,
                                            duckdb::vector<LogicalType> &return_types,
                                            duckdb::vector<std::string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<SearchBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->roots = VertexIds(input.inputs[1], "the root vertex");
	bind_data->traversal = Mode;
	bind_data->limit = input.inputs.size() > 2 && !input.inputs[2].IsNull()
	                       ? static_cast<double>(input.inputs[2].GetValue<int64_t>())
	                       : static_cast<double>(std::numeric_limits<int64_t>::max());
	for (auto &parameter : input.named_parameters) {
		if (duckdb::StringUtil::CIEquals(parameter.first, "max_depth")) {
			if (parameter.second.IsNull()) {
				throw BinderException("duckrouting: 'max_depth' must not be NULL");
			}
			bind_data->limit = static_cast<double>(parameter.second.GetValue<int64_t>());
		}
	}
	if (bind_data->limit < 0) {
		throw BinderException("duckrouting: 'max_depth' must not be negative");
	}
	bind_data->directed = NamedBool(input, "directed", true);

	names = {"seq", "depth", "start_vid", "pred", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> SearchInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SearchBindData>();
	auto state = duckdb::make_uniq<SearchGlobalState<DrivingDistanceRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = GraphTraversal(edges, bind_data.roots, bind_data.traversal, bind_data.limit,
	                             bind_data.directed);
	return std::move(state);
}

void SearchScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SearchGlobalState<DrivingDistanceRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.depth));
		output.SetValue(2, i, Value::BIGINT(row.start_vid));
		output.SetValue(3, i, Value::BIGINT(row.pred));
		output.SetValue(4, i, Value::BIGINT(row.node));
		output.SetValue(5, i, Value::BIGINT(row.edge));
		output.SetValue(6, i, Value::DOUBLE(row.cost));
		output.SetValue(7, i, Value::DOUBLE(row.agg_cost));
	}
	state.offset += count;
}

//! bellmanFord and dagShortestPath share pgRouting's dijkstra column shape.
template <bool IsDag>
duckdb::unique_ptr<FunctionData> PathBind(ClientContext &, TableFunctionBindInput &input,
                                          duckdb::vector<LogicalType> &return_types,
                                          duckdb::vector<std::string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<SearchBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->roots = VertexIds(input.inputs[1], "start_vid");
	bind_data->ends = VertexIds(input.inputs[2], "end_vid");
	bind_data->directed =
	    NamedBool(input, "directed", input.inputs.size() > 3 ? input.inputs[3].GetValue<bool>() : true);

	names = {"seq", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

template <bool IsDag>
duckdb::unique_ptr<GlobalTableFunctionState> PathInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SearchBindData>();
	auto state = duckdb::make_uniq<SearchGlobalState<PathRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = IsDag ? DagShortestPath(edges, bind_data.roots, bind_data.ends)
	                    : BellmanFord(edges, bind_data.roots, bind_data.ends, bind_data.directed);
	return std::move(state);
}

void PathScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SearchGlobalState<PathRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.path_seq));
		output.SetValue(2, i, Value::BIGINT(row.start_vid));
		output.SetValue(3, i, Value::BIGINT(row.end_vid));
		output.SetValue(4, i, Value::BIGINT(row.node));
		output.SetValue(5, i, Value::BIGINT(row.edge));
		output.SetValue(6, i, Value::DOUBLE(row.cost));
		output.SetValue(7, i, Value::DOUBLE(row.agg_cost));
	}
	state.offset += count;
}

template <Traversal Mode>
TableFunctionSet SearchSet(const char *name) {
	TableFunctionSet set(name);
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t root_is_list = 0; root_is_list < 2; root_is_list++) {
		for (size_t with_depth = 0; with_depth < 2; with_depth++) {
			duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR,
			                                       root_is_list ? list : LogicalType::BIGINT};
			if (with_depth) {
				arguments.push_back(LogicalType::BIGINT);
			}
			TableFunction function(arguments, SearchScan, SearchBind<Mode>, SearchInit);
			function.named_parameters["max_depth"] = LogicalType::BIGINT;
			function.named_parameters["directed"] = LogicalType::BOOLEAN;
			set.AddFunction(function);
		}
	}
	return set;
}

template <bool IsDag>
TableFunctionSet PathSet(const char *name, bool accepts_directed) {
	TableFunctionSet set(name);
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t end_is_list = 0; end_is_list < 2; end_is_list++) {
			const size_t variants = accepts_directed ? 2u : 1u;
			for (size_t with_flag = 0; with_flag < variants; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR,
				                                       start_is_list ? list : LogicalType::BIGINT,
				                                       end_is_list ? list : LogicalType::BIGINT};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, PathScan, PathBind<IsDag>, PathInit<IsDag>);
				if (accepts_directed) {
					function.named_parameters["directed"] = LogicalType::BOOLEAN;
				}
				set.AddFunction(function);
			}
		}
	}
	return set;
}

} // namespace

TableFunctionSet GetBreadthFirstSearchFunction() {
	return SearchSet<Traversal::Bfs>("duckrouting_breadth_first_search");
}

TableFunctionSet GetDepthFirstSearchFunction() {
	return SearchSet<Traversal::DfsDepth>("duckrouting_depth_first_search");
}

TableFunctionSet GetBellmanFordFunction() {
	return PathSet<false>("duckrouting_bellman_ford", true);
}

TableFunctionSet GetDagShortestPathFunction() {
	// A DAG is directed by definition, so there is no undirected variant.
	return PathSet<true>("duckrouting_dag_shortest_path", false);
}

} // namespace duckrouting
