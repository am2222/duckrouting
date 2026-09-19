#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace duckrouting {

namespace {

//! One edge of the graph being simplified. Original edges keep their id;
//! shortcuts get a fresh negative one.
struct WorkingEdge {
	int64_t id;
	int64_t source;
	int64_t target;
	double cost;
	bool alive;
	std::vector<int64_t> contracted;
};

//! The vertices a given vertex has absorbed, and which edges still touch it.
struct WorkingVertex {
	std::vector<int64_t> absorbed;
	std::set<size_t> edges;
	bool alive;
};

//! Distinct neighbours of `vertex` across its live edges. Dead ends and linear
//! chains are both defined by how many *different* vertices you can get to,
//! not by how many edges there are.
std::set<int64_t> Neighbours(const std::map<int64_t, WorkingVertex> &vertices, const std::vector<WorkingEdge> &edges,
                             int64_t vertex) {
	std::set<int64_t> result;
	auto entry = vertices.find(vertex);
	if (entry == vertices.end()) {
		return result;
	}
	for (auto it = entry->second.edges.begin(); it != entry->second.edges.end(); ++it) {
		const WorkingEdge &edge = edges[*it];
		if (!edge.alive) {
			continue;
		}
		const int64_t other = edge.source == vertex ? edge.target : edge.source;
		if (other != vertex) {
			result.insert(other);
		}
	}
	return result;
}

void DetachEdge(std::map<int64_t, WorkingVertex> &vertices, std::vector<WorkingEdge> &edges, size_t index) {
	edges[index].alive = false;
	vertices[edges[index].source].edges.erase(index);
	vertices[edges[index].target].edges.erase(index);
}

} // namespace

std::vector<ContractionRow> Contract(const std::vector<EdgeRow> &input, bool directed,
                                     const std::vector<ContractionMethod> &methods, int64_t cycles,
                                     const std::vector<int64_t> &forbidden) {
	std::vector<EdgeRow> ordered(input);
	std::stable_sort(ordered.begin(), ordered.end(), [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });

	std::vector<WorkingEdge> edges;
	std::map<int64_t, WorkingVertex> vertices;
	for (size_t i = 0; i < ordered.size(); i++) {
		if (!IsTraversable(ordered[i].cost) && !IsTraversable(ordered[i].reverse_cost)) {
			continue;
		}
		double cost = IsTraversable(ordered[i].cost) ? ordered[i].cost : ordered[i].reverse_cost;
		if (IsTraversable(ordered[i].cost) && IsTraversable(ordered[i].reverse_cost)) {
			cost = (std::min)(ordered[i].cost, ordered[i].reverse_cost);
		}
		WorkingEdge edge {};
		edge.id = ordered[i].id;
		edge.source = ordered[i].source;
		edge.target = ordered[i].target;
		edge.cost = cost;
		edge.alive = true;
		edges.push_back(edge);

		const size_t index = edges.size() - 1;
		vertices[edge.source].alive = true;
		vertices[edge.target].alive = true;
		vertices[edge.source].edges.insert(index);
		vertices[edge.target].edges.insert(index);
	}

	const std::set<int64_t> off_limits(forbidden.begin(), forbidden.end());
	int64_t next_shortcut = -1;

	for (int64_t cycle = 0; cycle < (std::max)(cycles, static_cast<int64_t>(1)); cycle++) {
		bool changed = false;

		for (size_t m = 0; m < methods.size(); m++) {
			bool progress = true;
			while (progress) {
				progress = false;
				// Vertices are visited in ascending id, which is what makes the
				// result reproducible.
				for (auto entry = vertices.begin(); entry != vertices.end(); ++entry) {
					const int64_t vertex = entry->first;
					if (!entry->second.alive || off_limits.count(vertex)) {
						continue;
					}
					const std::set<int64_t> neighbours = Neighbours(vertices, edges, vertex);

					if (methods[m] == ContractionMethod::DeadEnd && neighbours.size() == 1) {
						// A dead end is absorbed into the one vertex it reaches,
						// which inherits whatever it had already absorbed.
						const int64_t keeper = *neighbours.begin();
						if (off_limits.count(keeper)) {
							continue;
						}
						std::vector<int64_t> carried = entry->second.absorbed;
						carried.push_back(vertex);
						vertices[keeper].absorbed.insert(vertices[keeper].absorbed.end(), carried.begin(),
						                                 carried.end());
						std::vector<size_t> touching(entry->second.edges.begin(), entry->second.edges.end());
						for (size_t i = 0; i < touching.size(); i++) {
							if (edges[touching[i]].alive) {
								DetachEdge(vertices, edges, touching[i]);
							}
						}
						entry->second.alive = false;
						progress = true;
						changed = true;
						break;
					}

					if (methods[m] == ContractionMethod::Linear && neighbours.size() == 2) {
						// A vertex in the middle of a chain is replaced by one
						// shortcut joining its two neighbours.
						auto it = neighbours.begin();
						const int64_t left = *it++;
						const int64_t right = *it;
						double cost = 0;
						std::vector<int64_t> carried;
						std::vector<size_t> touching(entry->second.edges.begin(), entry->second.edges.end());
						bool usable = true;
						for (size_t i = 0; i < touching.size(); i++) {
							if (!edges[touching[i]].alive) {
								continue;
							}
							cost += edges[touching[i]].cost;
							carried.insert(carried.end(), edges[touching[i]].contracted.begin(),
							               edges[touching[i]].contracted.end());
						}
						if (!usable) {
							continue;
						}
						for (size_t i = 0; i < touching.size(); i++) {
							if (edges[touching[i]].alive) {
								DetachEdge(vertices, edges, touching[i]);
							}
						}

						WorkingEdge shortcut {};
						shortcut.id = next_shortcut--;
						shortcut.source = left;
						shortcut.target = right;
						shortcut.cost = cost;
						shortcut.alive = true;
						shortcut.contracted = carried;
						shortcut.contracted.insert(shortcut.contracted.end(), entry->second.absorbed.begin(),
						                           entry->second.absorbed.end());
						shortcut.contracted.push_back(vertex);
						edges.push_back(shortcut);

						const size_t index = edges.size() - 1;
						vertices[left].edges.insert(index);
						vertices[right].edges.insert(index);
						entry->second.alive = false;
						progress = true;
						changed = true;
						break;
					}
				}
			}
		}
		if (!changed) {
			break;
		}
	}

	std::vector<ContractionRow> rows;
	for (auto entry = vertices.begin(); entry != vertices.end(); ++entry) {
		if (!entry->second.alive || entry->second.absorbed.empty()) {
			continue;
		}
		ContractionRow row {};
		row.is_vertex = true;
		row.id = entry->first;
		row.contracted_vertices = entry->second.absorbed;
		std::sort(row.contracted_vertices.begin(), row.contracted_vertices.end());
		row.source = -1;
		row.target = -1;
		row.cost = -1;
		row.metric = -1;
		row.vertex_order = -1;
		rows.push_back(row);
	}
	for (size_t i = 0; i < edges.size(); i++) {
		if (!edges[i].alive || edges[i].id >= 0) {
			continue;
		}
		ContractionRow row {};
		row.is_vertex = false;
		row.id = edges[i].id;
		row.contracted_vertices = edges[i].contracted;
		std::sort(row.contracted_vertices.begin(), row.contracted_vertices.end());
		row.source = edges[i].source;
		row.target = edges[i].target;
		row.cost = DecodeInfinity(edges[i].cost);
		row.metric = -1;
		row.vertex_order = -1;
		rows.push_back(row);
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

struct ContractBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	bool directed = true;
	std::vector<ContractionMethod> methods;
	int64_t cycles = 1;
	std::vector<int64_t> forbidden;
};

struct ContractState : public GlobalTableFunctionState {
	std::vector<ContractionRow> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

std::vector<int64_t> Ids(const Value &value) {
	std::vector<int64_t> ids;
	if (value.IsNull()) {
		return ids;
	}
	if (value.type().id() != LogicalTypeId::LIST) {
		ids.push_back(value.GetValue<int64_t>());
		return ids;
	}
	for (auto &child : duckdb::ListValue::GetChildren(value)) {
		if (!child.IsNull()) {
			ids.push_back(child.GetValue<int64_t>());
		}
	}
	return ids;
}

template <int Fixed>
duckdb::unique_ptr<FunctionData> ContractBind(ClientContext &, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types,
                                              duckdb::vector<std::string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<ContractBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		bind_data->directed = input.inputs[1].GetValue<bool>();
	}
	if (Fixed == 1) {
		bind_data->methods.push_back(ContractionMethod::DeadEnd);
	} else if (Fixed == 2) {
		bind_data->methods.push_back(ContractionMethod::Linear);
	} else {
		// The combined form defaults to dead ends then chains, as pgRouting does.
		bind_data->methods.push_back(ContractionMethod::DeadEnd);
		bind_data->methods.push_back(ContractionMethod::Linear);
	}

	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw BinderException("duckrouting: '%s' must not be NULL", parameter.first.c_str());
		}
		if (duckdb::StringUtil::CIEquals(parameter.first, "directed")) {
			bind_data->directed = parameter.second.GetValue<bool>();
		} else if (duckdb::StringUtil::CIEquals(parameter.first, "forbidden")) {
			bind_data->forbidden = Ids(parameter.second);
		} else if (Fixed == 0 && duckdb::StringUtil::CIEquals(parameter.first, "cycles")) {
			bind_data->cycles = parameter.second.GetValue<int64_t>();
			if (bind_data->cycles < 1) {
				throw BinderException("duckrouting: 'cycles' must be at least 1");
			}
		} else if (Fixed == 0 && duckdb::StringUtil::CIEquals(parameter.first, "methods")) {
			bind_data->methods.clear();
			auto values = Ids(parameter.second);
			for (size_t i = 0; i < values.size(); i++) {
				if (values[i] != 1 && values[i] != 2) {
					throw BinderException("duckrouting: 'methods' may only contain 1 or 2");
				}
				bind_data->methods.push_back(static_cast<ContractionMethod>(values[i]));
			}
			if (bind_data->methods.empty()) {
				throw BinderException("duckrouting: 'methods' must not be empty");
			}
		}
	}

	names = {"type", "id", "contracted_vertices", "source", "target", "cost"};
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::LIST(LogicalType::BIGINT),
	                LogicalType::BIGINT,  LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> ContractInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ContractBindData>();
	auto state = duckdb::make_uniq<ContractState>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = Contract(edges, bind_data.directed, bind_data.methods, bind_data.cycles, bind_data.forbidden);
	return std::move(state);
}

void ContractScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ContractState>();
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
	}
	state.offset += count;
}

template <int Fixed>
TableFunctionSet ContractSet(const char *name, bool with_method_options) {
	TableFunctionSet set(name);
	for (size_t with_flag = 0; with_flag < 2; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, ContractScan, ContractBind<Fixed>, ContractInit);
		function.named_parameters["directed"] = LogicalType::BOOLEAN;
		function.named_parameters["forbidden"] = LogicalType::LIST(LogicalType::BIGINT);
		if (with_method_options) {
			function.named_parameters["methods"] = LogicalType::LIST(LogicalType::BIGINT);
			function.named_parameters["cycles"] = LogicalType::BIGINT;
		}
		set.AddFunction(function);
	}
	return set;
}

} // namespace

TableFunctionSet GetContractionFunction() {
	return ContractSet<0>("duckrouting_contraction", true);
}
TableFunctionSet GetDeadEndContractionFunction() {
	return ContractSet<1>("duckrouting_dead_end_contraction", false);
}
TableFunctionSet GetLinearContractionFunction() {
	return ContractSet<2>("duckrouting_linear_contraction", false);
}

} // namespace duckrouting
