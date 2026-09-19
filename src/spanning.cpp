#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <boost/graph/connected_components.hpp>
#include <boost/graph/kruskal_min_spanning_tree.hpp>
#include <boost/graph/prim_minimum_spanning_tree.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <limits>
#include <set>

namespace duckrouting {

namespace {

//! One side of a spanning-tree edge, kept in edge-id order so that traversals
//! visit neighbours the same way pgRouting's do.
struct TreeArc {
	uint64_t to;
	int64_t edge;
	double cost;
};

//! Cheapest cost recorded for an edge id, so a tree edge reports the weight
//! that actually got it selected.
std::map<int64_t, double> EdgeCosts(const std::vector<EdgeRow> &edges) {
	std::map<int64_t, double> costs;
	for (size_t i = 0; i < edges.size(); i++) {
		const EdgeRow &edge = edges[i];
		if (IsTraversable(edge.cost)) {
			auto seen = costs.find(edge.id);
			if (seen == costs.end() || edge.cost < seen->second) {
				costs[edge.id] = edge.cost;
			}
		}
		if (IsTraversable(edge.reverse_cost)) {
			auto seen = costs.find(edge.id);
			if (seen == costs.end() || edge.reverse_cost < seen->second) {
				costs[edge.id] = edge.reverse_cost;
			}
		}
	}
	return costs;
}

std::vector<SpanningEdgeRow> ToRows(const std::set<int64_t> &ids, const std::map<int64_t, double> &costs) {
	std::vector<SpanningEdgeRow> rows;
	for (auto it = ids.begin(); it != ids.end(); ++it) {
		auto cost = costs.find(*it);
		rows.push_back(SpanningEdgeRow {*it, cost == costs.end() ? 0 : DecodeInfinity(cost->second)});
	}
	return rows;
}

//! The set of edge ids forming the minimum spanning forest.
std::set<int64_t> SpanningEdgeIds(const UndirectedGraph &graph, const VertexIndex &index, bool use_prim) {
	std::set<int64_t> chosen;
	if (!use_prim) {
		std::vector<UndirectedGraph::edge_descriptor> tree;
		boost::kruskal_minimum_spanning_tree(graph, std::back_inserter(tree),
		                                     boost::weight_map(boost::get(&RoutingEdge::cost, graph)));
		for (size_t i = 0; i < tree.size(); i++) {
			chosen.insert(graph[tree[i]].id);
		}
		return chosen;
	}

	// Prim grows from a single root, so a disconnected graph needs one run per
	// component to produce the whole forest.
	std::vector<int> component(boost::num_vertices(graph));
	const int components = boost::connected_components(graph, &component[0]);

	for (int c = 0; c < components; c++) {
		uint64_t root = 0;
		bool has_root = false;
		for (uint64_t v = 0; v < index.Size(); v++) {
			if (component[v] == c) {
				root = v;
				has_root = true;
				break;
			}
		}
		if (!has_root) {
			continue;
		}

		std::vector<UndirectedGraph::vertex_descriptor> predecessor(boost::num_vertices(graph));
		boost::prim_minimum_spanning_tree(
		    graph, &predecessor[0],
		    boost::root_vertex(static_cast<UndirectedGraph::vertex_descriptor>(root))
		        .weight_map(boost::get(&RoutingEdge::cost, graph)));

		for (uint64_t v = 0; v < index.Size(); v++) {
			if (component[v] != c || static_cast<uint64_t>(predecessor[v]) == v) {
				continue;
			}
			// Recover the edge Prim used between v and its predecessor.
			const uint64_t parent = static_cast<uint64_t>(predecessor[v]);
			bool found = false;
			RoutingEdge best {};
			UndirectedGraph::out_edge_iterator it, end;
			for (boost::tie(it, end) = boost::out_edges(v, graph); it != end; ++it) {
				if (static_cast<uint64_t>(boost::target(*it, graph)) != parent) {
					continue;
				}
				if (!found || graph[*it].cost < best.cost) {
					best = graph[*it];
					found = true;
				}
			}
			if (found) {
				chosen.insert(best.id);
			}
		}
	}
	return chosen;
}

//! Adjacency of the spanning forest only, in edge-id order.
std::vector<std::vector<TreeArc>> TreeAdjacency(const std::vector<EdgeRow> &edges, const VertexIndex &index,
                                                const std::set<int64_t> &chosen) {
	std::vector<std::vector<TreeArc>> adjacency(index.Size());
	std::vector<EdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(),
	                 [](const EdgeRow &a, const EdgeRow &b) { return a.id < b.id; });

	for (size_t i = 0; i < ordered.size(); i++) {
		const EdgeRow &edge = ordered[i];
		if (!chosen.count(edge.id)) {
			continue;
		}
		uint64_t source = 0;
		uint64_t target = 0;
		if (!index.Find(edge.source, source) || !index.Find(edge.target, target)) {
			continue;
		}
		double cost = IsTraversable(edge.cost) ? edge.cost : edge.reverse_cost;
		if (IsTraversable(edge.cost) && IsTraversable(edge.reverse_cost)) {
			cost = (std::min)(edge.cost, edge.reverse_cost);
		}
		// A spanning tree is undirected, so both ends carry the arc.
		adjacency[source].push_back(TreeArc {target, edge.id, cost});
		adjacency[target].push_back(TreeArc {source, edge.id, cost});
	}
	return adjacency;
}

} // namespace

std::vector<SpanningEdgeRow> Kruskal(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	auto graph = BuildGraph<UndirectedGraph>(edges, index);
	return ToRows(SpanningEdgeIds(graph, index, false), EdgeCosts(edges));
}

std::vector<SpanningEdgeRow> Prim(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	auto graph = BuildGraph<UndirectedGraph>(edges, index);
	return ToRows(SpanningEdgeIds(graph, index, true), EdgeCosts(edges));
}

std::vector<DrivingDistanceRow> SpanningTraversal(const std::vector<EdgeRow> &edges, bool use_prim,
                                                  const std::vector<int64_t> &roots, Traversal traversal,
                                                  double limit) {
	VertexIndex index;
	auto graph = BuildGraph<UndirectedGraph>(edges, index);
	const auto chosen = SpanningEdgeIds(graph, index, use_prim);
	const auto adjacency = TreeAdjacency(edges, index, chosen);

	std::vector<int64_t> ordered_roots(roots);
	std::sort(ordered_roots.begin(), ordered_roots.end());
	ordered_roots.erase(std::unique(ordered_roots.begin(), ordered_roots.end()), ordered_roots.end());

	std::vector<DrivingDistanceRow> rows;
	for (size_t r = 0; r < ordered_roots.size(); r++) {
		const int64_t root_id = ordered_roots[r];
		uint64_t root = 0;
		if (!index.Find(root_id, root)) {
			// pgRouting still reports a lone row for a root outside the graph.
			rows.push_back(DrivingDistanceRow {0, root_id, root_id, root_id, -1, 0, 0});
			continue;
		}

		std::vector<bool> seen(index.Size(), false);

		// Each pending entry carries everything the output row needs.
		struct Pending {
			uint64_t vertex;
			int64_t pred;
			int64_t edge;
			double cost;
			int64_t depth;
			double agg_cost;
		};
		std::deque<Pending> queue;
		queue.push_back(Pending {root, root_id, -1, 0, 0, 0});

		while (!queue.empty()) {
			Pending current;
			if (traversal == Traversal::Bfs) {
				current = queue.front();
				queue.pop_front();
			} else {
				current = queue.back();
				queue.pop_back();
			}
			if (seen[current.vertex]) {
				continue;
			}
			seen[current.vertex] = true;

			rows.push_back(DrivingDistanceRow {current.depth, root_id, current.pred, index.IdOf(current.vertex),
			                                   current.edge, DecodeInfinity(current.cost),
			                                   DecodeInfinity(current.agg_cost)});

			// Children are pushed in edge-id order; a stack needs them
			// reversed so they come back off in that same order.
			const std::vector<TreeArc> &neighbours = adjacency[current.vertex];
			for (size_t i = 0; i < neighbours.size(); i++) {
				const TreeArc &arc =
				    traversal == Traversal::Bfs ? neighbours[i] : neighbours[neighbours.size() - 1 - i];
				if (seen[arc.to]) {
					continue;
				}
				const int64_t depth = current.depth + 1;
				const double agg_cost = current.agg_cost + arc.cost;
				if (traversal == Traversal::DfsCost) {
					if (agg_cost > limit) {
						continue;
					}
				} else if (static_cast<double>(depth) > limit) {
					continue;
				}
				queue.push_back(
				    Pending {arc.to, index.IdOf(current.vertex), arc.edge, arc.cost, depth, agg_cost});
			}
		}
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

struct SpanningBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::vector<int64_t> roots;
	bool use_prim = false;
	Traversal traversal = Traversal::Bfs;
	double limit = 0;
};

template <typename Row>
struct SpanningGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

std::vector<int64_t> RootIds(const Value &value) {
	if (value.IsNull()) {
		throw BinderException("duckrouting: the root vertex must not be NULL");
	}
	if (value.type().id() != LogicalTypeId::LIST) {
		return std::vector<int64_t>(1, value.GetValue<int64_t>());
	}
	std::vector<int64_t> ids;
	for (auto &child : duckdb::ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw BinderException("duckrouting: the root vertex list must not contain NULL");
		}
		ids.push_back(child.GetValue<int64_t>());
	}
	return ids;
}

//! (edge, cost) -- kruskal and prim.
duckdb::unique_ptr<FunctionData> ForestBind(ClientContext &, TableFunctionBindInput &input,
                                            duckdb::vector<LogicalType> &return_types,
                                            duckdb::vector<std::string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<SpanningBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	names = {"edge", "cost"};
	return_types = {LogicalType::BIGINT, LogicalType::DOUBLE};
	return std::move(bind_data);
}

template <bool UsePrim>
duckdb::unique_ptr<GlobalTableFunctionState> ForestInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SpanningBindData>();
	auto state = duckdb::make_uniq<SpanningGlobalState<SpanningEdgeRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = UsePrim ? Prim(edges) : Kruskal(edges);
	return std::move(state);
}

void ForestScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SpanningGlobalState<SpanningEdgeRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(row.edge));
		output.SetValue(1, i, Value::DOUBLE(row.cost));
	}
	state.offset += count;
}

//! The traversal variants all share pgRouting's drivingDistance column shape.
template <bool UsePrim, Traversal Mode>
duckdb::unique_ptr<FunctionData> TraversalBind(ClientContext &, TableFunctionBindInput &input,
                                               duckdb::vector<LogicalType> &return_types,
                                               duckdb::vector<std::string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<SpanningBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->roots = RootIds(input.inputs[1]);
	bind_data->use_prim = UsePrim;
	bind_data->traversal = Mode;

	if (Mode == Traversal::DfsCost) {
		if (input.inputs[2].IsNull()) {
			throw BinderException("duckrouting: distance must not be NULL");
		}
		bind_data->limit = input.inputs[2].GetValue<double>();
	} else {
		// max_depth defaults to unbounded, as it does in pgRouting.
		bind_data->limit = input.inputs.size() > 2 && !input.inputs[2].IsNull()
		                       ? static_cast<double>(input.inputs[2].GetValue<int64_t>())
		                       : static_cast<double>(std::numeric_limits<int64_t>::max());
		for (auto &parameter : input.named_parameters) {
			if (duckdb::StringUtil::CIEquals(parameter.first, "max_depth")) {
				if (parameter.second.IsNull()) {
					throw BinderException("duckrouting: 'max_depth' must not be NULL");
				}
				bind_data->limit = static_cast<double>(parameter.second.GetValue<int64_t>());
			}
		}
		if (bind_data->limit < 0) {
			throw BinderException("duckrouting: 'max_depth' must not be negative");
		}
	}

	names = {"seq", "depth", "start_vid", "pred", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> TraversalInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SpanningBindData>();
	auto state = duckdb::make_uniq<SpanningGlobalState<DrivingDistanceRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = SpanningTraversal(edges, bind_data.use_prim, bind_data.roots, bind_data.traversal,
	                                bind_data.limit);
	return std::move(state);
}

void TraversalScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SpanningGlobalState<DrivingDistanceRow>>();
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

template <bool UsePrim, Traversal Mode>
TableFunctionSet TraversalSet(const char *name) {
	TableFunctionSet set(name);
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t root_is_list = 0; root_is_list < 2; root_is_list++) {
		const LogicalType root = root_is_list ? list : LogicalType::BIGINT;
		if (Mode == Traversal::DfsCost) {
			// distance is required for the DD variants.
			TableFunction function({LogicalType::VARCHAR, root, LogicalType::DOUBLE}, TraversalScan,
			                       TraversalBind<UsePrim, Mode>, TraversalInit);
			set.AddFunction(function);
			continue;
		}
		for (size_t with_depth = 0; with_depth < 2; with_depth++) {
			duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, root};
			if (with_depth) {
				arguments.push_back(LogicalType::BIGINT);
			}
			TableFunction function(arguments, TraversalScan, TraversalBind<UsePrim, Mode>, TraversalInit);
			function.named_parameters["max_depth"] = LogicalType::BIGINT;
			set.AddFunction(function);
		}
	}
	return set;
}

} // namespace

TableFunctionSet GetKruskalFunction() {
	TableFunctionSet set("duckrouting_kruskal");
	set.AddFunction(TableFunction({LogicalType::VARCHAR}, ForestScan, ForestBind, ForestInit<false>));
	return set;
}

TableFunctionSet GetPrimFunction() {
	TableFunctionSet set("duckrouting_prim");
	set.AddFunction(TableFunction({LogicalType::VARCHAR}, ForestScan, ForestBind, ForestInit<true>));
	return set;
}

TableFunctionSet GetKruskalBFSFunction() {
	return TraversalSet<false, Traversal::Bfs>("duckrouting_kruskal_bfs");
}
TableFunctionSet GetKruskalDFSFunction() {
	return TraversalSet<false, Traversal::DfsDepth>("duckrouting_kruskal_dfs");
}
TableFunctionSet GetKruskalDDFunction() {
	return TraversalSet<false, Traversal::DfsCost>("duckrouting_kruskal_dd");
}
TableFunctionSet GetPrimBFSFunction() {
	return TraversalSet<true, Traversal::Bfs>("duckrouting_prim_bfs");
}
TableFunctionSet GetPrimDFSFunction() {
	return TraversalSet<true, Traversal::DfsDepth>("duckrouting_prim_dfs");
}
TableFunctionSet GetPrimDDFunction() {
	return TraversalSet<true, Traversal::DfsCost>("duckrouting_prim_dd");
}

} // namespace duckrouting
