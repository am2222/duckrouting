#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <boost/graph/bandwidth.hpp>
#include <boost/graph/betweenness_centrality.hpp>
#include <boost/graph/bipartite.hpp>
#include <boost/graph/boyer_myrvold_planar_test.hpp>
#include <boost/graph/dominator_tree.hpp>
#include <boost/graph/edge_coloring.hpp>
#include <boost/graph/hawick_circuits.hpp>
#include <boost/graph/one_bit_color_map.hpp>
#include <boost/graph/sequential_vertex_coloring.hpp>
#include <boost/graph/stoer_wagner_min_cut.hpp>
#include <boost/property_map/property_map.hpp>

#include <algorithm>
#include <map>
#include <vector>

namespace duckrouting {

namespace {

//! Several of these algorithms want a bare structural graph with no bundled
//! edge properties, and some insist on a mutable interior colour map.
typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS, boost::no_property,
                              boost::property<boost::edge_index_t, std::size_t>>
    PlainGraph;

//! Builds the structural undirected graph, recording which edge id each Boost
//! edge came from so results can be reported in the user's own terms.
PlainGraph BuildPlainGraph(const std::vector<EdgeRow> &edges, VertexIndex &index,
                           std::vector<int64_t> &edge_ids) {
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(),
	                 [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });
	for (size_t i = 0; i < ordered.size(); i++) {
		index.GetOrCreate(ordered[i].source);
		index.GetOrCreate(ordered[i].target);
	}

	PlainGraph graph(index.Size());
	edge_ids.clear();
	for (size_t i = 0; i < ordered.size(); i++) {
		if (!IsTraversable(ordered[i].cost) && !IsTraversable(ordered[i].reverse_cost)) {
			continue;
		}
		uint64_t source = 0;
		uint64_t target = 0;
		index.Find(ordered[i].source, source);
		index.Find(ordered[i].target, target);
		boost::add_edge(source, target, edge_ids.size(), graph);
		edge_ids.push_back(ordered[i].id);
	}
	return graph;
}

} // namespace

std::vector<ColorRow> SequentialVertexColoring(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	std::vector<int64_t> edge_ids;
	auto graph = BuildPlainGraph(edges, index, edge_ids);

	std::vector<std::size_t> color(boost::num_vertices(graph));
	boost::sequential_vertex_coloring(
	    graph, boost::make_iterator_property_map(color.begin(), boost::get(boost::vertex_index, graph)));

	std::vector<ColorRow> rows;
	for (uint64_t v = 0; v < index.Size(); v++) {
		// Boost counts colours from 0; pgRouting reports them from 1.
		rows.push_back(ColorRow {index.IdOf(v), static_cast<int64_t>(color[v]) + 1});
	}
	std::sort(rows.begin(), rows.end(), [](const ColorRow &a, const ColorRow &b) { return a.id < b.id; });
	return rows;
}

std::vector<ColorRow> EdgeColoring(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	std::vector<int64_t> edge_ids;
	auto graph = BuildPlainGraph(edges, index, edge_ids);

	typedef boost::graph_traits<PlainGraph>::edge_descriptor Edge;
	std::map<Edge, std::size_t> storage;
	boost::associative_property_map<std::map<Edge, std::size_t>> color(storage);
	boost::edge_coloring(graph, color);

	std::vector<ColorRow> rows;
	for (auto entry = storage.begin(); entry != storage.end(); ++entry) {
		const std::size_t slot = boost::get(boost::edge_index, graph, entry->first);
		rows.push_back(ColorRow {edge_ids[slot], static_cast<int64_t>(entry->second) + 1});
	}
	std::sort(rows.begin(), rows.end(), [](const ColorRow &a, const ColorRow &b) { return a.id < b.id; });
	return rows;
}

std::vector<ColorRow> Bipartite(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	std::vector<int64_t> edge_ids;
	auto graph = BuildPlainGraph(edges, index, edge_ids);

	std::vector<boost::default_color_type> partition(boost::num_vertices(graph));
	auto partition_map =
	    boost::make_iterator_property_map(partition.begin(), boost::get(boost::vertex_index, graph));

	if (!boost::is_bipartite(graph, boost::get(boost::vertex_index, graph), partition_map)) {
		// pgRouting reports a non-bipartite graph as an empty result.
		return {};
	}

	std::vector<ColorRow> rows;
	for (uint64_t v = 0; v < index.Size(); v++) {
		rows.push_back(
		    ColorRow {index.IdOf(v), partition[v] == boost::color_traits<boost::default_color_type>::white()
		                                 ? 0
		                                 : 1});
	}
	std::sort(rows.begin(), rows.end(), [](const ColorRow &a, const ColorRow &b) { return a.id < b.id; });
	return rows;
}

bool IsPlanar(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	std::vector<int64_t> edge_ids;
	auto graph = BuildPlainGraph(edges, index, edge_ids);
	return boost::boyer_myrvold_planarity_test(graph);
}

std::vector<PairRow> BoyerMyrvold(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	std::vector<int64_t> edge_ids;
	auto graph = BuildPlainGraph(edges, index, edge_ids);

	typedef std::vector<boost::graph_traits<PlainGraph>::edge_descriptor> Rotation;
	std::vector<Rotation> embedding(boost::num_vertices(graph));
	auto embedding_map =
	    boost::make_iterator_property_map(embedding.begin(), boost::get(boost::vertex_index, graph));

	if (!boost::boyer_myrvold_planarity_test(boost::boyer_myrvold_params::graph = graph,
	                                         boost::boyer_myrvold_params::embedding = embedding_map)) {
		// A non-planar graph has no embedding to report.
		return {};
	}

	// The embedding is the edges around each vertex in rotation order.
	std::vector<PairRow> rows;
	for (uint64_t v = 0; v < index.Size(); v++) {
		for (size_t i = 0; i < embedding[v].size(); i++) {
			const uint64_t other = static_cast<uint64_t>(boost::source(embedding[v][i], graph)) == v
			                           ? static_cast<uint64_t>(boost::target(embedding[v][i], graph))
			                           : static_cast<uint64_t>(boost::source(embedding[v][i], graph));
			rows.push_back(PairRow {index.IdOf(v), index.IdOf(other)});
		}
	}
	return rows;
}

int64_t Bandwidth(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	std::vector<int64_t> edge_ids;
	auto graph = BuildPlainGraph(edges, index, edge_ids);
	return static_cast<int64_t>(boost::bandwidth(graph));
}

std::vector<CentralityRow> BetweennessCentrality(const std::vector<EdgeRow> &edges, bool directed) {
	VertexIndex index;
	std::vector<double> centrality;

	if (directed) {
		auto graph = BuildGraph<DirectedGraph>(edges, index);
		centrality.assign(boost::num_vertices(graph), 0);
		boost::brandes_betweenness_centrality(
		    graph, boost::centrality_map(boost::make_iterator_property_map(
		                                     centrality.begin(), boost::get(boost::vertex_index, graph)))
		               .weight_map(boost::get(&RoutingEdge::cost, graph)));
	} else {
		auto graph = BuildGraph<UndirectedGraph>(edges, index);
		centrality.assign(boost::num_vertices(graph), 0);
		boost::brandes_betweenness_centrality(
		    graph, boost::centrality_map(boost::make_iterator_property_map(
		                                     centrality.begin(), boost::get(boost::vertex_index, graph)))
		               .weight_map(boost::get(&RoutingEdge::cost, graph)));
	}

	// Boost's relative_betweenness_centrality always divides by
	// (n-1)(n-2)/2 -- the undirected normalisation. A directed graph has twice
	// as many ordered pairs, so it wants (n-1)(n-2). Scaling here rather than
	// calling that helper keeps both cases right.
	const double n = static_cast<double>(index.Size());
	const double pairs = (n - 1) * (n - 2);
	const double factor = pairs > 0 ? (directed ? 1.0 / pairs : 2.0 / pairs) : 0.0;

	std::vector<CentralityRow> rows;
	for (uint64_t v = 0; v < index.Size(); v++) {
		rows.push_back(CentralityRow {index.IdOf(v), centrality[v] * factor});
	}
	std::sort(rows.begin(), rows.end(),
	          [](const CentralityRow &a, const CentralityRow &b) { return a.vid < b.vid; });
	return rows;
}

std::vector<MinCutRow> StoerWagner(const std::vector<EdgeRow> &edges) {
	// A minimum cut sums edge weights, so the graph must carry each edge once.
	// BuildGraph deliberately adds a parallel edge for cost and another for
	// reverse_cost, which would double the weight of any cut crossing it.
	typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS, boost::no_property, RoutingEdge>
	    CutGraph;

	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(),
	                 [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });

	VertexIndex index;
	for (size_t i = 0; i < ordered.size(); i++) {
		index.GetOrCreate(ordered[i].source);
		index.GetOrCreate(ordered[i].target);
	}
	if (index.Size() < 2) {
		return {};
	}

	CutGraph graph(index.Size());
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
		uint64_t source = 0;
		uint64_t target = 0;
		index.Find(ordered[i].source, source);
		index.Find(ordered[i].target, target);
		boost::add_edge(source, target, RoutingEdge {ordered[i].id, cost}, graph);
	}

	std::vector<bool> parity(boost::num_vertices(graph));
	auto parity_map = boost::make_iterator_property_map(parity.begin(), boost::get(boost::vertex_index, graph));
	const double weight =
	    boost::stoer_wagner_min_cut(graph, boost::get(&RoutingEdge::cost, graph), boost::parity_map(parity_map));

	// The cut is the set of edges whose endpoints landed on opposite sides.
	std::vector<MinCutRow> rows;
	boost::graph_traits<CutGraph>::edge_iterator it, end;
	for (boost::tie(it, end) = boost::edges(graph); it != end; ++it) {
		const uint64_t source = static_cast<uint64_t>(boost::source(*it, graph));
		const uint64_t target = static_cast<uint64_t>(boost::target(*it, graph));
		if (parity[source] == parity[target]) {
			continue;
		}
		rows.push_back(MinCutRow {graph[*it].id, DecodeInfinity(graph[*it].cost), DecodeInfinity(weight)});
	}
	std::sort(rows.begin(), rows.end(), [](const MinCutRow &a, const MinCutRow &b) { return a.edge < b.edge; });
	return rows;
}

namespace {

//! hawick_circuits hands each circuit to the visitor as a vertex sequence.
struct CircuitCollector {
	std::vector<std::vector<uint64_t>> *circuits;

	template <typename Path, typename Graph>
	void cycle(const Path &path, const Graph &) {
		std::vector<uint64_t> vertices;
		for (typename Path::const_iterator it = path.begin(); it != path.end(); ++it) {
			vertices.push_back(static_cast<uint64_t>(*it));
		}
		circuits->push_back(vertices);
	}
};

} // namespace

std::vector<CircuitRow> HawickCircuits(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	auto graph = BuildGraph<DirectedGraph>(edges, index);

	std::vector<std::vector<uint64_t>> circuits;
	CircuitCollector collector {&circuits};
	boost::hawick_circuits(graph, collector);

	std::vector<CircuitRow> rows;
	int64_t path_id = 0;
	for (size_t c = 0; c < circuits.size(); c++) {
		const std::vector<uint64_t> &circuit = circuits[c];
		if (circuit.empty()) {
			continue;
		}
		path_id++;
		const int64_t start_vid = index.IdOf(circuit[0]);
		double agg_cost = 0;
		// A circuit closes back on its start, so walk one step past the end.
		for (size_t i = 0; i <= circuit.size(); i++) {
			const uint64_t node = circuit[i % circuit.size()];
			CircuitRow row {};
			row.path_id = path_id;
			// pgRouting numbers circuit steps from 0.
			row.path_seq = static_cast<int64_t>(i);
			row.start_vid = start_vid;
			row.end_vid = start_vid;
			row.node = index.IdOf(node);
			row.agg_cost = DecodeInfinity(agg_cost);
			if (i < circuit.size()) {
				const uint64_t next = circuit[(i + 1) % circuit.size()];
				bool found = false;
				RoutingEdge best {};
				DirectedGraph::out_edge_iterator it, end;
				for (boost::tie(it, end) = boost::out_edges(node, graph); it != end; ++it) {
					if (static_cast<uint64_t>(boost::target(*it, graph)) != next) {
						continue;
					}
					if (!found || graph[*it].cost < best.cost) {
						best = graph[*it];
						found = true;
					}
				}
				if (!found) {
					break;
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
	}
	return rows;
}

std::vector<DominatorRow> DominatorTree(const std::vector<EdgeRow> &edges, int64_t root) {
	// The dominator tree walks backwards from each vertex, so it needs a graph
	// that stores in-edges as well as out-edges.
	typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS, boost::no_property,
	                              RoutingEdge>
	    BidirectionalGraph;

	VertexIndex index;
	auto graph = BuildGraph<BidirectionalGraph>(edges, index);

	uint64_t source = 0;
	if (!index.Find(root, source)) {
		return {};
	}

	const uint64_t none = boost::graph_traits<BidirectionalGraph>::null_vertex();
	std::vector<uint64_t> dominator(boost::num_vertices(graph), none);
	boost::lengauer_tarjan_dominator_tree(
	    graph, source,
	    boost::make_iterator_property_map(dominator.begin(), boost::get(boost::vertex_index, graph)));

	std::vector<DominatorRow> rows;
	for (uint64_t v = 0; v < index.Size(); v++) {
		// The root, and anything the root cannot reach, has no dominator; 0 is
		// how pgRouting spells that.
		const int64_t idom = dominator[v] == none ? 0 : index.IdOf(dominator[v]);
		rows.push_back(DominatorRow {index.IdOf(v), idom});
	}
	std::sort(rows.begin(), rows.end(),
	          [](const DominatorRow &a, const DominatorRow &b) { return a.vertex_id < b.vertex_id; });
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

struct AnalysisBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	bool directed = true;
	int64_t root = 0;
};

template <typename Row>
struct AnalysisGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

void ReadEdges(TableFunctionBindInput &input, AnalysisBindData &bind_data) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	bind_data.edges_sql = input.inputs[0].GetValue<std::string>();
	for (auto &parameter : input.named_parameters) {
		if (duckdb::StringUtil::CIEquals(parameter.first, "directed")) {
			if (parameter.second.IsNull()) {
				throw BinderException("duckrouting: 'directed' must not be NULL");
			}
			bind_data.directed = parameter.second.GetValue<bool>();
		}
	}
}

//! Two BIGINT columns, used by the colouring functions and the dominator tree.
template <const char *First, const char *Second>
duckdb::unique_ptr<FunctionData> PairBind(ClientContext &, TableFunctionBindInput &input,
                                          duckdb::vector<LogicalType> &return_types,
                                          duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<AnalysisBindData>();
	ReadEdges(input, *bind_data);
	if (input.inputs.size() > 1) {
		if (input.inputs[1].IsNull()) {
			throw BinderException("duckrouting: the root vertex must not be NULL");
		}
		bind_data->root = input.inputs[1].GetValue<int64_t>();
	}
	names = {First, Second};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT};
	return std::move(bind_data);
}

typedef std::vector<ColorRow> (*ColorFunction)(const std::vector<EdgeRow> &);

template <ColorFunction Compute>
duckdb::unique_ptr<GlobalTableFunctionState> ColorInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<AnalysisBindData>();
	auto state = duckdb::make_uniq<AnalysisGlobalState<ColorRow>>();
	state->rows = Compute(LoadEdges(context, bind_data.edges_sql));
	return std::move(state);
}

void ColorScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AnalysisGlobalState<ColorRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(row.id));
		output.SetValue(1, i, Value::BIGINT(row.color));
	}
	state.offset += count;
}

duckdb::unique_ptr<GlobalTableFunctionState> DominatorInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<AnalysisBindData>();
	auto state = duckdb::make_uniq<AnalysisGlobalState<DominatorRow>>();
	state->rows = DominatorTree(LoadEdges(context, bind_data.edges_sql), bind_data.root);
	return std::move(state);
}

void DominatorScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AnalysisGlobalState<DominatorRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(row.vertex_id));
		output.SetValue(1, i, Value::BIGINT(row.idom));
	}
	state.offset += count;
}

//! Single-value answers: is_planar and bandwidth.
template <bool IsBoolean>
duckdb::unique_ptr<FunctionData> ScalarBind(ClientContext &, TableFunctionBindInput &input,
                                            duckdb::vector<LogicalType> &return_types,
                                            duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<AnalysisBindData>();
	ReadEdges(input, *bind_data);
	names = {IsBoolean ? "is_planar" : "bandwidth"};
	return_types = {IsBoolean ? LogicalType::BOOLEAN : LogicalType::BIGINT};
	return std::move(bind_data);
}

template <bool IsBoolean>
duckdb::unique_ptr<GlobalTableFunctionState> ScalarInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<AnalysisBindData>();
	auto state = duckdb::make_uniq<AnalysisGlobalState<Value>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows.push_back(IsBoolean ? Value::BOOLEAN(IsPlanar(edges)) : Value::BIGINT(Bandwidth(edges)));
	return std::move(state);
}

void ScalarScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AnalysisGlobalState<Value>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		output.SetValue(0, i, state.rows[state.offset + i]);
	}
	state.offset += count;
}

//! (vid, centrality)
duckdb::unique_ptr<FunctionData> CentralityBind(ClientContext &, TableFunctionBindInput &input,
                                                duckdb::vector<LogicalType> &return_types,
                                                duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<AnalysisBindData>();
	ReadEdges(input, *bind_data);
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		bind_data->directed = input.inputs[1].GetValue<bool>();
	}
	names = {"vid", "centrality"};
	return_types = {LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> CentralityInit(ClientContext &context,
                                                            TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<AnalysisBindData>();
	auto state = duckdb::make_uniq<AnalysisGlobalState<CentralityRow>>();
	state->rows = BetweennessCentrality(LoadEdges(context, bind_data.edges_sql), bind_data.directed);
	return std::move(state);
}

void CentralityScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AnalysisGlobalState<CentralityRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(row.vid));
		output.SetValue(1, i, Value::DOUBLE(row.centrality));
	}
	state.offset += count;
}

//! (seq, edge, cost, mincut)
duckdb::unique_ptr<FunctionData> MinCutBind(ClientContext &, TableFunctionBindInput &input,
                                            duckdb::vector<LogicalType> &return_types,
                                            duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<AnalysisBindData>();
	ReadEdges(input, *bind_data);
	names = {"seq", "edge", "cost", "mincut"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> MinCutInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<AnalysisBindData>();
	auto state = duckdb::make_uniq<AnalysisGlobalState<MinCutRow>>();
	state->rows = StoerWagner(LoadEdges(context, bind_data.edges_sql));
	return std::move(state);
}

void MinCutScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AnalysisGlobalState<MinCutRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.edge));
		output.SetValue(2, i, Value::DOUBLE(row.cost));
		output.SetValue(3, i, Value::DOUBLE(row.mincut));
	}
	state.offset += count;
}

//! (seq, path_id, path_seq, start_vid, end_vid, node, edge, cost, agg_cost)
duckdb::unique_ptr<FunctionData> CircuitBind(ClientContext &, TableFunctionBindInput &input,
                                             duckdb::vector<LogicalType> &return_types,
                                             duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<AnalysisBindData>();
	ReadEdges(input, *bind_data);
	names = {"seq", "path_id", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE,
	                LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> CircuitInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<AnalysisBindData>();
	auto state = duckdb::make_uniq<AnalysisGlobalState<CircuitRow>>();
	state->rows = HawickCircuits(LoadEdges(context, bind_data.edges_sql));
	return std::move(state);
}

void CircuitScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AnalysisGlobalState<CircuitRow>>();
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

//! (source, target) -- the planar embedding.
duckdb::unique_ptr<FunctionData> EmbeddingBind(ClientContext &, TableFunctionBindInput &input,
                                               duckdb::vector<LogicalType> &return_types,
                                               duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<AnalysisBindData>();
	ReadEdges(input, *bind_data);
	names = {"seq", "source", "target"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> EmbeddingInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<AnalysisBindData>();
	auto state = duckdb::make_uniq<AnalysisGlobalState<PairRow>>();
	state->rows = BoyerMyrvold(LoadEdges(context, bind_data.edges_sql));
	return std::move(state);
}

void EmbeddingScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<AnalysisGlobalState<PairRow>>();
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

char kNodeName[] = "node";
char kEdgeName[] = "edge";
char kColorName[] = "color";
char kVertexIdName[] = "vertex_id";
char kIdomName[] = "idom";

TableFunctionSet OneArgSet(const char *name, duckdb::table_function_t scan, duckdb::table_function_bind_t bind,
                           duckdb::table_function_init_global_t init) {
	TableFunctionSet set(name);
	set.AddFunction(TableFunction({LogicalType::VARCHAR}, scan, bind, init));
	return set;
}

} // namespace

TableFunctionSet GetSequentialVertexColoringFunction() {
	return OneArgSet("duckrouting_sequential_vertex_coloring", ColorScan, PairBind<kNodeName, kColorName>,
	                 ColorInit<SequentialVertexColoring>);
}

TableFunctionSet GetEdgeColoringFunction() {
	return OneArgSet("duckrouting_edge_coloring", ColorScan, PairBind<kEdgeName, kColorName>,
	                 ColorInit<EdgeColoring>);
}

TableFunctionSet GetBipartiteFunction() {
	return OneArgSet("duckrouting_bipartite", ColorScan, PairBind<kNodeName, kColorName>, ColorInit<Bipartite>);
}

TableFunctionSet GetIsPlanarFunction() {
	return OneArgSet("duckrouting_is_planar", ScalarScan, ScalarBind<true>, ScalarInit<true>);
}

TableFunctionSet GetBandwidthFunction() {
	return OneArgSet("duckrouting_bandwidth", ScalarScan, ScalarBind<false>, ScalarInit<false>);
}

TableFunctionSet GetBoyerMyrvoldFunction() {
	return OneArgSet("duckrouting_boyer_myrvold", EmbeddingScan, EmbeddingBind, EmbeddingInit);
}

TableFunctionSet GetStoerWagnerFunction() {
	return OneArgSet("duckrouting_stoer_wagner", MinCutScan, MinCutBind, MinCutInit);
}

TableFunctionSet GetHawickCircuitsFunction() {
	return OneArgSet("duckrouting_hawick_circuits", CircuitScan, CircuitBind, CircuitInit);
}

TableFunctionSet GetBetweennessCentralityFunction() {
	TableFunctionSet set("duckrouting_betweenness_centrality");
	for (size_t with_flag = 0; with_flag < 2; with_flag++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR};
		if (with_flag) {
			arguments.push_back(LogicalType::BOOLEAN);
		}
		TableFunction function(arguments, CentralityScan, CentralityBind, CentralityInit);
		function.named_parameters["directed"] = LogicalType::BOOLEAN;
		set.AddFunction(function);
	}
	return set;
}

TableFunctionSet GetDominatorTreeFunction() {
	TableFunctionSet set("duckrouting_dominator_tree");
	set.AddFunction(TableFunction({LogicalType::VARCHAR, LogicalType::BIGINT}, DominatorScan,
	                              PairBind<kVertexIdName, kIdomName>, DominatorInit));
	return set;
}

} // namespace duckrouting
