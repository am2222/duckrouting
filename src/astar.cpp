#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <boost/graph/astar_search.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace duckrouting {

namespace {

//! A* needs to know where each vertex is, so unlike the routing graph this one
//! carries a coordinate per vertex.
struct PlacedVertex {
	double x = 0;
	double y = 0;
};

typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, PlacedVertex, RoutingEdge> AStarDirectedGraph;
typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS, PlacedVertex, RoutingEdge>
    AStarUndirectedGraph;

//! Thrown to stop the search once every goal has been settled.
struct GoalsReached {};

template <typename Vertex>
struct GoalVisitor : public boost::default_astar_visitor {
	std::set<Vertex> *pending;

	template <typename Graph>
	void examine_vertex(Vertex vertex, const Graph &) {
		pending->erase(vertex);
		if (pending->empty()) {
			// Nothing left to find, so stop rather than explore the rest.
			throw GoalsReached();
		}
	}
};

//! pgRouting's heuristics, verbatim. Each estimates the remaining distance from
//! `u` to the nearest goal; the estimate must never exceed the true remaining
//! cost or the result stops being a shortest path.
template <typename Graph>
struct DistanceHeuristic : public boost::astar_heuristic<Graph, double> {
	const Graph *graph;
	const std::set<typename boost::graph_traits<Graph>::vertex_descriptor> *goals;
	Heuristic heuristic;
	double factor;

	double operator()(typename boost::graph_traits<Graph>::vertex_descriptor u) {
		if (heuristic == Heuristic::None || goals->empty()) {
			return 0;
		}
		double best = (std::numeric_limits<double>::max)();
		for (auto goal = goals->begin(); goal != goals->end(); ++goal) {
			const double dx = (*graph)[*goal].x - (*graph)[u].x;
			const double dy = (*graph)[*goal].y - (*graph)[u].y;
			double current = 0;
			switch (heuristic) {
			case Heuristic::MaxDelta:
				current = std::fabs((std::max)(dx, dy)) * factor;
				break;
			case Heuristic::MinDelta:
				current = std::fabs((std::min)(dx, dy)) * factor;
				break;
			case Heuristic::SquaredEuclidean:
				current = (dx * dx + dy * dy) * factor * factor;
				break;
			case Heuristic::Euclidean:
				current = std::sqrt(dx * dx + dy * dy) * factor;
				break;
			case Heuristic::Manhattan:
				current = (std::fabs(dx) + std::fabs(dy)) * factor;
				break;
			case Heuristic::None:
			default:
				current = 0;
			}
			best = (std::min)(best, current);
		}
		return best;
	}
};

//! Builds the A* graph. A vertex takes its coordinates from whichever edge
//! first mentions it; pgRouting's sample data is consistent about this.
template <typename Graph>
Graph BuildPlacedGraph(const std::vector<CoordinateEdgeRow> &edges, VertexIndex &index) {
	for (size_t i = 0; i < edges.size(); i++) {
		index.GetOrCreate(edges[i].edge.source);
		index.GetOrCreate(edges[i].edge.target);
	}

	Graph graph(index.Size());
	std::vector<bool> placed(index.Size(), false);
	for (size_t i = 0; i < edges.size(); i++) {
		const EdgeRow &edge = edges[i].edge;
		uint64_t source = 0;
		uint64_t target = 0;
		index.Find(edge.source, source);
		index.Find(edge.target, target);

		if (!placed[source]) {
			graph[source].x = edges[i].x1;
			graph[source].y = edges[i].y1;
			placed[source] = true;
		}
		if (!placed[target]) {
			graph[target].x = edges[i].x2;
			graph[target].y = edges[i].y2;
			placed[target] = true;
		}

		if (IsTraversable(edge.cost)) {
			boost::add_edge(source, target, RoutingEdge {edge.id, edge.cost}, graph);
		}
		if (IsTraversable(edge.reverse_cost)) {
			boost::add_edge(target, source, RoutingEdge {edge.id, edge.reverse_cost}, graph);
		}
	}
	return graph;
}

template <typename Graph>
bool CheapestEdge(const Graph &graph, uint64_t from, uint64_t to, RoutingEdge &result) {
	bool found = false;
	typename boost::graph_traits<Graph>::out_edge_iterator it, end;
	for (boost::tie(it, end) = boost::out_edges(from, graph); it != end; ++it) {
		if (static_cast<uint64_t>(boost::target(*it, graph)) != to) {
			continue;
		}
		if (!found || graph[*it].cost < result.cost) {
			result = graph[*it];
			found = true;
		}
	}
	return found;
}

std::vector<int64_t> Normalized(const std::vector<int64_t> &values) {
	std::vector<int64_t> result(values);
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

//! Runs one A* search from `source` and returns the predecessor/distance maps.
template <typename Graph>
void RunAStar(const Graph &graph, const VertexIndex &index, uint64_t source, const std::vector<int64_t> &ends,
              const AStarOptions &options, std::vector<uint64_t> &predecessor, std::vector<double> &distance) {
	typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
	const size_t n = boost::num_vertices(graph);

	predecessor.assign(n, 0);
	for (size_t i = 0; i < n; i++) {
		predecessor[i] = i;
	}
	distance.assign(n, std::numeric_limits<double>::infinity());

	std::set<Vertex> goals;
	for (size_t i = 0; i < ends.size(); i++) {
		uint64_t sink = 0;
		if (index.Find(ends[i], sink)) {
			goals.insert(static_cast<Vertex>(sink));
		}
	}
	if (goals.empty()) {
		return;
	}

	DistanceHeuristic<Graph> estimate;
	estimate.graph = &graph;
	estimate.goals = &goals;
	estimate.heuristic = options.heuristic;
	// pgRouting folds epsilon into the factor rather than applying it separately.
	estimate.factor = options.factor * options.epsilon;

	std::set<Vertex> pending(goals);
	GoalVisitor<Vertex> visitor;
	visitor.pending = &pending;

	std::vector<boost::default_color_type> color(n);
	std::vector<double> rank(n);

	try {
		boost::astar_search(
		    graph, static_cast<Vertex>(source), estimate,
		    boost::predecessor_map(
		        boost::make_iterator_property_map(predecessor.begin(), boost::get(boost::vertex_index, graph)))
		        .distance_map(
		            boost::make_iterator_property_map(distance.begin(), boost::get(boost::vertex_index, graph)))
		        .weight_map(boost::get(&RoutingEdge::cost, graph))
		        .rank_map(boost::make_iterator_property_map(rank.begin(), boost::get(boost::vertex_index, graph)))
		        .color_map(boost::make_iterator_property_map(color.begin(), boost::get(boost::vertex_index, graph)))
		        .visitor(visitor)
		        .distance_inf(std::numeric_limits<double>::infinity()));
	} catch (const GoalsReached &) {
		// Every goal was settled, which is the point of the visitor.
	}
}

template <typename Graph>
std::vector<PathRow> AStarImpl(const std::vector<CoordinateEdgeRow> &edges, const std::vector<int64_t> &starts,
                               const std::vector<int64_t> &ends, const AStarOptions &options, bool costs_only,
                               std::vector<CostRow> *costs) {
	VertexIndex index;
	auto graph = BuildPlacedGraph<Graph>(edges, index);

	std::vector<PathRow> rows;
	std::vector<uint64_t> predecessor;
	std::vector<double> distance;

	for (size_t s = 0; s < starts.size(); s++) {
		uint64_t source = 0;
		if (!index.Find(starts[s], source)) {
			continue;
		}
		RunAStar(graph, index, source, ends, options, predecessor, distance);

		for (size_t e = 0; e < ends.size(); e++) {
			if (ends[e] == starts[s]) {
				continue;
			}
			uint64_t sink = 0;
			if (!index.Find(ends[e], sink) || predecessor[sink] == sink) {
				continue;
			}
			if (costs_only) {
				costs->push_back(CostRow {starts[s], ends[e], DecodeInfinity(distance[sink])});
				continue;
			}

			std::vector<uint64_t> path;
			for (uint64_t at = sink;; at = predecessor[at]) {
				path.push_back(at);
				if (at == source) {
					break;
				}
			}
			std::reverse(path.begin(), path.end());

			double agg_cost = 0;
			for (size_t i = 0; i < path.size(); i++) {
				PathRow row {};
				row.path_seq = static_cast<int64_t>(i) + 1;
				row.start_vid = starts[s];
				row.end_vid = ends[e];
				row.node = index.IdOf(path[i]);
				row.agg_cost = DecodeInfinity(agg_cost);
				if (i + 1 < path.size()) {
					RoutingEdge used {};
					if (!CheapestEdge(graph, path[i], path[i + 1], used)) {
						break;
					}
					row.edge = used.id;
					row.cost = DecodeInfinity(used.cost);
					agg_cost += used.cost;
				} else {
					row.edge = -1;
					row.cost = 0;
				}
				rows.push_back(row);
			}
		}
	}
	return rows;
}

} // namespace

std::vector<PathRow> AStar(const std::vector<CoordinateEdgeRow> &edges, const std::vector<int64_t> &starts,
                           const std::vector<int64_t> &ends, const AStarOptions &options) {
	const auto s = Normalized(starts);
	const auto e = Normalized(ends);
	return options.directed ? AStarImpl<AStarDirectedGraph>(edges, s, e, options, false, nullptr)
	                        : AStarImpl<AStarUndirectedGraph>(edges, s, e, options, false, nullptr);
}

std::vector<CostRow> AStarCost(const std::vector<CoordinateEdgeRow> &edges, const std::vector<int64_t> &starts,
                               const std::vector<int64_t> &ends, const AStarOptions &options) {
	const auto s = Normalized(starts);
	const auto e = Normalized(ends);
	std::vector<CostRow> costs;
	if (options.directed) {
		AStarImpl<AStarDirectedGraph>(edges, s, e, options, true, &costs);
	} else {
		AStarImpl<AStarUndirectedGraph>(edges, s, e, options, true, &costs);
	}
	return costs;
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

struct AStarBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::vector<int64_t> starts;
	std::vector<int64_t> ends;
	AStarOptions options;
};

template <typename Row>
struct AStarGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

std::vector<int64_t> AStarVertexIds(const Value &value, const char *what) {
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

void ReadOptions(TableFunctionBindInput &input, AStarOptions &options) {
	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw BinderException("duckrouting: '%s' must not be NULL", parameter.first.c_str());
		}
		if (duckdb::StringUtil::CIEquals(parameter.first, "directed")) {
			options.directed = parameter.second.GetValue<bool>();
		} else if (duckdb::StringUtil::CIEquals(parameter.first, "heuristic")) {
			const int64_t value = parameter.second.GetValue<int64_t>();
			if (value < 0 || value > 5) {
				throw BinderException("duckrouting: 'heuristic' must be between 0 and 5");
			}
			options.heuristic = static_cast<Heuristic>(value);
		} else if (duckdb::StringUtil::CIEquals(parameter.first, "factor")) {
			options.factor = parameter.second.GetValue<double>();
		} else if (duckdb::StringUtil::CIEquals(parameter.first, "epsilon")) {
			options.epsilon = parameter.second.GetValue<double>();
			// An epsilon below 1 would shrink the heuristic, which pgRouting
			// rejects rather than silently ignore.
			if (options.epsilon < 1) {
				throw BinderException("duckrouting: 'epsilon' must be at least 1");
			}
		}
	}
}

//! (seq, path_seq, start_vid, end_vid, node, edge, cost, agg_cost)
duckdb::unique_ptr<FunctionData> AStarBind(ClientContext &, TableFunctionBindInput &input,
                                           duckdb::vector<LogicalType> &return_types,
                                           duckdb::vector<std::string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<AStarBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->starts = AStarVertexIds(input.inputs[1], "start_vid");
	bind_data->ends = AStarVertexIds(input.inputs[2], "end_vid");
	if (input.inputs.size() > 3 && !input.inputs[3].IsNull()) {
		bind_data->options.directed = input.inputs[3].GetValue<bool>();
	}
	ReadOptions(input, bind_data->options);

	names = {"seq", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> AStarInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<AStarBindData>();
	auto state = duckdb::make_uniq<AStarGlobalState<PathRow>>();
	auto edges = LoadCoordinateEdges(context, bind_data.edges_sql);
	state->rows = AStar(edges, bind_data.starts, bind_data.ends, bind_data.options);
	return std::move(state);
}

void AStarScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AStarGlobalState<PathRow>>();
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

//! (start_vid, end_vid, agg_cost) -- the cost and matrix forms.
duckdb::unique_ptr<FunctionData> AStarCostBind(ClientContext &context, TableFunctionBindInput &input,
                                               duckdb::vector<LogicalType> &return_types,
                                               duckdb::vector<std::string> &names) {
	AStarBind(context, input, return_types, names);
	auto bind_data = duckdb::make_uniq<AStarBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->starts = AStarVertexIds(input.inputs[1], "start_vid");
	bind_data->ends = AStarVertexIds(input.inputs[2], "end_vid");
	if (input.inputs.size() > 3 && !input.inputs[3].IsNull()) {
		bind_data->options.directed = input.inputs[3].GetValue<bool>();
	}
	ReadOptions(input, bind_data->options);

	names = {"start_vid", "end_vid", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

//! The matrix form routes one vertex list against itself.
duckdb::unique_ptr<FunctionData> AStarMatrixBind(ClientContext &, TableFunctionBindInput &input,
                                                 duckdb::vector<LogicalType> &return_types,
                                                 duckdb::vector<std::string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<AStarBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->starts = AStarVertexIds(input.inputs[1], "vids");
	bind_data->ends = bind_data->starts;
	if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
		bind_data->options.directed = input.inputs[2].GetValue<bool>();
	}
	ReadOptions(input, bind_data->options);

	names = {"start_vid", "end_vid", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> AStarCostInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<AStarBindData>();
	auto state = duckdb::make_uniq<AStarGlobalState<CostRow>>();
	auto edges = LoadCoordinateEdges(context, bind_data.edges_sql);
	state->rows = AStarCost(edges, bind_data.starts, bind_data.ends, bind_data.options);
	return std::move(state);
}

void AStarCostScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AStarGlobalState<CostRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(row.start_vid));
		output.SetValue(1, i, Value::BIGINT(row.end_vid));
		output.SetValue(2, i, Value::DOUBLE(row.agg_cost));
	}
	state.offset += count;
}

void AddAStarOptions(TableFunction &function) {
	function.named_parameters["directed"] = LogicalType::BOOLEAN;
	function.named_parameters["heuristic"] = LogicalType::BIGINT;
	function.named_parameters["factor"] = LogicalType::DOUBLE;
	function.named_parameters["epsilon"] = LogicalType::DOUBLE;
}

} // namespace

TableFunctionSet GetAStarFunction() {
	TableFunctionSet set("duckrouting_astar");
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t end_is_list = 0; end_is_list < 2; end_is_list++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, start_is_list ? list : LogicalType::BIGINT,
				                                       end_is_list ? list : LogicalType::BIGINT};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, AStarScan, AStarBind, AStarInit);
				AddAStarOptions(function);
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetAStarCostFunction() {
	TableFunctionSet set("duckrouting_astar_cost");
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t end_is_list = 0; end_is_list < 2; end_is_list++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, start_is_list ? list : LogicalType::BIGINT,
				                                       end_is_list ? list : LogicalType::BIGINT};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, AStarCostScan, AStarCostBind, AStarCostInit);
				AddAStarOptions(function);
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetAStarCostMatrixFunction() {
	TableFunctionSet set("duckrouting_astar_cost_matrix");
	for (size_t with_flag = 0; with_flag < 2; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::BIGINT)};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, AStarCostScan, AStarMatrixBind, AStarCostInit);
		AddAStarOptions(function);
		set.AddFunction(function);
	}
	return set;
}

} // namespace duckrouting
