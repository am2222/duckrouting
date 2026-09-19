#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"
#include "duckrouting/yen.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace duckrouting {

namespace {

//! The edges of the input, sorted so results are reproducible.
std::vector<EdgeRow> Sorted(const std::vector<EdgeRow> &edges) {
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(),
	                 [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });
	return ordered;
}

} // namespace

std::vector<TransformedEdgeRow> LineGraph(const std::vector<EdgeRow> &edges, bool directed) {
	const auto ordered = Sorted(edges);

	// Which edges touch each vertex, and from which end. Undirected ignores
	// the cost columns entirely: two edges are adjacent when they share an
	// endpoint, whichever way either of them happens to be traversable.
	std::map<int64_t, std::vector<size_t>> incoming;
	std::map<int64_t, std::vector<size_t>> outgoing;
	for (size_t i = 0; i < ordered.size(); i++) {
		if (!directed) {
			if (!IsTraversable(ordered[i].cost) && !IsTraversable(ordered[i].reverse_cost)) {
				continue;
			}
			incoming[ordered[i].source].push_back(i);
			incoming[ordered[i].target].push_back(i);
			outgoing[ordered[i].source].push_back(i);
			outgoing[ordered[i].target].push_back(i);
			continue;
		}
		if (IsTraversable(ordered[i].cost)) {
			outgoing[ordered[i].source].push_back(i);
			incoming[ordered[i].target].push_back(i);
		}
		if (IsTraversable(ordered[i].reverse_cost)) {
			outgoing[ordered[i].target].push_back(i);
			incoming[ordered[i].source].push_back(i);
		}
	}

	// Two edges become adjacent when you can leave one and enter the other at
	// a shared vertex. Undirected drops the direction requirement.
	std::set<std::pair<int64_t, int64_t>> seen;
	std::vector<TransformedEdgeRow> rows;
	for (auto entry = incoming.begin(); entry != incoming.end(); ++entry) {
		auto leaving = outgoing.find(entry->first);
		if (leaving == outgoing.end()) {
			continue;
		}
		for (size_t a = 0; a < entry->second.size(); a++) {
			for (size_t b = 0; b < leaving->second.size(); b++) {
				const int64_t from = ordered[entry->second[a]].id;
				const int64_t to = ordered[leaving->second[b]].id;
				if (from == to) {
					continue;
				}
				std::pair<int64_t, int64_t> key(from, to);
				if (!directed && from > to) {
					key = std::make_pair(to, from);
				}
				if (seen.count(key)) {
					continue;
				}
				seen.insert(key);
				// pgRouting reports these as one-way edges of unit cost.
				rows.push_back(TransformedEdgeRow {key.first, key.second, 1, -1, 0});
			}
		}
	}
	std::sort(rows.begin(), rows.end(), [](const TransformedEdgeRow &a, const TransformedEdgeRow &b) {
		if (a.source != b.source) {
			return a.source < b.source;
		}
		return a.target < b.target;
	});
	return rows;
}

std::vector<TransformedEdgeRow> LineGraphFull(const std::vector<EdgeRow> &edges) {
	const auto ordered = Sorted(edges);

	// Every half-edge -- one end of one edge -- becomes its own node, numbered
	// negatively. Turn costs can then be attached to the zero-cost links
	// between the half-edges meeting at a vertex.
	std::map<std::pair<int64_t, int64_t>, int64_t> node_of;
	int64_t next = -1;
	auto half_edge = [&](int64_t edge_id, int64_t vertex) {
		const std::pair<int64_t, int64_t> key(edge_id, vertex);
		auto entry = node_of.find(key);
		if (entry != node_of.end()) {
			return entry->second;
		}
		node_of[key] = next;
		return next--;
	};

	std::vector<TransformedEdgeRow> rows;
	std::map<int64_t, std::vector<int64_t>> at_vertex;

	for (size_t i = 0; i < ordered.size(); i++) {
		const EdgeRow &edge = ordered[i];
		if (IsTraversable(edge.cost)) {
			const int64_t entry_node = half_edge(edge.id, edge.source);
			rows.push_back(TransformedEdgeRow {entry_node, edge.target, edge.cost, -1, edge.id});
			at_vertex[edge.source].push_back(entry_node);
		}
		if (IsTraversable(edge.reverse_cost)) {
			// The reverse direction is reported with a negated edge id.
			const int64_t entry_node = half_edge(-edge.id, edge.target);
			rows.push_back(TransformedEdgeRow {entry_node, edge.source, edge.reverse_cost, -1, -edge.id});
			at_vertex[edge.target].push_back(entry_node);
		}
	}

	// Reaching a vertex lets you take any half-edge leaving it, at no cost.
	for (auto entry = at_vertex.begin(); entry != at_vertex.end(); ++entry) {
		for (size_t i = 0; i < entry->second.size(); i++) {
			rows.push_back(TransformedEdgeRow {entry->first, entry->second[i], 0, -1, 0});
		}
	}
	return rows;
}

namespace {

//! Hierholzer's algorithm: walk the graph consuming edges, splicing in
//! detours whenever the walk returns to a vertex with unused edges.
std::vector<std::pair<int64_t, size_t>> EulerianCircuit(const std::vector<EdgeRow> &edges,
                                                        const std::vector<size_t> &multiplicity,
                                                        int64_t start) {
	std::map<int64_t, std::vector<std::pair<int64_t, size_t>>> adjacency;
	std::vector<size_t> remaining(edges.size());
	for (size_t i = 0; i < edges.size(); i++) {
		remaining[i] = multiplicity[i];
		for (size_t copy = 0; copy < multiplicity[i]; copy++) {
			adjacency[edges[i].source].push_back(std::make_pair(edges[i].target, i));
			adjacency[edges[i].target].push_back(std::make_pair(edges[i].source, i));
		}
	}
	std::vector<size_t> used(edges.size(), 0);

	std::vector<std::pair<int64_t, size_t>> circuit;
	std::vector<std::pair<int64_t, size_t>> stack;
	stack.push_back(std::make_pair(start, static_cast<size_t>(-1)));

	while (!stack.empty()) {
		const int64_t at = stack.back().first;
		bool advanced = false;
		auto entry = adjacency.find(at);
		if (entry != adjacency.end()) {
			for (size_t i = 0; i < entry->second.size(); i++) {
				const size_t edge = entry->second[i].second;
				if (used[edge] >= remaining[edge]) {
					continue;
				}
				used[edge]++;
				stack.push_back(std::make_pair(entry->second[i].first, edge));
				advanced = true;
				break;
			}
		}
		if (!advanced) {
			circuit.push_back(stack.back());
			stack.pop_back();
		}
	}
	std::reverse(circuit.begin(), circuit.end());
	return circuit;
}

//! How many times each edge must be walked. An odd-degree vertex has to be
//! paired with another and the path between them duplicated.
std::vector<size_t> Multiplicities(const std::vector<EdgeRow> &edges, double &added_cost) {
	std::vector<size_t> multiplicity(edges.size(), 1);
	added_cost = 0;

	std::map<int64_t, int64_t> degree;
	for (size_t i = 0; i < edges.size(); i++) {
		degree[edges[i].source]++;
		degree[edges[i].target]++;
	}
	std::vector<int64_t> odd;
	for (auto entry = degree.begin(); entry != degree.end(); ++entry) {
		if (entry->second % 2 != 0) {
			odd.push_back(entry->first);
		}
	}

	// Pair the odd vertices greedily by shortest path. Optimal pairing needs a
	// minimum-weight perfect matching on a general graph; greedy can overshoot
	// the true optimum when there are many odd vertices.
	VertexIndex index;
	const Adjacency adjacency = BuildAdjacency(edges, index, false);
	std::set<int64_t> pending(odd.begin(), odd.end());
	while (pending.size() >= 2) {
		const int64_t from = *pending.begin();
		pending.erase(pending.begin());

		double best = 0;
		int64_t partner = 0;
		SimplePath best_path;
		bool found = false;
		for (auto it = pending.begin(); it != pending.end(); ++it) {
			uint64_t source = 0;
			uint64_t sink = 0;
			if (!index.Find(from, source) || !index.Find(*it, sink)) {
				continue;
			}
			SimplePath path;
			if (!ConstrainedShortestPath(adjacency, source, sink, std::set<uint64_t>(), std::set<ArcKey>(),
			                             path)) {
				continue;
			}
			if (!found || path.cost < best) {
				found = true;
				best = path.cost;
				partner = *it;
				best_path = path;
			}
		}
		if (!found) {
			continue;
		}
		pending.erase(partner);
		added_cost += best;

		// Walking the connecting path a second time makes both ends even.
		for (size_t i = 0; i + 1 < best_path.steps.size(); i++) {
			const int64_t edge_id = best_path.steps[i].edge;
			for (size_t e = 0; e < edges.size(); e++) {
				if (edges[e].id == edge_id) {
					multiplicity[e]++;
					break;
				}
			}
		}
	}
	return multiplicity;
}

//! The usable edges, each collapsed to a single traversable cost.
std::vector<EdgeRow> Usable(const std::vector<EdgeRow> &edges) {
	std::vector<EdgeRow> usable;
	const auto ordered = Sorted(edges);
	for (size_t i = 0; i < ordered.size(); i++) {
		const bool forward = IsTraversable(ordered[i].cost);
		const bool backward = IsTraversable(ordered[i].reverse_cost);
		if (!forward && !backward) {
			continue;
		}
		double cost = forward ? ordered[i].cost : ordered[i].reverse_cost;
		if (forward && backward) {
			cost = (std::min)(ordered[i].cost, ordered[i].reverse_cost);
		}
		usable.push_back(EdgeRow {ordered[i].id, ordered[i].source, ordered[i].target, cost, cost});
	}
	return usable;
}

} // namespace

std::vector<PathRow> ChinesePostman(const std::vector<EdgeRow> &edges, bool) {
	const auto usable = Usable(edges);
	if (usable.empty()) {
		return {};
	}

	double added = 0;
	const auto multiplicity = Multiplicities(usable, added);
	const auto circuit = EulerianCircuit(usable, multiplicity, usable[0].source);
	if (circuit.empty()) {
		return {};
	}

	std::vector<PathRow> rows;
	double agg_cost = 0;
	for (size_t i = 0; i < circuit.size(); i++) {
		PathRow row {};
		row.path_seq = static_cast<int64_t>(i) + 1;
		row.start_vid = circuit.front().first;
		row.end_vid = circuit.back().first;
		row.node = circuit[i].first;
		row.agg_cost = DecodeInfinity(agg_cost);
		if (i + 1 < circuit.size()) {
			const size_t edge = circuit[i + 1].second;
			row.edge = usable[edge].id;
			row.cost = DecodeInfinity(usable[edge].cost);
			agg_cost += usable[edge].cost;
		} else {
			row.edge = -1;
			row.cost = 0;
		}
		rows.push_back(row);
	}
	return rows;
}

double ChinesePostmanCost(const std::vector<EdgeRow> &edges, bool directed) {
	const auto rows = ChinesePostman(edges, directed);
	return rows.empty() ? 0 : rows.back().agg_cost;
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
using duckdb::TableFunction;
using duckdb::TableFunctionBindInput;
using duckdb::TableFunctionInitInput;
using duckdb::TableFunctionInput;
using duckdb::TableFunctionSet;
using duckdb::Value;

struct TransformBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	bool directed = true;
};

template <typename Row>
struct TransformState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

void ReadEdgesArgument(TableFunctionBindInput &input, TransformBindData &bind_data) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	bind_data.edges_sql = input.inputs[0].GetValue<std::string>();
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		bind_data.directed = input.inputs[1].GetValue<bool>();
	}
	for (auto &parameter : input.named_parameters) {
		if (duckdb::StringUtil::CIEquals(parameter.first, "directed")) {
			if (parameter.second.IsNull()) {
				throw BinderException("duckrouting: 'directed' must not be NULL");
			}
			bind_data.directed = parameter.second.GetValue<bool>();
		}
	}
}

//! (seq, source, target, cost, reverse_cost) for the line graph; lineGraphFull
//! swaps the last column for the edge it came from.
template <bool Full>
duckdb::unique_ptr<FunctionData> TransformBind(ClientContext &, TableFunctionBindInput &input,
                                               duckdb::vector<LogicalType> &return_types,
                                               duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<TransformBindData>();
	ReadEdgesArgument(input, *bind_data);
	if (Full) {
		names = {"seq", "source", "target", "cost", "edge"};
		return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE,
		                LogicalType::BIGINT};
	} else {
		names = {"seq", "source", "target", "cost", "reverse_cost"};
		return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE,
		                LogicalType::DOUBLE};
	}
	return std::move(bind_data);
}

template <bool Full>
duckdb::unique_ptr<GlobalTableFunctionState> TransformInit(ClientContext &context,
                                                           TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TransformBindData>();
	auto state = duckdb::make_uniq<TransformState<TransformedEdgeRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = Full ? LineGraphFull(edges) : LineGraph(edges, bind_data.directed);
	return std::move(state);
}

template <bool Full>
void TransformScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<TransformState<TransformedEdgeRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.source));
		output.SetValue(2, i, Value::BIGINT(row.target));
		output.SetValue(3, i, Value::DOUBLE(row.cost));
		if (Full) {
			output.SetValue(4, i, Value::BIGINT(row.edge));
		} else {
			output.SetValue(4, i, Value::DOUBLE(row.reverse_cost));
		}
	}
	state.offset += count;
}

duckdb::unique_ptr<FunctionData> PostmanBind(ClientContext &, TableFunctionBindInput &input,
                                             duckdb::vector<LogicalType> &return_types,
                                             duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<TransformBindData>();
	ReadEdgesArgument(input, *bind_data);
	names = {"seq", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE,
	                LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> PostmanInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TransformBindData>();
	auto state = duckdb::make_uniq<TransformState<PathRow>>();
	state->rows = ChinesePostman(LoadEdges(context, bind_data.edges_sql), bind_data.directed);
	return std::move(state);
}

void PostmanScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<TransformState<PathRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.node));
		output.SetValue(2, i, Value::BIGINT(row.edge));
		output.SetValue(3, i, Value::DOUBLE(row.cost));
		output.SetValue(4, i, Value::DOUBLE(row.agg_cost));
	}
	state.offset += count;
}

duckdb::unique_ptr<FunctionData> PostmanCostBind(ClientContext &, TableFunctionBindInput &input,
                                                 duckdb::vector<LogicalType> &return_types,
                                                 duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<TransformBindData>();
	ReadEdgesArgument(input, *bind_data);
	names = {"cost"};
	return_types = {LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> PostmanCostInit(ClientContext &context,
                                                             TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TransformBindData>();
	auto state = duckdb::make_uniq<TransformState<Value>>();
	state->rows.push_back(
	    Value::DOUBLE(ChinesePostmanCost(LoadEdges(context, bind_data.edges_sql), bind_data.directed)));
	return std::move(state);
}

void PostmanCostScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<TransformState<Value>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		output.SetValue(0, i, state.rows[state.offset + i]);
	}
	state.offset += count;
}

TableFunctionSet EdgesOnlySet(const char *name, duckdb::table_function_t scan,
                              duckdb::table_function_bind_t bind, duckdb::table_function_init_global_t init,
                              bool accepts_directed) {
	TableFunctionSet set(name);
	const size_t variants = accepts_directed ? 2u : 1u;
	for (size_t with_flag = 0; with_flag < variants; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, scan, bind, init);
		if (accepts_directed) {
			function.named_parameters["directed"] = LogicalType::BOOLEAN;
		}
		set.AddFunction(function);
	}
	return set;
}

} // namespace

TableFunctionSet GetLineGraphFunction() {
	return EdgesOnlySet("duckrouting_line_graph", TransformScan<false>, TransformBind<false>,
	                    TransformInit<false>, true);
}
TableFunctionSet GetLineGraphFullFunction() {
	return EdgesOnlySet("duckrouting_line_graph_full", TransformScan<true>, TransformBind<true>,
	                    TransformInit<true>, false);
}
TableFunctionSet GetChinesePostmanFunction() {
	return EdgesOnlySet("duckrouting_chinese_postman", PostmanScan, PostmanBind, PostmanInit, true);
}
TableFunctionSet GetChinesePostmanCostFunction() {
	return EdgesOnlySet("duckrouting_chinese_postman_cost", PostmanCostScan, PostmanCostBind, PostmanCostInit,
	                    true);
}

} // namespace duckrouting
