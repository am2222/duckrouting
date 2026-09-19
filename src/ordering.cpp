#include "duckrouting/graph_functions.hpp"
#include "duckrouting/compat.hpp"

#include "duckrouting/graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <boost/graph/cuthill_mckee_ordering.hpp>
#include <boost/graph/king_ordering.hpp>
#include <boost/graph/sloan_ordering.hpp>
#include <boost/graph/topological_sort.hpp>
#include <boost/graph/transitive_closure.hpp>

#include <algorithm>
#include <map>
#include <vector>

namespace duckrouting {

std::vector<ClosureRow> TransitiveClosure(const std::vector<EdgeRow> &edges) {
	VertexIndex index;
	auto graph = BuildGraph<DirectedGraph>(edges, index);

	// transitive_closure writes into a fresh graph whose vertices correspond to
	// the input's by index.
	typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS> ClosureGraph;
	ClosureGraph closure;
	boost::transitive_closure(graph, closure);

	std::vector<ClosureRow> rows;
	for (uint64_t v = 0; v < index.Size() && v < boost::num_vertices(closure); v++) {
		ClosureRow row {};
		row.node = index.IdOf(v);
		boost::graph_traits<ClosureGraph>::out_edge_iterator it, end;
		for (boost::tie(it, end) = boost::out_edges(v, closure); it != end; ++it) {
			row.targets.push_back(index.IdOf(static_cast<uint64_t>(boost::target(*it, closure))));
		}
		// Boost emits these in its own order; sorting makes the result stable.
		std::sort(row.targets.begin(), row.targets.end());
		rows.push_back(row);
	}
	std::sort(rows.begin(), rows.end(), [](const ClosureRow &a, const ClosureRow &b) { return a.node < b.node; });
	return rows;
}

namespace {

//! The bandwidth-reduction orderings need interior vertex properties that the
//! routing graph does not carry, and they only care about structure, not cost.
//! So they get their own graph type.
typedef boost::adjacency_list<
    boost::vecS, boost::vecS, boost::undirectedS,
    boost::property<boost::vertex_color_t, boost::default_color_type,
                    boost::property<boost::vertex_degree_t, int, boost::property<boost::vertex_priority_t, double>>>>
    OrderingGraph;

OrderingGraph BuildOrderingGraph(const std::vector<EdgeRow> &edges, VertexIndex &index) {
	for (size_t i = 0; i < edges.size(); i++) {
		index.GetOrCreate(edges[i].source);
		index.GetOrCreate(edges[i].target);
	}
	OrderingGraph graph(index.Size());
	for (size_t i = 0; i < edges.size(); i++) {
		if (!IsTraversable(edges[i].cost) && !IsTraversable(edges[i].reverse_cost)) {
			continue;
		}
		uint64_t source = 0;
		uint64_t target = 0;
		index.Find(edges[i].source, source);
		index.Find(edges[i].target, target);
		boost::add_edge(source, target, graph);
	}
	return graph;
}

} // namespace

std::vector<IdentifierRow> VertexOrdering(const std::vector<EdgeRow> &edges, Ordering ordering) {
	VertexIndex index;
	std::vector<IdentifierRow> rows;

	if (ordering == Ordering::Topological) {
		// Topological sort is the only one of the four that needs direction.
		auto graph = BuildGraph<DirectedGraph>(edges, index);
		std::vector<boost::default_color_type> color(boost::num_vertices(graph));
		std::vector<DirectedGraph::vertex_descriptor> order;
		try {
			boost::topological_sort(graph, std::back_inserter(order),
			                        boost::color_map(boost::make_iterator_property_map(
			                            color.begin(), boost::get(boost::vertex_index, graph))));
		} catch (const boost::not_a_dag &) {
			throw duckdb::InvalidInputException(
			    "duckrouting_topological_sort: the graph contains a cycle, so it is not a DAG");
		}
		// Boost produces reverse topological order.
		std::reverse(order.begin(), order.end());
		for (size_t i = 0; i < order.size(); i++) {
			rows.push_back(IdentifierRow {index.IdOf(static_cast<uint64_t>(order[i]))});
		}
		return rows;
	}

	auto graph = BuildOrderingGraph(edges, index);
	std::vector<OrderingGraph::vertex_descriptor> order;

	if (ordering == Ordering::CuthillMckee) {
		boost::cuthill_mckee_ordering(graph, std::back_inserter(order), boost::get(boost::vertex_color, graph),
		                              boost::make_degree_map(graph));
	} else if (ordering == Ordering::King) {
		boost::king_ordering(graph, std::back_inserter(order), boost::get(boost::vertex_color, graph),
		                     boost::make_degree_map(graph), boost::get(boost::vertex_index, graph));
	} else {
		boost::sloan_ordering(graph, std::back_inserter(order), boost::get(boost::vertex_color, graph),
		                      boost::make_degree_map(graph), boost::get(boost::vertex_priority, graph));
	}

	if (ordering != Ordering::Sloan) {
		// Boost's cuthill_mckee_ordering and king_ordering emit the *reverse*
		// ordering when collected forwards. pgRouting writes into rbegin() to
		// undo that; reversing here is the same thing.
		std::reverse(order.begin(), order.end());
	}

	for (size_t i = 0; i < order.size(); i++) {
		rows.push_back(IdentifierRow {index.IdOf(static_cast<uint64_t>(order[i]))});
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

struct OrderingBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
};

template <typename Row>
struct OrderingGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

void ReadEdgesArgument(TableFunctionBindInput &input, OrderingBindData &bind_data) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	bind_data.edges_sql = input.inputs[0].GetValue<std::string>();
}

//! (seq, node) -- the four ordering functions.
duckdb::unique_ptr<FunctionData> OrderingBind(ClientContext &, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	auto bind_data = duckdb::make_uniq<OrderingBindData>();
	ReadEdgesArgument(input, *bind_data);
	names = {"seq", "node"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT};
	return std::move(bind_data);
}

template <Ordering Which>
duckdb::unique_ptr<GlobalTableFunctionState> OrderingInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<OrderingBindData>();
	auto state = duckdb::make_uniq<OrderingGlobalState<IdentifierRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = VertexOrdering(edges, Which);
	return std::move(state);
}

void OrderingScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OrderingGlobalState<IdentifierRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(state.rows[state.offset + i].id));
	}
	state.offset += count;
}

//! (node, targets) -- transitiveClosure.
duckdb::unique_ptr<FunctionData> ClosureBind(ClientContext &, TableFunctionBindInput &input,
                                             duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	auto bind_data = duckdb::make_uniq<OrderingBindData>();
	ReadEdgesArgument(input, *bind_data);
	names = {"node", "targets"};
	return_types = {LogicalType::BIGINT, LogicalType::LIST(LogicalType::BIGINT)};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> ClosureInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<OrderingBindData>();
	auto state = duckdb::make_uniq<OrderingGlobalState<ClosureRow>>();
	auto edges = LoadEdges(context, bind_data.edges_sql);
	state->rows = TransitiveClosure(edges);
	return std::move(state);
}

void ClosureScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<OrderingGlobalState<ClosureRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(row.node));
		duckdb::vector<Value> targets;
		for (size_t t = 0; t < row.targets.size(); t++) {
			targets.push_back(Value::BIGINT(row.targets[t]));
		}
		output.SetValue(1, i, Value::LIST(LogicalType::BIGINT, targets));
	}
	state.offset += count;
}

template <Ordering Which>
TableFunctionSet OrderingSet(const char *name) {
	TableFunctionSet set(name);
	set.AddFunction(TableFunction({LogicalType::VARCHAR}, OrderingScan, OrderingBind, OrderingInit<Which>));
	return set;
}

} // namespace

TableFunctionSet GetTransitiveClosureFunction() {
	TableFunctionSet set("duckrouting_transitive_closure");
	set.AddFunction(TableFunction({LogicalType::VARCHAR}, ClosureScan, ClosureBind, ClosureInit));
	return set;
}

TableFunctionSet GetCuthillMckeeOrderingFunction() {
	return OrderingSet<Ordering::CuthillMckee>("duckrouting_cuthill_mckee_ordering");
}
TableFunctionSet GetKingOrderingFunction() {
	return OrderingSet<Ordering::King>("duckrouting_king_ordering");
}
TableFunctionSet GetSloanOrderingFunction() {
	return OrderingSet<Ordering::Sloan>("duckrouting_sloan_ordering");
}
TableFunctionSet GetTopologicalSortFunction() {
	return OrderingSet<Ordering::Topological>("duckrouting_topological_sort");
}

} // namespace duckrouting
