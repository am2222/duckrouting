#include "duckrouting/graph_functions.hpp"
#include "duckrouting/compat.hpp"

#include "duckrouting/graph.hpp"
#include "duckrouting/yen.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <cctype>

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <vector>

namespace duckrouting {

namespace {

//! A search state is a vertex plus the edges most recently travelled, because
//! whether a turn is restricted depends on how you arrived, not just where you
//! are. The history is only as long as the longest restriction needs.
struct TurnState {
	uint64_t vertex;
	std::vector<int64_t> history;

	bool operator<(const TurnState &other) const {
		if (vertex != other.vertex) {
			return vertex < other.vertex;
		}
		return history < other.history;
	}
};

//! The extra cost of having just travelled `history` -- any restriction whose
//! path is a suffix of it applies.
double RestrictionPenalty(const std::vector<Restriction> &restrictions, const std::vector<int64_t> &history) {
	double penalty = 0;
	for (size_t r = 0; r < restrictions.size(); r++) {
		const std::vector<int64_t> &path = restrictions[r].path;
		if (path.size() > history.size()) {
			continue;
		}
		bool matches = true;
		for (size_t i = 0; i < path.size(); i++) {
			if (history[history.size() - path.size() + i] != path[i]) {
				matches = false;
				break;
			}
		}
		if (matches) {
			penalty += restrictions[r].cost;
		}
	}
	return penalty;
}

struct SearchResult {
	std::vector<int64_t> nodes;
	std::vector<int64_t> edges;
	std::vector<double> costs;
	double total = 0;
	bool found = false;
};

//! Dijkstra over (vertex, history) states.
SearchResult SolveWithRestrictions(const Adjacency &adjacency, const VertexIndex &index,
                                   const std::vector<Restriction> &restrictions, size_t history_length, uint64_t source,
                                   uint64_t sink) {
	SearchResult result;
	if (source == sink) {
		return result;
	}

	typedef std::pair<double, TurnState> Entry;
	struct Compare {
		bool operator()(const Entry &a, const Entry &b) const {
			return a.first > b.first;
		}
	};

	std::map<TurnState, double> distance;
	std::map<TurnState, TurnState> parent;
	std::map<TurnState, int64_t> parent_edge;
	std::map<TurnState, double> parent_cost;
	std::priority_queue<Entry, std::vector<Entry>, Compare> queue;

	TurnState start {};
	start.vertex = source;
	distance[start] = 0;
	queue.push(std::make_pair(0.0, start));

	TurnState arrival {};
	bool arrived = false;
	while (!queue.empty()) {
		const double at_cost = queue.top().first;
		const TurnState at = queue.top().second;
		queue.pop();
		auto known = distance.find(at);
		if (known == distance.end() || at_cost > known->second) {
			continue;
		}
		if (at.vertex == sink) {
			arrival = at;
			arrived = true;
			break;
		}

		for (size_t i = 0; i < adjacency[at.vertex].size(); i++) {
			const Arc &arc = adjacency[at.vertex][i];

			TurnState next {};
			next.vertex = arc.to;
			next.history = at.history;
			next.history.push_back(arc.edge);
			// Only keep as much history as the longest restriction can use.
			if (next.history.size() > history_length) {
				next.history.erase(next.history.begin(),
				                   next.history.begin() + static_cast<long>(next.history.size() - history_length));
			}

			const double step = arc.cost + RestrictionPenalty(restrictions, next.history);
			const double candidate = at_cost + step;
			auto seen = distance.find(next);
			if (seen != distance.end() && candidate >= seen->second) {
				continue;
			}
			distance[next] = candidate;
			parent[next] = at;
			parent_edge[next] = arc.edge;
			parent_cost[next] = step;
			queue.push(std::make_pair(candidate, next));
		}
	}

	if (!arrived) {
		return result;
	}

	// Walk the state chain back to the start.
	std::vector<int64_t> nodes;
	std::vector<int64_t> edges;
	std::vector<double> costs;
	TurnState at = arrival;
	while (true) {
		nodes.push_back(index.IdOf(at.vertex));
		auto previous = parent.find(at);
		if (previous == parent.end()) {
			break;
		}
		edges.push_back(parent_edge[at]);
		costs.push_back(parent_cost[at]);
		at = previous->second;
	}
	std::reverse(nodes.begin(), nodes.end());
	std::reverse(edges.begin(), edges.end());
	std::reverse(costs.begin(), costs.end());

	result.nodes = nodes;
	result.edges = edges;
	result.costs = costs;
	result.total = distance[arrival];
	result.found = true;
	return result;
}

std::vector<int64_t> Normalized(const std::vector<int64_t> &values) {
	std::vector<int64_t> result(values);
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

size_t HistoryLength(const std::vector<Restriction> &restrictions) {
	size_t longest = 1;
	for (size_t i = 0; i < restrictions.size(); i++) {
		longest = (std::max)(longest, restrictions[i].path.size());
	}
	return longest;
}

std::vector<PathRow> ToRows(const SearchResult &result, int64_t start_vid, int64_t end_vid) {
	std::vector<PathRow> rows;
	double agg_cost = 0;
	for (size_t i = 0; i < result.nodes.size(); i++) {
		PathRow row {};
		row.path_seq = static_cast<int64_t>(i) + 1;
		row.start_vid = start_vid;
		row.end_vid = end_vid;
		row.node = result.nodes[i];
		row.agg_cost = DecodeInfinity(agg_cost);
		if (i < result.edges.size()) {
			row.edge = result.edges[i];
			row.cost = DecodeInfinity(result.costs[i]);
			agg_cost += result.costs[i];
		} else {
			row.edge = -1;
			row.cost = 0;
		}
		rows.push_back(row);
	}
	return rows;
}

} // namespace

std::vector<PathRow> Trsp(const std::vector<EdgeRow> &edges, const std::vector<Restriction> &restrictions,
                          const std::vector<int64_t> &starts, const std::vector<int64_t> &ends, bool directed) {
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(), [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });

	VertexIndex index;
	const Adjacency adjacency = BuildAdjacency(ordered, index, directed);
	const size_t history = HistoryLength(restrictions);

	const auto normalized_starts = Normalized(starts);
	const auto normalized_ends = Normalized(ends);

	std::vector<PathRow> rows;
	for (size_t s = 0; s < normalized_starts.size(); s++) {
		uint64_t source = 0;
		if (!index.Find(normalized_starts[s], source)) {
			continue;
		}
		for (size_t e = 0; e < normalized_ends.size(); e++) {
			if (normalized_ends[e] == normalized_starts[s]) {
				continue;
			}
			uint64_t sink = 0;
			if (!index.Find(normalized_ends[e], sink)) {
				continue;
			}
			const SearchResult result = SolveWithRestrictions(adjacency, index, restrictions, history, source, sink);
			if (!result.found) {
				continue;
			}
			auto path = ToRows(result, normalized_starts[s], normalized_ends[e]);
			rows.insert(rows.end(), path.begin(), path.end());
		}
	}
	return rows;
}

std::vector<ViaRow> TrspVia(const std::vector<EdgeRow> &edges, const std::vector<Restriction> &restrictions,
                            const std::vector<int64_t> &via, bool directed, bool strict, bool) {
	std::vector<ViaRow> rows;
	if (via.size() < 2) {
		return rows;
	}

	// Each leg is its own restricted search; an unreachable leg still consumes
	// a path_id so the numbering lines up with the via sequence.
	std::vector<std::vector<PathRow>> legs(via.size() - 1);
	std::vector<bool> found(via.size() - 1, false);
	for (size_t leg = 0; leg + 1 < via.size(); leg++) {
		std::vector<int64_t> from(1, via[leg]);
		std::vector<int64_t> to(1, via[leg + 1]);
		auto path = Trsp(edges, restrictions, from, to, directed);
		if (path.empty()) {
			if (strict) {
				return rows;
			}
			continue;
		}
		legs[leg] = path;
		found[leg] = true;
	}

	size_t last_found = 0;
	bool any = false;
	for (size_t leg = 0; leg < found.size(); leg++) {
		if (found[leg]) {
			last_found = leg;
			any = true;
		}
	}
	if (!any) {
		return rows;
	}

	double route_agg_cost = 0;
	for (size_t leg = 0; leg < legs.size(); leg++) {
		if (!found[leg]) {
			continue;
		}
		const std::vector<PathRow> &path = legs[leg];
		for (size_t i = 0; i < path.size(); i++) {
			ViaRow row {};
			row.path_id = static_cast<int64_t>(leg) + 1;
			row.path_seq = path[i].path_seq;
			row.start_vid = path[i].start_vid;
			row.end_vid = path[i].end_vid;
			row.node = path[i].node;
			row.edge = path[i].edge;
			row.cost = path[i].cost;
			row.agg_cost = path[i].agg_cost;
			row.route_agg_cost = route_agg_cost + path[i].agg_cost;
			if (leg == last_found && i + 1 == path.size()) {
				row.edge = -2;
			}
			rows.push_back(row);
		}
		route_agg_cost += path.back().agg_cost;
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

struct TrspBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::string restrictions_sql;
	std::string points_sql;
	std::vector<int64_t> starts;
	std::vector<int64_t> ends;
	std::vector<int64_t> via;
	bool directed = true;
	bool strict = false;
	bool u_turn_on_edge = true;
	bool details = false;
	char driving_side = 'b';
};

template <typename Row>
struct TrspState : public GlobalTableFunctionState {
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

void ReadOptions(TableFunctionBindInput &input, TrspBindData &bind_data) {
	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw BinderException("duckrouting: '%s' must not be NULL", parameter.first.c_str());
		}
		if (NameMatches(parameter.first, "directed")) {
			bind_data.directed = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "strict")) {
			bind_data.strict = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "u_turn_on_edge")) {
			bind_data.u_turn_on_edge = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "details")) {
			bind_data.details = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "driving_side")) {
			const std::string side = parameter.second.GetValue<std::string>();
			bind_data.driving_side = side.empty() ? 'b' : static_cast<char>(std::tolower(side[0]));
		}
	}
}

void PathColumns(duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	names = {"seq", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
}

void ViaColumns(duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	names = {"seq",  "path_id", "path_seq", "start_vid", "end_vid",
	         "node", "edge",    "cost",     "agg_cost",  "route_agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE,
	                LogicalType::DOUBLE, LogicalType::DOUBLE};
}

//! WithPoints variants slot a points query in after the restrictions one.
template <bool WithPoints, bool Via>
duckdb::unique_ptr<FunctionData> TrspBind(ClientContext &, TableFunctionBindInput &input,
                                          duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("duckrouting: the edges and restrictions queries must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<TrspBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->restrictions_sql = input.inputs[1].GetValue<std::string>();

	idx_t next = 2;
	if (WithPoints) {
		if (input.inputs[2].IsNull()) {
			throw BinderException("duckrouting: the points query must not be NULL");
		}
		bind_data->points_sql = input.inputs[2].GetValue<std::string>();
		next = 3;
	}
	if (Via) {
		bind_data->via = Ids(input.inputs[next], "via_vids");
		next++;
	} else {
		bind_data->starts = Ids(input.inputs[next], "start_vid");
		bind_data->ends = Ids(input.inputs[next + 1], "end_vid");
		next += 2;
	}
	if (input.inputs.size() > next && !input.inputs[next].IsNull()) {
		bind_data->directed = input.inputs[next].GetValue<bool>();
	}
	ReadOptions(input, *bind_data);

	if (Via) {
		ViaColumns(return_types, names);
	} else {
		PathColumns(return_types, names);
	}
	return std::move(bind_data);
}

//! Loads the graph, splitting it at the points first when asked.
std::vector<EdgeRow> TrspGraph(ClientContext &context, const TrspBindData &bind_data) {
	auto edges = LoadEdges(context, bind_data.edges_sql);
	if (bind_data.points_sql.empty()) {
		return edges;
	}
	auto points = LoadPointsOnEdges(context, bind_data.points_sql);
	return SplitEdgesAtPoints(edges, points, bind_data.driving_side);
}

template <bool WithPoints>
duckdb::unique_ptr<GlobalTableFunctionState> TrspInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TrspBindData>();
	auto state = duckdb::make_uniq<TrspState<PathRow>>();
	auto restrictions = LoadRestrictions(context, bind_data.restrictions_sql);
	auto rows = Trsp(TrspGraph(context, bind_data), restrictions, bind_data.starts, bind_data.ends, bind_data.directed);
	if (WithPoints && !bind_data.details) {
		std::vector<int64_t> visible(bind_data.starts);
		visible.insert(visible.end(), bind_data.ends.begin(), bind_data.ends.end());
		rows = HidePoints(rows, visible);
	}
	state->rows = rows;
	return std::move(state);
}

duckdb::unique_ptr<GlobalTableFunctionState> TrspViaInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TrspBindData>();
	auto state = duckdb::make_uniq<TrspState<ViaRow>>();
	auto restrictions = LoadRestrictions(context, bind_data.restrictions_sql);
	state->rows = TrspVia(TrspGraph(context, bind_data), restrictions, bind_data.via, bind_data.directed,
	                      bind_data.strict, bind_data.u_turn_on_edge);
	return std::move(state);
}

void TrspScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<TrspState<PathRow>>();
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

void TrspViaScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<TrspState<ViaRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.path_id));
		output.SetValue(2, i, Value::BIGINT(row.path_seq));
		output.SetValue(3, i, Value::BIGINT(row.start_vid));
		output.SetValue(4, i, Value::BIGINT(row.end_vid));
		output.SetValue(5, i, Value::BIGINT(row.node));
		output.SetValue(6, i, Value::BIGINT(row.edge));
		output.SetValue(7, i, Value::DOUBLE(row.cost));
		output.SetValue(8, i, Value::DOUBLE(row.agg_cost));
		output.SetValue(9, i, Value::DOUBLE(row.route_agg_cost));
	}
	state.offset += count;
}

void AddTrspOptions(TableFunction &function, bool with_points, bool via) {
	function.named_parameters["directed"] = LogicalType::BOOLEAN;
	if (via) {
		function.named_parameters["strict"] = LogicalType::BOOLEAN;
		function.named_parameters["u_turn_on_edge"] = LogicalType::BOOLEAN;
	}
	if (with_points) {
		function.named_parameters["driving_side"] = LogicalType::VARCHAR;
		function.named_parameters["details"] = LogicalType::BOOLEAN;
	}
}

} // namespace

TableFunctionSet GetTrspFunction() {
	TableFunctionSet set("duckrouting_trsp");
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t end_is_list = 0; end_is_list < 2; end_is_list++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR,
				                                       start_is_list ? list : LogicalType::BIGINT,
				                                       end_is_list ? list : LogicalType::BIGINT};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, TrspScan, TrspBind<false, false>, TrspInit<false>);
				AddTrspOptions(function, false, false);
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetTrspViaFunction() {
	TableFunctionSet set("duckrouting_trsp_via");
	for (size_t with_flag = 0; with_flag < 2; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR,
		                                       LogicalType::LIST(LogicalType::BIGINT)};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, TrspViaScan, TrspBind<false, true>, TrspViaInit);
		AddTrspOptions(function, false, true);
		set.AddFunction(function);
	}
	return set;
}

TableFunctionSet GetTrspWithPointsFunction() {
	TableFunctionSet set("duckrouting_trsp_with_points");
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t end_is_list = 0; end_is_list < 2; end_is_list++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
				                                       start_is_list ? list : LogicalType::BIGINT,
				                                       end_is_list ? list : LogicalType::BIGINT};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, TrspScan, TrspBind<true, false>, TrspInit<true>);
				AddTrspOptions(function, true, false);
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetTrspViaWithPointsFunction() {
	TableFunctionSet set("duckrouting_trsp_via_with_points");
	for (size_t with_flag = 0; with_flag < 2; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                                       LogicalType::LIST(LogicalType::BIGINT)};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, TrspViaScan, TrspBind<true, true>, TrspViaInit);
		AddTrspOptions(function, true, true);
		set.AddFunction(function);
	}
	return set;
}

} // namespace duckrouting
