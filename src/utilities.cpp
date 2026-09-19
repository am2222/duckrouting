#include "duckrouting/graph_functions.hpp"
#include "duckrouting/compat.hpp"

#include "duckrouting/graph.hpp"
#include "duckrouting/yen.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <boost/version.hpp>

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>

namespace duckrouting {

std::vector<VertexEdgesRow> ExtractVertices(const std::vector<EdgeRow> &edges) {
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(), [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });

	std::map<int64_t, VertexEdgesRow> vertices;
	for (size_t i = 0; i < ordered.size(); i++) {
		// A vertex is anything an edge mentions, whether or not it is usable.
		vertices[ordered[i].source].id = ordered[i].source;
		vertices[ordered[i].target].id = ordered[i].target;
		vertices[ordered[i].source].out_edges.push_back(ordered[i].id);
		vertices[ordered[i].target].in_edges.push_back(ordered[i].id);
	}

	std::vector<VertexEdgesRow> rows;
	for (auto entry = vertices.begin(); entry != vertices.end(); ++entry) {
		rows.push_back(entry->second);
	}
	return rows;
}

std::vector<DegreeRow> Degree(const std::vector<EdgeRow> &edges) {
	std::map<int64_t, int64_t> degree;
	for (size_t i = 0; i < edges.size(); i++) {
		// Degree counts incident edges, so a loop counts twice.
		degree[edges[i].source]++;
		degree[edges[i].target]++;
	}

	std::vector<DegreeRow> rows;
	for (auto entry = degree.begin(); entry != degree.end(); ++entry) {
		rows.push_back(DegreeRow {entry->first, entry->second});
	}
	return rows;
}

namespace {

std::vector<int64_t> Normalized(const std::vector<int64_t> &values) {
	std::vector<int64_t> result(values);
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

//! Turns parent maps into pgRouting's per-node rows.
std::vector<PathRow> Reconstruct(const std::vector<uint64_t> &parent, const std::vector<int64_t> &parent_edge,
                                 const std::vector<double> &parent_cost, const VertexIndex &index, uint64_t source,
                                 uint64_t sink, int64_t start_vid, int64_t end_vid) {
	if (sink != source && parent[sink] == sink) {
		return {};
	}
	std::vector<uint64_t> path;
	for (uint64_t at = sink;; at = parent[at]) {
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
			row.edge = parent_edge[path[i + 1]];
			row.cost = DecodeInfinity(parent_cost[path[i + 1]]);
			agg_cost += parent_cost[path[i + 1]];
		} else {
			row.edge = -1;
			row.cost = 0;
		}
		rows.push_back(row);
	}
	return rows;
}

//! Shared driver: `relax` runs one single-source search and fills the maps.
template <typename Relax>
std::vector<PathRow> SingleSourceDriver(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                        const std::vector<int64_t> &ends, bool directed, Relax relax) {
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(), [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });

	VertexIndex index;
	const Adjacency adjacency = BuildAdjacency(ordered, index, directed);
	const size_t n = index.Size();

	std::vector<PathRow> rows;
	for (size_t s = 0; s < starts.size(); s++) {
		uint64_t source = 0;
		if (!index.Find(starts[s], source)) {
			continue;
		}
		std::vector<double> distance(n, std::numeric_limits<double>::infinity());
		std::vector<uint64_t> parent(n);
		std::vector<int64_t> parent_edge(n, -1);
		std::vector<double> parent_cost(n, 0);
		for (size_t i = 0; i < n; i++) {
			parent[i] = i;
		}
		relax(adjacency, source, distance, parent, parent_edge, parent_cost);

		for (size_t e = 0; e < ends.size(); e++) {
			if (ends[e] == starts[s]) {
				continue;
			}
			uint64_t sink = 0;
			if (!index.Find(ends[e], sink)) {
				continue;
			}
			auto path = Reconstruct(parent, parent_edge, parent_cost, index, source, sink, starts[s], ends[e]);
			rows.insert(rows.end(), path.begin(), path.end());
		}
	}
	return rows;
}

} // namespace

std::vector<PathRow> EdwardMoore(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                 const std::vector<int64_t> &ends, bool directed) {
	return SingleSourceDriver(edges, Normalized(starts), Normalized(ends), directed,
	                          [](const Adjacency &adjacency, uint64_t source, std::vector<double> &distance,
	                             std::vector<uint64_t> &parent, std::vector<int64_t> &parent_edge,
	                             std::vector<double> &parent_cost) {
		                          // SPFA: Bellman-Ford that only revisits vertices whose distance
		                          // actually improved, held in a queue.
		                          std::vector<bool> queued(distance.size(), false);
		                          std::deque<uint64_t> pending;
		                          distance[source] = 0;
		                          pending.push_back(source);
		                          queued[source] = true;

		                          while (!pending.empty()) {
			                          const uint64_t at = pending.front();
			                          pending.pop_front();
			                          queued[at] = false;

			                          for (size_t i = 0; i < adjacency[at].size(); i++) {
				                          const Arc &arc = adjacency[at][i];
				                          const double candidate = distance[at] + arc.cost;
				                          if (candidate >= distance[arc.to]) {
					                          continue;
				                          }
				                          distance[arc.to] = candidate;
				                          parent[arc.to] = at;
				                          parent_edge[arc.to] = arc.edge;
				                          parent_cost[arc.to] = arc.cost;
				                          if (!queued[arc.to]) {
					                          pending.push_back(arc.to);
					                          queued[arc.to] = true;
				                          }
			                          }
		                          }
	                          });
}

std::vector<PathRow> BinaryBreadthFirstSearch(const std::vector<EdgeRow> &edges, const std::vector<int64_t> &starts,
                                              const std::vector<int64_t> &ends, bool directed) {
	return SingleSourceDriver(edges, Normalized(starts), Normalized(ends), directed,
	                          [](const Adjacency &adjacency, uint64_t source, std::vector<double> &distance,
	                             std::vector<uint64_t> &parent, std::vector<int64_t> &parent_edge,
	                             std::vector<double> &parent_cost) {
		                          // 0-1 BFS: a zero-cost edge goes to the front of the deque and a
		                          // unit-cost edge to the back, which keeps it in distance order
		                          // without a priority queue.
		                          std::deque<uint64_t> pending;
		                          distance[source] = 0;
		                          pending.push_back(source);

		                          while (!pending.empty()) {
			                          const uint64_t at = pending.front();
			                          pending.pop_front();

			                          for (size_t i = 0; i < adjacency[at].size(); i++) {
				                          const Arc &arc = adjacency[at][i];
				                          const double candidate = distance[at] + arc.cost;
				                          if (candidate >= distance[arc.to]) {
					                          continue;
				                          }
				                          distance[arc.to] = candidate;
				                          parent[arc.to] = at;
				                          parent_edge[arc.to] = arc.edge;
				                          parent_cost[arc.to] = arc.cost;
				                          if (arc.cost == 0) {
					                          pending.push_front(arc.to);
				                          } else {
					                          pending.push_back(arc.to);
				                          }
			                          }
		                          }
	                          });
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

struct UtilityBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::vector<int64_t> starts;
	std::vector<int64_t> ends;
	bool directed = true;
	AStarOptions options;
};

template <typename Row>
struct UtilityGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

std::vector<int64_t> Ids(const Value &value, const char *what) {
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

void ReadPathArguments(TableFunctionBindInput &input, UtilityBindData &bind_data) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	bind_data.edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data.starts = Ids(input.inputs[1], "start_vid");
	bind_data.ends = Ids(input.inputs[2], "end_vid");
	if (input.inputs.size() > 3 && !input.inputs[3].IsNull()) {
		bind_data.directed = input.inputs[3].GetValue<bool>();
	}
	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw BinderException("duckrouting: '%s' must not be NULL", parameter.first.c_str());
		}
		if (NameMatches(parameter.first, "directed")) {
			bind_data.directed = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "heuristic")) {
			const int64_t value = parameter.second.GetValue<int64_t>();
			if (value < 0 || value > 5) {
				throw BinderException("duckrouting: 'heuristic' must be between 0 and 5");
			}
			bind_data.options.heuristic = static_cast<Heuristic>(value);
		} else if (NameMatches(parameter.first, "factor")) {
			bind_data.options.factor = parameter.second.GetValue<double>();
		} else if (NameMatches(parameter.first, "epsilon")) {
			bind_data.options.epsilon = parameter.second.GetValue<double>();
			if (bind_data.options.epsilon < 1) {
				throw BinderException("duckrouting: 'epsilon' must be at least 1");
			}
		}
	}
	bind_data.options.directed = bind_data.directed;
}

void PathColumns(duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	names = {"seq", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
}

void PathScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<UtilityGlobalState<PathRow>>();
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

void CostColumns(duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	names = {"start_vid", "end_vid", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
}

void CostRowScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<UtilityGlobalState<CostRow>>();
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

// --- the four path-shaped functions -----------------------------------------

enum class PathAlgorithm { BdDijkstra, BdAStar, EdwardMooreAlgorithm, BinaryBfs };

template <PathAlgorithm Algorithm, bool CostsOnly>
duckdb::unique_ptr<FunctionData> PathBind(ClientContext &, TableFunctionBindInput &input,
                                          duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	auto bind_data = duckdb::make_uniq<UtilityBindData>();
	ReadPathArguments(input, *bind_data);
	if (CostsOnly) {
		CostColumns(return_types, names);
	} else {
		PathColumns(return_types, names);
	}
	return std::move(bind_data);
}

//! The matrix forms route one vertex list against itself.
template <PathAlgorithm Algorithm>
duckdb::unique_ptr<FunctionData> MatrixBind(ClientContext &, TableFunctionBindInput &input,
                                            duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<UtilityBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->starts = Ids(input.inputs[1], "vids");
	bind_data->ends = bind_data->starts;
	if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
		bind_data->directed = input.inputs[2].GetValue<bool>();
	}
	for (auto &parameter : input.named_parameters) {
		if (!parameter.second.IsNull() && NameMatches(parameter.first, "directed")) {
			bind_data->directed = parameter.second.GetValue<bool>();
		}
	}
	bind_data->options.directed = bind_data->directed;
	CostColumns(return_types, names);
	return std::move(bind_data);
}

template <PathAlgorithm Algorithm>
duckdb::unique_ptr<GlobalTableFunctionState> PathInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<UtilityBindData>();
	auto state = duckdb::make_uniq<UtilityGlobalState<PathRow>>();
	if (Algorithm == PathAlgorithm::BdAStar) {
		auto edges = LoadCoordinateEdges(context, bind_data.edges_sql);
		state->rows = BidirectionalAStar(edges, bind_data.starts, bind_data.ends, bind_data.options);
	} else {
		auto edges = LoadEdges(context, bind_data.edges_sql);
		if (Algorithm == PathAlgorithm::BdDijkstra) {
			state->rows = BidirectionalDijkstra(edges, bind_data.starts, bind_data.ends, bind_data.directed);
		} else if (Algorithm == PathAlgorithm::EdwardMooreAlgorithm) {
			state->rows = EdwardMoore(edges, bind_data.starts, bind_data.ends, bind_data.directed);
		} else {
			state->rows = BinaryBreadthFirstSearch(edges, bind_data.starts, bind_data.ends, bind_data.directed);
		}
	}
	return std::move(state);
}

template <PathAlgorithm Algorithm>
duckdb::unique_ptr<GlobalTableFunctionState> CostInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<UtilityBindData>();
	auto state = duckdb::make_uniq<UtilityGlobalState<CostRow>>();
	if (Algorithm == PathAlgorithm::BdAStar) {
		auto edges = LoadCoordinateEdges(context, bind_data.edges_sql);
		state->rows = BidirectionalAStarCost(edges, bind_data.starts, bind_data.ends, bind_data.options);
	} else {
		auto edges = LoadEdges(context, bind_data.edges_sql);
		state->rows = BidirectionalDijkstraCost(edges, bind_data.starts, bind_data.ends, bind_data.directed);
	}
	return std::move(state);
}

// --- extract_vertices, degree, full_version ---------------------------------

duckdb::unique_ptr<FunctionData> VerticesBind(ClientContext &, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<UtilityBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	names = {"id", "in_edges", "out_edges"};
	return_types = {LogicalType::BIGINT, LogicalType::LIST(LogicalType::BIGINT),
	                LogicalType::LIST(LogicalType::BIGINT)};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> VerticesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<UtilityBindData>();
	auto state = duckdb::make_uniq<UtilityGlobalState<VertexEdgesRow>>();
	// Only id, source and target are read, so a cost column is not required.
	state->rows = ExtractVertices(LoadEdges(context, bind_data.edges_sql, true, false));
	return std::move(state);
}

void VerticesScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<UtilityGlobalState<VertexEdgesRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(row.id));
		duckdb::vector<Value> incoming, outgoing;
		for (size_t e = 0; e < row.in_edges.size(); e++) {
			incoming.push_back(Value::BIGINT(row.in_edges[e]));
		}
		for (size_t e = 0; e < row.out_edges.size(); e++) {
			outgoing.push_back(Value::BIGINT(row.out_edges[e]));
		}
		output.SetValue(1, i, Value::LIST(LogicalType::BIGINT, incoming));
		output.SetValue(2, i, Value::LIST(LogicalType::BIGINT, outgoing));
	}
	state.offset += count;
}

duckdb::unique_ptr<FunctionData> DegreeBind(ClientContext &, TableFunctionBindInput &input,
                                            duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<UtilityBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	names = {"node", "degree"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> DegreeInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<UtilityBindData>();
	auto state = duckdb::make_uniq<UtilityGlobalState<DegreeRow>>();
	// Degree is structural too.
	state->rows = Degree(LoadEdges(context, bind_data.edges_sql, true, false));
	return std::move(state);
}

void DegreeScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<UtilityGlobalState<DegreeRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(row.node));
		output.SetValue(1, i, Value::BIGINT(row.degree));
	}
	state.offset += count;
}

struct VersionState : public GlobalTableFunctionState {
	bool emitted = false;

	idx_t MaxThreads() const override {
		return 1;
	}
};

duckdb::unique_ptr<FunctionData> FullVersionBind(ClientContext &, TableFunctionBindInput &,
                                                 duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	names = {"version", "boost", "compiler", "build_type"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return nullptr;
}

duckdb::unique_ptr<GlobalTableFunctionState> FullVersionInit(ClientContext &, TableFunctionInitInput &) {
	return duckdb::make_uniq<VersionState>();
}

void FullVersionScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<VersionState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	state.emitted = true;
	output.SetCardinality(1);
#ifdef EXT_VERSION_DUCKROUTING
	output.SetValue(0, 0, Value(EXT_VERSION_DUCKROUTING));
#else
	output.SetValue(0, 0, Value("unknown"));
#endif
	output.SetValue(1, 0, Value(BOOST_LIB_VERSION));
#ifdef __clang_version__
	output.SetValue(2, 0, Value(std::string("clang ") + __clang_version__));
#else
	output.SetValue(2, 0, Value("unknown"));
#endif
#ifdef NDEBUG
	output.SetValue(3, 0, Value("release"));
#else
	output.SetValue(3, 0, Value("debug"));
#endif
}

//! Registers the scalar/array spellings of a path-shaped function.
template <PathAlgorithm Algorithm, bool CostsOnly>
TableFunctionSet PathSet(const char *name, bool with_astar_options) {
	TableFunctionSet set(name);
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t end_is_list = 0; end_is_list < 2; end_is_list++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, start_is_list ? list : LogicalType::BIGINT,
				                                       end_is_list ? list : LogicalType::BIGINT};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, CostsOnly ? CostRowScan : PathScan, PathBind<Algorithm, CostsOnly>,
				                       CostsOnly ? CostInit<Algorithm> : PathInit<Algorithm>);
				function.named_parameters["directed"] = LogicalType::BOOLEAN;
				if (with_astar_options) {
					function.named_parameters["heuristic"] = LogicalType::BIGINT;
					function.named_parameters["factor"] = LogicalType::DOUBLE;
					function.named_parameters["epsilon"] = LogicalType::DOUBLE;
				}
				set.AddFunction(function);
			}
		}
	}
	return set;
}

template <PathAlgorithm Algorithm>
TableFunctionSet MatrixSet(const char *name) {
	TableFunctionSet set(name);
	for (size_t with_flag = 0; with_flag < 2; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::BIGINT)};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, CostRowScan, MatrixBind<Algorithm>, CostInit<Algorithm>);
		function.named_parameters["directed"] = LogicalType::BOOLEAN;
		set.AddFunction(function);
	}
	return set;
}

} // namespace

TableFunctionSet GetBdDijkstraFunction() {
	return PathSet<PathAlgorithm::BdDijkstra, false>("duckrouting_bd_dijkstra", false);
}
TableFunctionSet GetBdDijkstraCostFunction() {
	return PathSet<PathAlgorithm::BdDijkstra, true>("duckrouting_bd_dijkstra_cost", false);
}
TableFunctionSet GetBdDijkstraCostMatrixFunction() {
	return MatrixSet<PathAlgorithm::BdDijkstra>("duckrouting_bd_dijkstra_cost_matrix");
}
TableFunctionSet GetBdAStarFunction() {
	return PathSet<PathAlgorithm::BdAStar, false>("duckrouting_bd_astar", true);
}
TableFunctionSet GetBdAStarCostFunction() {
	return PathSet<PathAlgorithm::BdAStar, true>("duckrouting_bd_astar_cost", true);
}
TableFunctionSet GetBdAStarCostMatrixFunction() {
	return MatrixSet<PathAlgorithm::BdAStar>("duckrouting_bd_astar_cost_matrix");
}
TableFunctionSet GetEdwardMooreFunction() {
	return PathSet<PathAlgorithm::EdwardMooreAlgorithm, false>("duckrouting_edward_moore", false);
}
TableFunctionSet GetBinaryBreadthFirstSearchFunction() {
	return PathSet<PathAlgorithm::BinaryBfs, false>("duckrouting_binary_breadth_first_search", false);
}

TableFunctionSet GetExtractVerticesFunction() {
	TableFunctionSet set("duckrouting_extract_vertices");
	set.AddFunction(TableFunction({LogicalType::VARCHAR}, VerticesScan, VerticesBind, VerticesInit));
	return set;
}

TableFunctionSet GetDegreeFunction() {
	TableFunctionSet set("duckrouting_degree");
	set.AddFunction(TableFunction({LogicalType::VARCHAR}, DegreeScan, DegreeBind, DegreeInit));
	return set;
}

TableFunctionSet GetFullVersionFunction() {
	TableFunctionSet set("duckrouting_full_version");
	set.AddFunction(TableFunction({}, FullVersionScan, FullVersionBind, FullVersionInit));
	return set;
}

} // namespace duckrouting
