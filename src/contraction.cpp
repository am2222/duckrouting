#include "duckrouting/graph_functions.hpp"
#include "duckrouting/compat.hpp"

#include "duckrouting/graph.hpp"
#include "duckrouting/yen.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <cctype>
#include <set>

namespace duckrouting {

namespace {

//! One arc of the graph being contracted. A shortcut remembers which vertices
//! it stands in for, so the result can be unpacked back into real edges.
struct ContractionArc {
	uint64_t to;
	double cost;
	std::vector<int64_t> contracted;
};

typedef std::vector<std::vector<ContractionArc>> ContractionAdjacency;

//! Shortest path from `from` to `to` that avoids `banned`, stopping once the
//! distance exceeds `limit`. Used as the witness search: if a route already
//! exists that is no worse than going through the vertex being contracted,
//! no shortcut is needed.
bool WitnessExists(const ContractionAdjacency &outgoing, const std::vector<bool> &removed, uint64_t from, uint64_t to,
                   uint64_t banned, double limit) {
	if (from == to) {
		return true;
	}
	std::vector<double> distance(outgoing.size(), std::numeric_limits<double>::infinity());
	typedef std::pair<double, uint64_t> Entry;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
	distance[from] = 0;
	queue.push(Entry(0, from));

	while (!queue.empty()) {
		const double at_cost = queue.top().first;
		const uint64_t at = queue.top().second;
		queue.pop();
		if (at_cost > distance[at] || at_cost > limit) {
			continue;
		}
		if (at == to) {
			return at_cost <= limit;
		}
		for (size_t i = 0; i < outgoing[at].size(); i++) {
			const ContractionArc &arc = outgoing[at][i];
			if (removed[arc.to] || arc.to == banned) {
				continue;
			}
			const double candidate = at_cost + arc.cost;
			if (candidate < distance[arc.to]) {
				distance[arc.to] = candidate;
				queue.push(Entry(candidate, arc.to));
			}
		}
	}
	return false;
}

//! Cheapest arc from `from` to `to`, or false when there is none.
bool BestArc(const ContractionAdjacency &adjacency, uint64_t from, uint64_t to, ContractionArc &result) {
	bool found = false;
	for (size_t i = 0; i < adjacency[from].size(); i++) {
		if (adjacency[from][i].to != to) {
			continue;
		}
		if (!found || adjacency[from][i].cost < result.cost) {
			result = adjacency[from][i];
			found = true;
		}
	}
	return found;
}

//! The shortcuts contracting `vertex` would require. Returning them without
//! applying them is what lets the same routine serve both the priority
//! calculation and the contraction itself.
std::vector<ContractionArc> ShortcutsFor(const ContractionAdjacency &outgoing, const ContractionAdjacency &incoming,
                                         const std::vector<bool> &removed, const VertexIndex &index, uint64_t vertex,
                                         std::vector<uint64_t> &shortcut_sources) {
	std::vector<ContractionArc> shortcuts;
	shortcut_sources.clear();

	std::set<uint64_t> predecessors;
	for (size_t i = 0; i < incoming[vertex].size(); i++) {
		if (!removed[incoming[vertex][i].to] && incoming[vertex][i].to != vertex) {
			predecessors.insert(incoming[vertex][i].to);
		}
	}
	std::set<uint64_t> successors;
	for (size_t i = 0; i < outgoing[vertex].size(); i++) {
		if (!removed[outgoing[vertex][i].to] && outgoing[vertex][i].to != vertex) {
			successors.insert(outgoing[vertex][i].to);
		}
	}

	for (auto u = predecessors.begin(); u != predecessors.end(); ++u) {
		ContractionArc to_vertex {};
		if (!BestArc(outgoing, *u, vertex, to_vertex)) {
			continue;
		}
		for (auto v = successors.begin(); v != successors.end(); ++v) {
			if (*u == *v) {
				continue;
			}
			ContractionArc from_vertex {};
			if (!BestArc(outgoing, vertex, *v, from_vertex)) {
				continue;
			}
			const double through = to_vertex.cost + from_vertex.cost;
			// Only add a shortcut when no route of equal or lower cost already
			// avoids this vertex.
			if (WitnessExists(outgoing, removed, *u, *v, vertex, through)) {
				continue;
			}

			ContractionArc shortcut {};
			shortcut.to = *v;
			shortcut.cost = through;
			shortcut.contracted = to_vertex.contracted;
			shortcut.contracted.push_back(index.IdOf(vertex));
			shortcut.contracted.insert(shortcut.contracted.end(), from_vertex.contracted.begin(),
			                           from_vertex.contracted.end());
			shortcuts.push_back(shortcut);
			shortcut_sources.push_back(*u);
		}
	}
	return shortcuts;
}

int64_t CountIncident(const ContractionAdjacency &outgoing, const ContractionAdjacency &incoming,
                      const std::vector<bool> &removed, uint64_t vertex) {
	int64_t total = 0;
	for (size_t i = 0; i < outgoing[vertex].size(); i++) {
		if (!removed[outgoing[vertex][i].to]) {
			total++;
		}
	}
	for (size_t i = 0; i < incoming[vertex].size(); i++) {
		if (!removed[incoming[vertex][i].to]) {
			total++;
		}
	}
	return total;
}

} // namespace

std::vector<ContractionRow> ContractionHierarchies(const std::vector<EdgeRow> &edges, bool directed,
                                                   const std::vector<int64_t> &forbidden) {
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(), [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });

	VertexIndex index;
	for (size_t i = 0; i < ordered.size(); i++) {
		index.GetOrCreate(ordered[i].source);
		index.GetOrCreate(ordered[i].target);
	}
	const size_t n = index.Size();
	if (n == 0) {
		return {};
	}

	ContractionAdjacency outgoing(n);
	ContractionAdjacency incoming(n);
	auto connect = [&](uint64_t from, uint64_t to, double cost) {
		outgoing[from].push_back(ContractionArc {to, cost, std::vector<int64_t>()});
		incoming[to].push_back(ContractionArc {from, cost, std::vector<int64_t>()});
	};

	for (size_t i = 0; i < ordered.size(); i++) {
		uint64_t source = 0;
		uint64_t target = 0;
		index.Find(ordered[i].source, source);
		index.Find(ordered[i].target, target);
		if (IsTraversable(ordered[i].cost)) {
			connect(source, target, ordered[i].cost);
			if (!directed) {
				connect(target, source, ordered[i].cost);
			}
		}
		if (IsTraversable(ordered[i].reverse_cost)) {
			connect(target, source, ordered[i].reverse_cost);
			if (!directed) {
				connect(source, target, ordered[i].reverse_cost);
			}
		}
	}

	std::set<uint64_t> off_limits;
	for (size_t i = 0; i < forbidden.size(); i++) {
		uint64_t v = 0;
		if (index.Find(forbidden[i], v)) {
			off_limits.insert(v);
		}
	}

	std::vector<bool> removed(n, false);
	std::vector<int64_t> metric(n, -1);
	std::vector<int64_t> order(n, -1);
	std::vector<ContractionRow> shortcut_rows;
	int64_t next_order = 1;
	int64_t next_shortcut_id = -1;

	// Contract greedily by edge difference: the vertex whose removal adds the
	// fewest shortcuts relative to the edges it takes away goes first. The
	// priority is recomputed lazily, since contracting one vertex changes its
	// neighbours' scores.
	while (true) {
		bool contracted_any = false;
		double best_score = 0;
		uint64_t best_vertex = 0;
		std::vector<ContractionArc> best_shortcuts;
		std::vector<uint64_t> best_sources;

		for (uint64_t v = 0; v < n; v++) {
			if (removed[v] || off_limits.count(v)) {
				continue;
			}
			std::vector<uint64_t> sources;
			auto shortcuts = ShortcutsFor(outgoing, incoming, removed, index, v, sources);
			const double score = static_cast<double>(shortcuts.size()) -
			                     static_cast<double>(CountIncident(outgoing, incoming, removed, v));
			if (!contracted_any || score < best_score) {
				contracted_any = true;
				best_score = score;
				best_vertex = v;
				best_shortcuts = shortcuts;
				best_sources = sources;
			}
		}
		if (!contracted_any) {
			break;
		}

		std::set<std::pair<uint64_t, uint64_t>> reported;
		for (size_t i = 0; i < best_shortcuts.size(); i++) {
			outgoing[best_sources[i]].push_back(best_shortcuts[i]);
			ContractionArc back {};
			back.to = best_sources[i];
			back.cost = best_shortcuts[i].cost;
			back.contracted = best_shortcuts[i].contracted;
			incoming[best_shortcuts[i].to].push_back(back);

			// An undirected contraction produces the same shortcut in both
			// directions; pgRouting reports each one once.
			if (!directed) {
				const std::pair<uint64_t, uint64_t> key = best_sources[i] < best_shortcuts[i].to
				                                              ? std::make_pair(best_sources[i], best_shortcuts[i].to)
				                                              : std::make_pair(best_shortcuts[i].to, best_sources[i]);
				if (reported.count(key)) {
					continue;
				}
				reported.insert(key);
			}

			ContractionRow row {};
			row.is_vertex = false;
			row.id = next_shortcut_id--;
			row.contracted_vertices = best_shortcuts[i].contracted;
			row.source = index.IdOf(best_sources[i]);
			row.target = index.IdOf(best_shortcuts[i].to);
			row.cost = DecodeInfinity(best_shortcuts[i].cost);
			row.metric = -1;
			row.vertex_order = -1;
			shortcut_rows.push_back(row);
		}

		removed[best_vertex] = true;
		metric[best_vertex] = static_cast<int64_t>(best_score);
		order[best_vertex] = next_order++;
	}

	std::vector<ContractionRow> rows;
	for (uint64_t v = 0; v < n; v++) {
		ContractionRow row {};
		row.is_vertex = true;
		row.id = index.IdOf(v);
		row.source = -1;
		row.target = -1;
		row.cost = -1;
		row.metric = metric[v];
		row.vertex_order = order[v];
		rows.push_back(row);
	}
	std::sort(rows.begin(), rows.end(), [](const ContractionRow &a, const ContractionRow &b) { return a.id < b.id; });
	rows.insert(rows.end(), shortcut_rows.begin(), shortcut_rows.end());
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

struct ContractionBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::string points_sql;
	std::vector<int64_t> forbidden;
	std::vector<int64_t> starts;
	double distance = 0;
	char driving_side = 'b';
	bool directed = true;
	bool details = false;
};

template <typename Row>
struct ContractionGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

std::vector<int64_t> IdList(const Value &value, const char *what) {
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

//! (type, id, contracted_vertices, source, target, cost, metric, vertex_order)
duckdb::unique_ptr<FunctionData> ContractionBind(ClientContext &, TableFunctionBindInput &input,
                                                 duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<ContractionBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		bind_data->directed = input.inputs[1].GetValue<bool>();
	}
	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw BinderException("duckrouting: '%s' must not be NULL", parameter.first.c_str());
		}
		if (NameMatches(parameter.first, "directed")) {
			bind_data->directed = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "forbidden")) {
			bind_data->forbidden = IdList(parameter.second, "'forbidden'");
		}
	}

	names = {"type", "id", "contracted_vertices", "source", "target", "cost", "metric", "vertex_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::LIST(LogicalType::BIGINT),
	                LogicalType::BIGINT,  LogicalType::BIGINT, LogicalType::DOUBLE,
	                LogicalType::BIGINT,  LogicalType::BIGINT};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> ContractionInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ContractionBindData>();
	auto state = duckdb::make_uniq<ContractionGlobalState<ContractionRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = ContractionHierarchies(edges, bind_data.directed, bind_data.forbidden);
	return std::move(state);
}

void ContractionScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ContractionGlobalState<ContractionRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value(row.is_vertex ? "v" : "e"));
		output.SetValue(1, i, Value::BIGINT(row.id));
		duckdb::vector<Value> contracted;
		for (size_t c = 0; c < row.contracted_vertices.size(); c++) {
			contracted.push_back(Value::BIGINT(row.contracted_vertices[c]));
		}
		output.SetValue(2, i, Value::LIST(LogicalType::BIGINT, contracted));
		output.SetValue(3, i, Value::BIGINT(row.source));
		output.SetValue(4, i, Value::BIGINT(row.target));
		output.SetValue(5, i, Value::DOUBLE(row.cost));
		output.SetValue(6, i, Value::BIGINT(row.metric));
		output.SetValue(7, i, Value::BIGINT(row.vertex_order));
	}
	state.offset += count;
}

//! withPointsDD shares drivingDistance's column shape.
duckdb::unique_ptr<FunctionData> WithPointsBind(ClientContext &, TableFunctionBindInput &input,
                                                duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("duckrouting: the edges and points queries must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<ContractionBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->points_sql = input.inputs[1].GetValue<std::string>();
	bind_data->starts = IdList(input.inputs[2], "start_vid");
	if (input.inputs[3].IsNull()) {
		throw BinderException("duckrouting: distance must not be NULL");
	}
	bind_data->distance = input.inputs[3].GetValue<double>();
	if (input.inputs.size() > 4 && !input.inputs[4].IsNull()) {
		const std::string side = input.inputs[4].GetValue<std::string>();
		bind_data->driving_side = side.empty() ? 'b' : static_cast<char>(std::tolower(side[0]));
	}
	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw BinderException("duckrouting: '%s' must not be NULL", parameter.first.c_str());
		}
		if (NameMatches(parameter.first, "directed")) {
			bind_data->directed = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "details")) {
			bind_data->details = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "driving_side")) {
			const std::string side = parameter.second.GetValue<std::string>();
			bind_data->driving_side = side.empty() ? 'b' : static_cast<char>(std::tolower(side[0]));
		}
	}
	if (bind_data->driving_side != 'r' && bind_data->driving_side != 'l' && bind_data->driving_side != 'b') {
		throw BinderException("duckrouting: 'driving_side' must be 'r', 'l' or 'b'");
	}

	names = {"seq", "depth", "start_vid", "pred", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> WithPointsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ContractionBindData>();
	auto state = duckdb::make_uniq<ContractionGlobalState<DrivingDistanceRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	auto points = LoadPointsOnEdges(context, bind_data.points_sql);
	state->rows = WithPointsDrivingDistance(edges, points, bind_data.starts, bind_data.distance, bind_data.driving_side,
	                                        bind_data.directed, bind_data.details);
	return std::move(state);
}

void WithPointsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ContractionGlobalState<DrivingDistanceRow>>();
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

} // namespace

TableFunctionSet GetContractionHierarchiesFunction() {
	TableFunctionSet set("duckrouting_contraction_hierarchies");
	for (size_t with_flag = 0; with_flag < 2; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, ContractionScan, ContractionBind, ContractionInit);
		function.named_parameters["directed"] = LogicalType::BOOLEAN;
		function.named_parameters["forbidden"] = LogicalType::LIST(LogicalType::BIGINT);
		set.AddFunction(function);
	}
	return set;
}

TableFunctionSet GetWithPointsDDFunction() {
	TableFunctionSet set("duckrouting_with_points_dd");
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t with_side = 0; with_side < 2; with_side++) {
			duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR,
			                                       start_is_list ? list : LogicalType::BIGINT, LogicalType::DOUBLE};
			if (with_side) {
				arguments.push_back(LogicalType::VARCHAR);
			}
			TableFunction function(arguments, WithPointsScan, WithPointsBind, WithPointsInit);
			function.named_parameters["directed"] = LogicalType::BOOLEAN;
			function.named_parameters["details"] = LogicalType::BOOLEAN;
			function.named_parameters["driving_side"] = LogicalType::VARCHAR;
			set.AddFunction(function);
		}
	}
	return set;
}

} // namespace duckrouting
