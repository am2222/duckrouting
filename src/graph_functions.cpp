#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <boost/graph/biconnected_components.hpp>
#include <boost/graph/connected_components.hpp>
#include <boost/graph/floyd_warshall_shortest.hpp>
#include <boost/graph/johnson_all_pairs_shortest.hpp>
#include <boost/graph/make_connected.hpp>
#include <boost/graph/strong_components.hpp>

#include <algorithm>
#include <limits>
#include <map>

namespace duckrouting {

namespace {

//! pgRouting labels a component by the smallest identifier it contains, rather
//! than by Boost's arbitrary index. This relabels and sorts accordingly.
std::vector<ComponentRow> Relabel(const std::vector<std::pair<int64_t, int64_t>> &index_and_member) {
	std::map<int64_t, int64_t> smallest;
	for (size_t i = 0; i < index_and_member.size(); i++) {
		const int64_t component = index_and_member[i].first;
		const int64_t member = index_and_member[i].second;
		auto entry = smallest.find(component);
		if (entry == smallest.end() || member < entry->second) {
			smallest[component] = member;
		}
	}

	std::vector<ComponentRow> rows;
	rows.reserve(index_and_member.size());
	for (size_t i = 0; i < index_and_member.size(); i++) {
		rows.push_back(ComponentRow {smallest[index_and_member[i].first], index_and_member[i].second});
	}
	std::sort(rows.begin(), rows.end(), [](const ComponentRow &a, const ComponentRow &b) {
		if (a.component != b.component) {
			return a.component < b.component;
		}
		return a.member < b.member;
	});
	return rows;
}

//! Costs come back from the all-pairs algorithms as a dense matrix; this turns
//! the finite off-diagonal entries into rows.
std::vector<CostRow> MatrixToRows(const std::vector<std::vector<double>> &matrix, const VertexIndex &index) {
	std::vector<CostRow> rows;
	for (uint64_t from = 0; from < index.Size(); from++) {
		for (uint64_t to = 0; to < index.Size(); to++) {
			if (from == to) {
				continue;
			}
			const double cost = matrix[from][to];
			if (cost == (std::numeric_limits<double>::max)() || cost == std::numeric_limits<double>::infinity()) {
				continue;
			}
			rows.push_back(CostRow {index.IdOf(from), index.IdOf(to), DecodeInfinity(cost)});
		}
	}
	std::sort(rows.begin(), rows.end(), [](const CostRow &a, const CostRow &b) {
		if (a.start_vid != b.start_vid) {
			return a.start_vid < b.start_vid;
		}
		return a.end_vid < b.end_vid;
	});
	return rows;
}

template <typename Graph>
std::vector<std::vector<double>> EmptyMatrix(const Graph &graph) {
	const size_t n = boost::num_vertices(graph);
	return std::vector<std::vector<double>>(n, std::vector<double>(n, (std::numeric_limits<double>::max)()));
}

//! Records the vertex pairs Boost's make_connected chooses to join.
struct RecordingVisitor {
	std::vector<std::pair<uint64_t, uint64_t>> *added;

	template <typename Vertex, typename Graph>
	void visit_vertex_pair(Vertex u, Vertex v, Graph &) {
		added->push_back(std::make_pair(static_cast<uint64_t>(u), static_cast<uint64_t>(v)));
	}
};

} // namespace

std::vector<CostRow> FloydWarshall(const std::vector<EdgeRow> &edges, bool directed) {
	VertexIndex index;
	if (directed) {
		auto graph = BuildGraph<DirectedGraph>(edges, index);
		auto matrix = EmptyMatrix(graph);
		boost::floyd_warshall_all_pairs_shortest_paths(graph, matrix,
		                                               boost::weight_map(boost::get(&RoutingEdge::cost, graph)));
		return MatrixToRows(matrix, index);
	}
	auto graph = BuildGraph<UndirectedGraph>(edges, index);
	auto matrix = EmptyMatrix(graph);
	boost::floyd_warshall_all_pairs_shortest_paths(graph, matrix,
	                                               boost::weight_map(boost::get(&RoutingEdge::cost, graph)));
	return MatrixToRows(matrix, index);
}

std::vector<CostRow> Johnson(const std::vector<EdgeRow> &edges, bool directed) {
	VertexIndex index;
	if (directed) {
		auto graph = BuildGraph<DirectedGraph>(edges, index);
		auto matrix = EmptyMatrix(graph);
		boost::johnson_all_pairs_shortest_paths(graph, matrix,
		                                        boost::weight_map(boost::get(&RoutingEdge::cost, graph)));
		return MatrixToRows(matrix, index);
	}
	auto graph = BuildGraph<UndirectedGraph>(edges, index);
	auto matrix = EmptyMatrix(graph);
	boost::johnson_all_pairs_shortest_paths(graph, matrix,
	                                        boost::weight_map(boost::get(&RoutingEdge::cost, graph)));
	return MatrixToRows(matrix, index);
}

std::vector<ComponentRow> ConnectedComponents(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	auto graph = BuildGraph<UndirectedGraph>(edges, index);
	std::vector<int> component(boost::num_vertices(graph));
	boost::connected_components(graph, &component[0]);

	std::vector<std::pair<int64_t, int64_t>> pairs;
	for (uint64_t v = 0; v < index.Size(); v++) {
		pairs.push_back(std::make_pair(static_cast<int64_t>(component[v]), index.IdOf(v)));
	}
	return Relabel(pairs);
}

std::vector<ComponentRow> StrongComponents(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	auto graph = BuildGraph<DirectedGraph>(edges, index);
	std::vector<int> component(boost::num_vertices(graph));
	boost::strong_components(graph, boost::make_iterator_property_map(component.begin(),
	                                                                  boost::get(boost::vertex_index, graph)));

	std::vector<std::pair<int64_t, int64_t>> pairs;
	for (uint64_t v = 0; v < index.Size(); v++) {
		pairs.push_back(std::make_pair(static_cast<int64_t>(component[v]), index.IdOf(v)));
	}
	return Relabel(pairs);
}

std::vector<ComponentRow> BiconnectedComponents(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	auto graph = BuildGraph<UndirectedGraph>(edges, index);

	std::map<UndirectedGraph::edge_descriptor, size_t> component_storage;
	boost::associative_property_map<std::map<UndirectedGraph::edge_descriptor, size_t>> component_map(
	    component_storage);
	boost::biconnected_components(graph, component_map);

	// An edge may appear twice when cost and reverse_cost both describe it; the
	// component is a property of the edge id, so collapse duplicates.
	std::map<int64_t, size_t> by_edge_id;
	for (auto entry = component_storage.begin(); entry != component_storage.end(); ++entry) {
		const int64_t edge_id = graph[entry->first].id;
		auto seen = by_edge_id.find(edge_id);
		if (seen == by_edge_id.end() || entry->second < seen->second) {
			by_edge_id[edge_id] = entry->second;
		}
	}

	std::vector<std::pair<int64_t, int64_t>> pairs;
	for (auto entry = by_edge_id.begin(); entry != by_edge_id.end(); ++entry) {
		pairs.push_back(std::make_pair(static_cast<int64_t>(entry->second), entry->first));
	}
	return Relabel(pairs);
}

std::vector<IdentifierRow> ArticulationPoints(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	auto graph = BuildGraph<UndirectedGraph>(edges, index);

	std::vector<UndirectedGraph::vertex_descriptor> points;
	boost::articulation_points(graph, std::back_inserter(points));

	std::vector<IdentifierRow> rows;
	for (size_t i = 0; i < points.size(); i++) {
		rows.push_back(IdentifierRow {index.IdOf(static_cast<uint64_t>(points[i]))});
	}
	std::sort(rows.begin(), rows.end(),
	          [](const IdentifierRow &a, const IdentifierRow &b) { return a.id < b.id; });
	return rows;
}

std::vector<IdentifierRow> Bridges(const std::vector<EdgeRow> &edges) {
	// A bridge is exactly a biconnected component consisting of one edge.
	const auto components = BiconnectedComponents(edges);
	std::map<int64_t, int> sizes;
	for (size_t i = 0; i < components.size(); i++) {
		sizes[components[i].component]++;
	}

	std::vector<IdentifierRow> rows;
	for (size_t i = 0; i < components.size(); i++) {
		if (sizes[components[i].component] == 1) {
			rows.push_back(IdentifierRow {components[i].member});
		}
	}
	std::sort(rows.begin(), rows.end(),
	          [](const IdentifierRow &a, const IdentifierRow &b) { return a.id < b.id; });
	return rows;
}

std::vector<PairRow> MakeConnected(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	auto graph = BuildGraph<UndirectedGraph>(edges, index);

	std::vector<std::pair<uint64_t, uint64_t>> added;
	RecordingVisitor visitor {&added};
	boost::make_connected(graph, boost::get(boost::vertex_index, graph), visitor);

	std::vector<PairRow> rows;
	for (size_t i = 0; i < added.size(); i++) {
		rows.push_back(PairRow {index.IdOf(added[i].first), index.IdOf(added[i].second)});
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
using duckdb::TableFunction;
using duckdb::TableFunctionBindInput;
using duckdb::TableFunctionInitInput;
using duckdb::TableFunctionInput;
using duckdb::TableFunctionSet;
using duckdb::Value;

struct GraphBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	bool directed = true;
};

template <typename Row>
struct GraphGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

GraphBindData &ReadGraphArguments(TableFunctionBindInput &input, duckdb::unique_ptr<GraphBindData> &bind_data) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bool directed = input.inputs.size() > 1 && !input.inputs[1].IsNull() ? input.inputs[1].GetValue<bool>() : true;
	for (auto &parameter : input.named_parameters) {
		if (duckdb::StringUtil::CIEquals(parameter.first, "directed")) {
			if (parameter.second.IsNull()) {
				throw BinderException("duckrouting: 'directed' must not be NULL");
			}
			directed = parameter.second.GetValue<bool>();
		}
	}
	bind_data->directed = directed;
	return *bind_data;
}

//! (start_vid, end_vid, agg_cost) -- floydWarshall and johnson.
duckdb::unique_ptr<FunctionData> AllPairsBind(ClientContext &, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types,
                                              duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<GraphBindData>();
	ReadGraphArguments(input, bind_data);
	names = {"start_vid", "end_vid", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

template <bool IsJohnson>
duckdb::unique_ptr<GlobalTableFunctionState> AllPairsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<GraphBindData>();
	auto state = duckdb::make_uniq<GraphGlobalState<CostRow>>();
	// Neither function reports an edge back, so `id` is optional here.
	auto edges = LoadEdges(context, bind_data.edges_sql, false);
	state->rows = IsJohnson ? Johnson(edges, bind_data.directed) : FloydWarshall(edges, bind_data.directed);
	return std::move(state);
}

void AllPairsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<GraphGlobalState<CostRow>>();
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

//! (seq, component, node|edge) -- the three component functions.
template <const char *MemberName>
duckdb::unique_ptr<FunctionData> ComponentBind(ClientContext &, TableFunctionBindInput &input,
                                               duckdb::vector<LogicalType> &return_types,
                                               duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<GraphBindData>();
	ReadGraphArguments(input, bind_data);
	names = {"seq", "component", MemberName};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT};
	return std::move(bind_data);
}

typedef std::vector<ComponentRow> (*ComponentFunction)(const std::vector<EdgeRow> &);

template <ComponentFunction Compute>
duckdb::unique_ptr<GlobalTableFunctionState> ComponentInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<GraphBindData>();
	auto state = duckdb::make_uniq<GraphGlobalState<ComponentRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = Compute(edges);
	return std::move(state);
}

void ComponentScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<GraphGlobalState<ComponentRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.component));
		output.SetValue(2, i, Value::BIGINT(row.member));
	}
	state.offset += count;
}

//! A bare identifier column -- articulationPoints and bridges.
template <const char *ColumnName>
duckdb::unique_ptr<FunctionData> IdentifierBind(ClientContext &, TableFunctionBindInput &input,
                                                duckdb::vector<LogicalType> &return_types,
                                                duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<GraphBindData>();
	ReadGraphArguments(input, bind_data);
	names = {ColumnName};
	return_types = {LogicalType::BIGINT};
	return std::move(bind_data);
}

typedef std::vector<IdentifierRow> (*IdentifierFunction)(const std::vector<EdgeRow> &);

template <IdentifierFunction Compute>
duckdb::unique_ptr<GlobalTableFunctionState> IdentifierInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<GraphBindData>();
	auto state = duckdb::make_uniq<GraphGlobalState<IdentifierRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = Compute(edges);
	return std::move(state);
}

void IdentifierScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<GraphGlobalState<IdentifierRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		output.SetValue(0, i, Value::BIGINT(state.rows[state.offset + i].id));
	}
	state.offset += count;
}

//! (seq, start_vid, end_vid) -- makeConnected.
duckdb::unique_ptr<FunctionData> MakeConnectedBind(ClientContext &, TableFunctionBindInput &input,
                                                   duckdb::vector<LogicalType> &return_types,
                                                   duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<GraphBindData>();
	ReadGraphArguments(input, bind_data);
	names = {"seq", "start_vid", "end_vid"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> MakeConnectedInit(ClientContext &context,
                                                               TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<GraphBindData>();
	auto state = duckdb::make_uniq<GraphGlobalState<PairRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = MakeConnected(edges);
	return std::move(state);
}

void MakeConnectedScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<GraphGlobalState<PairRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.start_vid));
		output.SetValue(2, i, Value::BIGINT(row.end_vid));
	}
	state.offset += count;
}

//! These functions take only the edges query, optionally with a directed flag.
TableFunctionSet SimpleSet(const char *name, duckdb::table_function_t scan, duckdb::table_function_bind_t bind,
                           duckdb::table_function_init_global_t init, bool accepts_directed) {
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

char kNodeColumn[] = "node";
char kEdgeColumn[] = "edge";

} // namespace

TableFunctionSet GetFloydWarshallFunction() {
	return SimpleSet("duckrouting_floyd_warshall", AllPairsScan, AllPairsBind, AllPairsInit<false>, true);
}

TableFunctionSet GetJohnsonFunction() {
	return SimpleSet("duckrouting_johnson", AllPairsScan, AllPairsBind, AllPairsInit<true>, true);
}

TableFunctionSet GetConnectedComponentsFunction() {
	return SimpleSet("duckrouting_connected_components", ComponentScan, ComponentBind<kNodeColumn>,
	                 ComponentInit<ConnectedComponents>, false);
}

TableFunctionSet GetStrongComponentsFunction() {
	return SimpleSet("duckrouting_strong_components", ComponentScan, ComponentBind<kNodeColumn>,
	                 ComponentInit<StrongComponents>, false);
}

TableFunctionSet GetBiconnectedComponentsFunction() {
	return SimpleSet("duckrouting_biconnected_components", ComponentScan, ComponentBind<kEdgeColumn>,
	                 ComponentInit<BiconnectedComponents>, false);
}

TableFunctionSet GetArticulationPointsFunction() {
	return SimpleSet("duckrouting_articulation_points", IdentifierScan, IdentifierBind<kNodeColumn>,
	                 IdentifierInit<ArticulationPoints>, false);
}

TableFunctionSet GetBridgesFunction() {
	return SimpleSet("duckrouting_bridges", IdentifierScan, IdentifierBind<kEdgeColumn>, IdentifierInit<Bridges>,
	                 false);
}

TableFunctionSet GetMakeConnectedFunction() {
	return SimpleSet("duckrouting_make_connected", MakeConnectedScan, MakeConnectedBind, MakeConnectedInit, false);
}

} // namespace duckrouting
