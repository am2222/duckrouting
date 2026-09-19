#include "duckrouting/dijkstra.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

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

//! pgRouting takes a vertex either as a single id or as an array of them; both
//! spellings mean the same thing to the algorithms.
std::vector<int64_t> ToVertexIds(const Value &value, const char *what) {
	if (value.IsNull()) {
		throw BinderException("duckrouting: %s must not be NULL", what);
	}
	if (value.type().id() != LogicalTypeId::LIST) {
		return {value.GetValue<int64_t>()};
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

bool NamedFlag(TableFunctionBindInput &input, const char *name, bool fallback) {
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

int64_t NamedInt(TableFunctionBindInput &input, const char *name, int64_t fallback) {
	for (auto &parameter : input.named_parameters) {
		if (duckdb::StringUtil::CIEquals(parameter.first, name)) {
			if (parameter.second.IsNull()) {
				throw BinderException("duckrouting: '%s' must not be NULL", name);
			}
			return parameter.second.GetValue<int64_t>();
		}
	}
	return fallback;
}

struct RoutingBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::vector<int64_t> starts;
	std::vector<int64_t> ends;
	bool directed = true;
	bool equicost = false;
	bool heap_paths = false;
	double distance = 0;
	int64_t cap = 1;
	int64_t k = 1;
	std::vector<int64_t> via;
};

//! Every function here materialises its whole result during init and then just
//! pages it out; routing results are small relative to the graph.
template <typename Row>
struct RoutingGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

void RequireNonNull(TableFunctionBindInput &input) {
	for (idx_t i = 0; i < input.inputs.size(); i++) {
		if (input.inputs[i].IsNull()) {
			throw BinderException("duckrouting: arguments must not be NULL");
		}
	}
}

// --- dijkstra ---------------------------------------------------------------

duckdb::unique_ptr<FunctionData> DijkstraBind(ClientContext &, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types,
                                              duckdb::vector<std::string> &names) {
	RequireNonNull(input);
	auto bind_data = duckdb::make_uniq<RoutingBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->starts = ToVertexIds(input.inputs[1], "start_vid");
	bind_data->ends = ToVertexIds(input.inputs[2], "end_vid");
	bind_data->directed =
	    NamedFlag(input, "directed", input.inputs.size() > 3 ? input.inputs[3].GetValue<bool>() : true);

	names = {"seq", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> DijkstraInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RoutingBindData>();
	auto state = duckdb::make_uniq<RoutingGlobalState<PathRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = Dijkstra(edges, bind_data.starts, bind_data.ends, bind_data.directed);
	return std::move(state);
}

void DijkstraScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RoutingGlobalState<PathRow>>();
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

// --- dijkstra_cost / dijkstra_cost_matrix -----------------------------------

duckdb::unique_ptr<FunctionData> CostBind(ClientContext &, TableFunctionBindInput &input,
                                          duckdb::vector<LogicalType> &return_types,
                                          duckdb::vector<std::string> &names) {
	RequireNonNull(input);
	auto bind_data = duckdb::make_uniq<RoutingBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->starts = ToVertexIds(input.inputs[1], "start_vid");
	bind_data->ends = ToVertexIds(input.inputs[2], "end_vid");
	bind_data->directed =
	    NamedFlag(input, "directed", input.inputs.size() > 3 ? input.inputs[3].GetValue<bool>() : true);

	names = {"start_vid", "end_vid", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

//! The matrix form takes one vertex list and routes it against itself.
duckdb::unique_ptr<FunctionData> CostMatrixBind(ClientContext &, TableFunctionBindInput &input,
                                                duckdb::vector<LogicalType> &return_types,
                                                duckdb::vector<std::string> &names) {
	RequireNonNull(input);
	auto bind_data = duckdb::make_uniq<RoutingBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->starts = ToVertexIds(input.inputs[1], "vids");
	bind_data->ends = bind_data->starts;
	bind_data->directed =
	    NamedFlag(input, "directed", input.inputs.size() > 2 ? input.inputs[2].GetValue<bool>() : true);

	names = {"start_vid", "end_vid", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> CostInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RoutingBindData>();
	auto state = duckdb::make_uniq<RoutingGlobalState<CostRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = DijkstraCost(edges, bind_data.starts, bind_data.ends, bind_data.directed);
	return std::move(state);
}

void CostScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RoutingGlobalState<CostRow>>();
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

// --- driving_distance -------------------------------------------------------

duckdb::unique_ptr<FunctionData> DrivingDistanceBind(ClientContext &, TableFunctionBindInput &input,
                                                     duckdb::vector<LogicalType> &return_types,
                                                     duckdb::vector<std::string> &names) {
	RequireNonNull(input);
	auto bind_data = duckdb::make_uniq<RoutingBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->starts = ToVertexIds(input.inputs[1], "start_vid");
	bind_data->distance = input.inputs[2].GetValue<double>();
	bind_data->directed =
	    NamedFlag(input, "directed", input.inputs.size() > 3 ? input.inputs[3].GetValue<bool>() : true);
	bind_data->equicost = NamedFlag(input, "equicost", false);

	names = {"seq", "depth", "start_vid", "pred", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> DrivingDistanceInit(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RoutingBindData>();
	auto state = duckdb::make_uniq<RoutingGlobalState<DrivingDistanceRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = DrivingDistance(edges, bind_data.starts, bind_data.distance, bind_data.directed,
	                              bind_data.equicost);
	return std::move(state);
}

void DrivingDistanceScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RoutingGlobalState<DrivingDistanceRow>>();
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


// --- dijkstra_via -----------------------------------------------------------

duckdb::unique_ptr<FunctionData> ViaBind(ClientContext &, TableFunctionBindInput &input,
                                         duckdb::vector<LogicalType> &return_types,
                                         duckdb::vector<std::string> &names) {
	RequireNonNull(input);
	auto bind_data = duckdb::make_uniq<RoutingBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->via = ToVertexIds(input.inputs[1], "via_vids");
	bind_data->directed =
	    NamedFlag(input, "directed", input.inputs.size() > 2 ? input.inputs[2].GetValue<bool>() : true);

	names = {"seq",  "path_id", "path_seq", "start_vid", "end_vid",
	         "node", "edge",    "cost",     "agg_cost",  "route_agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE,
	                LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> ViaInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RoutingBindData>();
	auto state = duckdb::make_uniq<RoutingGlobalState<ViaRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = DijkstraVia(edges, bind_data.via, bind_data.directed);
	return std::move(state);
}

void ViaScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RoutingGlobalState<ViaRow>>();
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

// --- dijkstra_near / dijkstra_near_cost -------------------------------------

duckdb::unique_ptr<FunctionData> NearBindCommon(TableFunctionBindInput &input, RoutingBindData &bind_data) {
	RequireNonNull(input);
	bind_data.edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data.starts = ToVertexIds(input.inputs[1], "start_vid");
	bind_data.ends = ToVertexIds(input.inputs[2], "end_vid");
	bind_data.directed =
	    NamedFlag(input, "directed", input.inputs.size() > 3 ? input.inputs[3].GetValue<bool>() : true);
	bind_data.cap = NamedInt(input, "cap", 1);
	if (bind_data.cap < 1) {
		throw BinderException("duckrouting: 'cap' must be at least 1");
	}
	return nullptr;
}

duckdb::unique_ptr<FunctionData> NearBind(ClientContext &, TableFunctionBindInput &input,
                                          duckdb::vector<LogicalType> &return_types,
                                          duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<RoutingBindData>();
	NearBindCommon(input, *bind_data);
	names = {"seq", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> NearInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RoutingBindData>();
	auto state = duckdb::make_uniq<RoutingGlobalState<PathRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = DijkstraNear(edges, bind_data.starts, bind_data.ends, bind_data.directed, bind_data.cap);
	return std::move(state);
}

duckdb::unique_ptr<FunctionData> NearCostBind(ClientContext &, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types,
                                              duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<RoutingBindData>();
	NearBindCommon(input, *bind_data);
	names = {"start_vid", "end_vid", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> NearCostInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RoutingBindData>();
	auto state = duckdb::make_uniq<RoutingGlobalState<CostRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = DijkstraNearCost(edges, bind_data.starts, bind_data.ends, bind_data.directed, bind_data.cap);
	return std::move(state);
}

// --- ksp --------------------------------------------------------------------

duckdb::unique_ptr<FunctionData> KspBind(ClientContext &, TableFunctionBindInput &input,
                                         duckdb::vector<LogicalType> &return_types,
                                         duckdb::vector<std::string> &names) {
	RequireNonNull(input);
	auto bind_data = duckdb::make_uniq<RoutingBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->starts = ToVertexIds(input.inputs[1], "start_vid");
	bind_data->ends = ToVertexIds(input.inputs[2], "end_vid");
	bind_data->k = input.inputs[3].GetValue<int64_t>();
	if (bind_data->k < 1) {
		throw BinderException("duckrouting_ksp: K must be at least 1");
	}
	bind_data->directed =
	    NamedFlag(input, "directed", input.inputs.size() > 4 ? input.inputs[4].GetValue<bool>() : true);
	bind_data->heap_paths = NamedFlag(input, "heap_paths", false);

	names = {"seq", "path_id", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE,
	                LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> KspInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RoutingBindData>();
	auto state = duckdb::make_uniq<RoutingGlobalState<KspRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = Ksp(edges, bind_data.starts, bind_data.ends, bind_data.k, bind_data.directed,
	                  bind_data.heap_paths);
	return std::move(state);
}

void KspScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RoutingGlobalState<KspRow>>();
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

//! pgRouting accepts a vertex as BIGINT or BIGINT[]; register both spellings
//! for each position, with and without the trailing `directed` flag.
const LogicalType &VidTypes(size_t which) {
	static const LogicalType scalar = LogicalType::BIGINT;
	static const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	return which == 0 ? scalar : list;
}

} // namespace

TableFunctionSet GetDijkstraFunction() {
	TableFunctionSet set("duckrouting_dijkstra");
	for (size_t start_shape = 0; start_shape < 2; start_shape++) {
		for (size_t end_shape = 0; end_shape < 2; end_shape++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, VidTypes(start_shape),
				                                       VidTypes(end_shape)};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, DijkstraScan, DijkstraBind, DijkstraInit);
				function.named_parameters["directed"] = LogicalType::BOOLEAN;
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetDijkstraCostFunction() {
	TableFunctionSet set("duckrouting_dijkstra_cost");
	for (size_t start_shape = 0; start_shape < 2; start_shape++) {
		for (size_t end_shape = 0; end_shape < 2; end_shape++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, VidTypes(start_shape),
				                                       VidTypes(end_shape)};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, CostScan, CostBind, CostInit);
				function.named_parameters["directed"] = LogicalType::BOOLEAN;
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetDijkstraCostMatrixFunction() {
	TableFunctionSet set("duckrouting_dijkstra_cost_matrix");
	for (size_t with_flag = 0; with_flag < 2; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::BIGINT)};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, CostScan, CostMatrixBind, CostInit);
		function.named_parameters["directed"] = LogicalType::BOOLEAN;
		set.AddFunction(function);
	}
	return set;
}

TableFunctionSet GetDrivingDistanceFunction() {
	TableFunctionSet set("duckrouting_driving_distance");
	for (size_t start_shape = 0; start_shape < 2; start_shape++) {
		for (size_t with_flag = 0; with_flag < 2; with_flag++) {
			duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, VidTypes(start_shape),
			                                       LogicalType::DOUBLE};
			if (with_flag) {
				arguments.push_back(LogicalType::BOOLEAN);
			}
			TableFunction function(arguments, DrivingDistanceScan, DrivingDistanceBind, DrivingDistanceInit);
			function.named_parameters["directed"] = LogicalType::BOOLEAN;
			function.named_parameters["equicost"] = LogicalType::BOOLEAN;
			set.AddFunction(function);
		}
	}
	return set;
}

TableFunctionSet GetDijkstraViaFunction() {
	TableFunctionSet set("duckrouting_dijkstra_via");
	for (size_t with_flag = 0; with_flag < 2; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::BIGINT)};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, ViaScan, ViaBind, ViaInit);
		function.named_parameters["directed"] = LogicalType::BOOLEAN;
		set.AddFunction(function);
	}
	return set;
}

TableFunctionSet GetDijkstraNearFunction() {
	TableFunctionSet set("duckrouting_dijkstra_near");
	for (size_t start_shape = 0; start_shape < 2; start_shape++) {
		for (size_t end_shape = 0; end_shape < 2; end_shape++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, VidTypes(start_shape),
				                                       VidTypes(end_shape)};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, DijkstraScan, NearBind, NearInit);
				function.named_parameters["directed"] = LogicalType::BOOLEAN;
				function.named_parameters["cap"] = LogicalType::BIGINT;
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetDijkstraNearCostFunction() {
	TableFunctionSet set("duckrouting_dijkstra_near_cost");
	for (size_t start_shape = 0; start_shape < 2; start_shape++) {
		for (size_t end_shape = 0; end_shape < 2; end_shape++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, VidTypes(start_shape),
				                                       VidTypes(end_shape)};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, CostScan, NearCostBind, NearCostInit);
				function.named_parameters["directed"] = LogicalType::BOOLEAN;
				function.named_parameters["cap"] = LogicalType::BIGINT;
				set.AddFunction(function);
			}
		}
	}
	return set;
}

TableFunctionSet GetKspFunction() {
	TableFunctionSet set("duckrouting_ksp");
	for (size_t start_shape = 0; start_shape < 2; start_shape++) {
		for (size_t end_shape = 0; end_shape < 2; end_shape++) {
			for (size_t with_flag = 0; with_flag < 2; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, VidTypes(start_shape),
				                                       VidTypes(end_shape), LogicalType::BIGINT};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, KspScan, KspBind, KspInit);
				function.named_parameters["directed"] = LogicalType::BOOLEAN;
				function.named_parameters["heap_paths"] = LogicalType::BOOLEAN;
				set.AddFunction(function);
			}
		}
	}
	return set;
}

} // namespace duckrouting
