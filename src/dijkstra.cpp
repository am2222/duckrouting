#include "duckrouting/dijkstra.hpp"

#include "duckrouting/graph.hpp"

#include <boost/graph/dijkstra_shortest_paths.hpp>

#include <algorithm>
#include <limits>

namespace duckrouting {

namespace {

//! Cheapest edge from `from` to `to`. Dijkstra relaxes parallel edges by their
//! minimum, so the cheapest one is the edge that the shortest path actually used.
template <typename Graph>
bool CheapestEdge(const Graph &graph, uint64_t from, uint64_t to, RoutingEdge &result) {
	bool found = false;
	typename boost::graph_traits<Graph>::out_edge_iterator it, end;
	for (boost::tie(it, end) = boost::out_edges(from, graph); it != end; ++it) {
		if (static_cast<uint64_t>(boost::target(*it, graph)) != to) {
			continue;
		}
		const RoutingEdge &candidate = graph[*it];
		if (!found || candidate.cost < result.cost) {
			result = candidate;
			found = true;
		}
	}
	return found;
}

template <typename Graph>
std::vector<PathRow> Solve(const Graph &graph, const VertexIndex &index, uint64_t source, uint64_t sink,
                           int64_t start_vid, int64_t end_vid) {
	std::vector<uint64_t> predecessor(boost::num_vertices(graph));
	std::vector<double> distance(boost::num_vertices(graph));

	boost::dijkstra_shortest_paths(
	    graph, source,
	    boost::predecessor_map(boost::make_iterator_property_map(predecessor.begin(), boost::get(boost::vertex_index, graph)))
	        .distance_map(boost::make_iterator_property_map(distance.begin(), boost::get(boost::vertex_index, graph)))
	        .weight_map(boost::get(&RoutingEdge::cost, graph))
	        // pgRouting uses a true infinity rather than Boost's default of
	        // numeric_limits<double>::max(), so that an Infinity edge cost
	        // propagates instead of overflowing.
	        .distance_inf(std::numeric_limits<double>::infinity()));

	// Boost leaves a vertex as its own predecessor when it was never reached.
	if (predecessor[sink] == sink) {
		return {};
	}

	// Walk the predecessor chain back to the source, then flip it.
	std::vector<uint64_t> path;
	for (uint64_t at = sink;; at = predecessor[at]) {
		path.push_back(at);
		if (at == source) {
			break;
		}
	}
	std::reverse(path.begin(), path.end());

	std::vector<PathRow> rows;
	rows.reserve(path.size());
	double agg_cost = 0;
	for (size_t i = 0; i < path.size(); i++) {
		PathRow row {};
		row.path_seq = static_cast<int64_t>(i) + 1;
		row.start_vid = start_vid;
		row.end_vid = end_vid;
		row.node = index.IdOf(path[i]);
		row.agg_cost = agg_cost;
		if (i + 1 < path.size()) {
			RoutingEdge used {};
			if (!CheapestEdge(graph, path[i], path[i + 1], used)) {
				// Unreachable in practice: the path came out of this same graph.
				return {};
			}
			row.edge = used.id;
			row.cost = used.cost;
			agg_cost += used.cost;
		} else {
			// pgRouting terminates a path with edge -1 and cost 0.
			row.edge = -1;
			row.cost = 0;
		}
		rows.push_back(row);
	}
	return rows;
}

} // namespace

std::vector<PathRow> Dijkstra(const std::vector<EdgeRow> &edges, int64_t start_vid, int64_t end_vid, bool directed) {
	// pgRouting drops pairs whose endpoints coincide rather than emitting a
	// zero-length path.
	if (start_vid == end_vid) {
		return {};
	}

	VertexIndex index;
	uint64_t source = 0;
	uint64_t sink = 0;
	if (directed) {
		auto graph = BuildGraph<DirectedGraph>(edges, index);
		if (!index.Find(start_vid, source) || !index.Find(end_vid, sink)) {
			return {};
		}
		return Solve(graph, index, source, sink, start_vid, end_vid);
	}
	auto graph = BuildGraph<UndirectedGraph>(edges, index);
	if (!index.Find(start_vid, source) || !index.Find(end_vid, sink)) {
		return {};
	}
	return Solve(graph, index, source, sink, start_vid, end_vid);
}

} // namespace duckrouting

// ---------------------------------------------------------------------------
// DuckDB table function binding
// ---------------------------------------------------------------------------

namespace duckrouting {

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
using duckdb::Value;

struct DijkstraBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	int64_t start_vid = 0;
	int64_t end_vid = 0;
	bool directed = true;
};

struct DijkstraGlobalState : public GlobalTableFunctionState {
	std::vector<PathRow> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

duckdb::unique_ptr<FunctionData> DijkstraBind(ClientContext &context, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types,
                                              duckdb::vector<std::string> &names) {
	for (idx_t i = 0; i < input.inputs.size(); i++) {
		if (input.inputs[i].IsNull()) {
			throw BinderException("duckrouting_dijkstra: arguments must not be NULL");
		}
	}

	auto bind_data = duckdb::make_uniq<DijkstraBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->start_vid = input.inputs[1].GetValue<int64_t>();
	bind_data->end_vid = input.inputs[2].GetValue<int64_t>();
	if (input.inputs.size() > 3) {
		bind_data->directed = input.inputs[3].GetValue<bool>();
	}
	// `directed => false` is how pgRouting spells it, so accept it as a named
	// parameter too.
	for (auto &parameter : input.named_parameters) {
		if (duckdb::StringUtil::CIEquals(parameter.first, "directed")) {
			if (parameter.second.IsNull()) {
				throw BinderException("duckrouting_dijkstra: 'directed' must not be NULL");
			}
			bind_data->directed = parameter.second.GetValue<bool>();
		}
	}

	names = {"seq", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> DijkstraInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<DijkstraBindData>();
	auto state = duckdb::make_uniq<DijkstraGlobalState>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = Dijkstra(edges, bind_data.start_vid, bind_data.end_vid, bind_data.directed);
	return std::move(state);
}

void DijkstraScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<DijkstraGlobalState>();
	const idx_t remaining = state.rows.size() - state.offset;
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, remaining);
	output.SetCardinality(count);

	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		// `seq` numbers the whole result, `path_seq` numbers within one path.
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

} // namespace

duckdb::TableFunctionSet GetDijkstraFunction() {
	duckdb::TableFunctionSet set("duckrouting_dijkstra");

	TableFunction implicit({LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT}, DijkstraScan,
	                       DijkstraBind, DijkstraInit);
	implicit.named_parameters["directed"] = LogicalType::BOOLEAN;
	set.AddFunction(implicit);

	TableFunction explicit_directed(
	    {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BOOLEAN}, DijkstraScan,
	    DijkstraBind, DijkstraInit);
	set.AddFunction(explicit_directed);

	return set;
}

} // namespace duckrouting
