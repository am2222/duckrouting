#include "duckrouting/graph_functions.hpp"
#include "duckrouting/compat.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace duckrouting {

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

struct WithPointsBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::string points_sql;
	std::vector<int64_t> starts;
	std::vector<int64_t> ends;
	std::vector<int64_t> via;
	char driving_side = 'b';
	bool directed = true;
	bool details = false;
	bool heap_paths = false;
	bool strict = false;
	bool u_turn_on_edge = true;
	int64_t k = 1;
};

template <typename Row>
struct WithPointsState : public GlobalTableFunctionState {
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

char ReadSide(const Value &value) {
	if (value.IsNull()) {
		return 'b';
	}
	const std::string text = value.GetValue<std::string>();
	return text.empty() ? 'b' : static_cast<char>(std::tolower(text[0]));
}

//! Every function here starts with an edges query, a points query and a
//! driving side; the differences are in what comes between.
void ReadCommon(TableFunctionBindInput &input, WithPointsBindData &bind_data, idx_t side_position) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("duckrouting: the edges and points queries must not be NULL");
	}
	bind_data.edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data.points_sql = input.inputs[1].GetValue<std::string>();
	if (input.inputs.size() > side_position) {
		bind_data.driving_side = ReadSide(input.inputs[side_position]);
	}
	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw BinderException("duckrouting: '%s' must not be NULL", parameter.first.c_str());
		}
		if (NameMatches(parameter.first, "directed")) {
			bind_data.directed = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "details")) {
			bind_data.details = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "driving_side")) {
			bind_data.driving_side = ReadSide(parameter.second);
		} else if (NameMatches(parameter.first, "heap_paths")) {
			bind_data.heap_paths = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "strict")) {
			bind_data.strict = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "u_turn_on_edge")) {
			bind_data.u_turn_on_edge = parameter.second.GetValue<bool>();
		}
	}
	if (bind_data.driving_side != 'r' && bind_data.driving_side != 'l' && bind_data.driving_side != 'b') {
		throw BinderException("duckrouting: 'driving_side' must be 'r', 'l' or 'b'");
	}
}

void PathColumns(duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	names = {"seq", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
}

void PathScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<WithPointsState<PathRow>>();
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

void CostScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<WithPointsState<CostRow>>();
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

//! Loads the edges, splits them at the points, and hands back the new graph.
std::vector<EdgeRow> PreparedGraph(ClientContext &context, const WithPointsBindData &bind_data) {
	auto edges = LoadEdges(context, bind_data.edges_sql);
	auto points = LoadPointsOnEdges(context, bind_data.points_sql);
	return SplitEdgesAtPoints(edges, points, bind_data.driving_side);
}

// --- withPoints / withPointsCost --------------------------------------------

template <bool CostsOnly>
duckdb::unique_ptr<FunctionData> PairBind(ClientContext &, TableFunctionBindInput &input,
                                          duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	auto bind_data = duckdb::make_uniq<WithPointsBindData>();
	bind_data->starts = Ids(input.inputs[2], "start_vid");
	bind_data->ends = Ids(input.inputs[3], "end_vid");
	ReadCommon(input, *bind_data, 4);
	if (CostsOnly) {
		names = {"start_vid", "end_vid", "agg_cost"};
		return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
	} else {
		PathColumns(return_types, names);
	}
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> WithPointsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<WithPointsBindData>();
	auto state = duckdb::make_uniq<WithPointsState<PathRow>>();
	auto rows = Dijkstra(PreparedGraph(context, bind_data), bind_data.starts, bind_data.ends, bind_data.directed);
	if (!bind_data.details) {
		// Only the points the caller asked about stay visible.
		std::vector<int64_t> visible(bind_data.starts);
		visible.insert(visible.end(), bind_data.ends.begin(), bind_data.ends.end());
		rows = HidePoints(rows, visible);
	}
	state->rows = rows;
	return std::move(state);
}

duckdb::unique_ptr<GlobalTableFunctionState> WithPointsCostInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<WithPointsBindData>();
	auto state = duckdb::make_uniq<WithPointsState<CostRow>>();
	state->rows = DijkstraCost(PreparedGraph(context, bind_data), bind_data.starts, bind_data.ends, bind_data.directed);
	return std::move(state);
}

// --- withPointsCostMatrix ---------------------------------------------------

duckdb::unique_ptr<FunctionData> MatrixBind(ClientContext &, TableFunctionBindInput &input,
                                            duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	auto bind_data = duckdb::make_uniq<WithPointsBindData>();
	bind_data->starts = Ids(input.inputs[2], "vids");
	bind_data->ends = bind_data->starts;
	ReadCommon(input, *bind_data, 3);
	names = {"start_vid", "end_vid", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

// --- withPointsVia ----------------------------------------------------------

duckdb::unique_ptr<FunctionData> ViaBind(ClientContext &, TableFunctionBindInput &input,
                                         duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	auto bind_data = duckdb::make_uniq<WithPointsBindData>();
	bind_data->via = Ids(input.inputs[2], "via_vids");
	ReadCommon(input, *bind_data, 3);
	names = {"seq",  "path_id", "path_seq", "start_vid", "end_vid",
	         "node", "edge",    "cost",     "agg_cost",  "route_agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE,
	                LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> ViaInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<WithPointsBindData>();
	auto state = duckdb::make_uniq<WithPointsState<ViaRow>>();
	state->rows = DijkstraVia(PreparedGraph(context, bind_data), bind_data.via, bind_data.directed, bind_data.strict,
	                          bind_data.u_turn_on_edge);
	return std::move(state);
}

void ViaScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<WithPointsState<ViaRow>>();
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

// --- withPointsKSP ----------------------------------------------------------

duckdb::unique_ptr<FunctionData> KspBind(ClientContext &, TableFunctionBindInput &input,
                                         duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	auto bind_data = duckdb::make_uniq<WithPointsBindData>();
	bind_data->starts = Ids(input.inputs[2], "start_vid");
	bind_data->ends = Ids(input.inputs[3], "end_vid");
	if (input.inputs[4].IsNull()) {
		throw BinderException("duckrouting: K must not be NULL");
	}
	bind_data->k = input.inputs[4].GetValue<int64_t>();
	if (bind_data->k < 1) {
		throw BinderException("duckrouting_with_points_ksp: K must be at least 1");
	}
	ReadCommon(input, *bind_data, 5);
	names = {"seq", "path_id", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> KspInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<WithPointsBindData>();
	auto state = duckdb::make_uniq<WithPointsState<KspRow>>();
	state->rows = Ksp(PreparedGraph(context, bind_data), bind_data.starts, bind_data.ends, bind_data.k,
	                  bind_data.directed, bind_data.heap_paths);
	return std::move(state);
}

void KspScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<WithPointsState<KspRow>>();
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
	}
	state.offset += count;
}

void AddCommonOptions(TableFunction &function) {
	function.named_parameters["directed"] = LogicalType::BOOLEAN;
	function.named_parameters["driving_side"] = LogicalType::VARCHAR;
	function.named_parameters["details"] = LogicalType::BOOLEAN;
}

} // namespace

TableFunctionSet GetWithPointsFunction() {
	TableFunctionSet set("duckrouting_with_points");
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t end_is_list = 0; end_is_list < 2; end_is_list++) {
			for (size_t with_side = 0; with_side < 2; with_side++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR,
				                                       start_is_list ? list : LogicalType::BIGINT,
				                                       end_is_list ? list : LogicalType::BIGINT};
				if (with_side) {
					arguments.push_back(LogicalType::VARCHAR);
				}
				TableFunction function(arguments, PathScan, PairBind<false>, WithPointsInit);
				AddCommonOptions(function);
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetWithPointsCostFunction() {
	TableFunctionSet set("duckrouting_with_points_cost");
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t end_is_list = 0; end_is_list < 2; end_is_list++) {
			for (size_t with_side = 0; with_side < 2; with_side++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR,
				                                       start_is_list ? list : LogicalType::BIGINT,
				                                       end_is_list ? list : LogicalType::BIGINT};
				if (with_side) {
					arguments.push_back(LogicalType::VARCHAR);
				}
				TableFunction function(arguments, CostScan, PairBind<true>, WithPointsCostInit);
				AddCommonOptions(function);
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetWithPointsCostMatrixFunction() {
	TableFunctionSet set("duckrouting_with_points_cost_matrix");
	for (size_t with_side = 0; with_side < 2; with_side++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR,
		                                       LogicalType::LIST(LogicalType::BIGINT)};
		if (with_side) {
			arguments.push_back(LogicalType::VARCHAR);
		}
		TableFunction function(arguments, CostScan, MatrixBind, WithPointsCostInit);
		AddCommonOptions(function);
		set.AddFunction(function);
	}
	return set;
}

TableFunctionSet GetWithPointsViaFunction() {
	TableFunctionSet set("duckrouting_with_points_via");
	for (size_t with_side = 0; with_side < 2; with_side++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR,
		                                       LogicalType::LIST(LogicalType::BIGINT)};
		if (with_side) {
			arguments.push_back(LogicalType::VARCHAR);
		}
		TableFunction function(arguments, ViaScan, ViaBind, ViaInit);
		AddCommonOptions(function);
		function.named_parameters["strict"] = LogicalType::BOOLEAN;
		function.named_parameters["u_turn_on_edge"] = LogicalType::BOOLEAN;
		set.AddFunction(function);
	}
	return set;
}

TableFunctionSet GetWithPointsKspFunction() {
	TableFunctionSet set("duckrouting_with_points_ksp");
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t start_is_list = 0; start_is_list < 2; start_is_list++) {
		for (size_t end_is_list = 0; end_is_list < 2; end_is_list++) {
			for (size_t with_side = 0; with_side < 2; with_side++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR,
				                                       start_is_list ? list : LogicalType::BIGINT,
				                                       end_is_list ? list : LogicalType::BIGINT, LogicalType::BIGINT};
				if (with_side) {
					arguments.push_back(LogicalType::VARCHAR);
				}
				TableFunction function(arguments, KspScan, KspBind, KspInit);
				AddCommonOptions(function);
				function.named_parameters["heap_paths"] = LogicalType::BOOLEAN;
				set.AddFunction(function);
			}
		}
	}
	return set;
}

} // namespace duckrouting
