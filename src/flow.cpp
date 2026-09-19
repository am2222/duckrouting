#include "duckrouting/graph_functions.hpp"

#include "duckrouting/graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <boost/graph/boykov_kolmogorov_max_flow.hpp>
#include <boost/graph/edmonds_karp_max_flow.hpp>
#include <boost/graph/find_flow_cost.hpp>
#include <boost/graph/max_cardinality_matching.hpp>
#include <boost/graph/push_relabel_max_flow.hpp>
#include <boost/graph/successive_shortest_path_nonnegative_weights.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace duckrouting {

namespace {

typedef boost::adjacency_list_traits<boost::vecS, boost::vecS, boost::directedS> FlowTraits;

//! Boost's max-flow algorithms need capacity, residual capacity and a pointer
//! from each arc to its reverse. The min-cost variant additionally needs a
//! weight.
typedef boost::adjacency_list<
    boost::vecS, boost::vecS, boost::directedS, boost::no_property,
    boost::property<boost::edge_capacity_t, double,
                    boost::property<boost::edge_residual_capacity_t, double,
                                    boost::property<boost::edge_reverse_t, FlowTraits::edge_descriptor,
                                                    boost::property<boost::edge_weight_t, double>>>>>
    FlowGraph;

typedef boost::graph_traits<FlowGraph>::edge_descriptor FlowEdge;
typedef boost::graph_traits<FlowGraph>::vertex_descriptor FlowVertex;

//! Adds one usable arc plus the zero-capacity partner Boost needs for residual
//! bookkeeping. The two directions of an input edge are added as *independent*
//! arcs rather than paired with each other: pairing them lets push-relabel
//! settle with flow circulating around cycles, which is a valid maximum but
//! reports flow on edges that carry none of it.
FlowEdge AddArc(FlowGraph &graph, FlowVertex from, FlowVertex to, double capacity, double weight) {
	FlowEdge forward, backward;
	bool ok = false;
	boost::tie(forward, ok) = boost::add_edge(from, to, graph);
	boost::tie(backward, ok) = boost::add_edge(to, from, graph);

	boost::put(boost::edge_capacity, graph, forward, capacity);
	boost::put(boost::edge_capacity, graph, backward, 0);
	boost::put(boost::edge_reverse, graph, forward, backward);
	boost::put(boost::edge_reverse, graph, backward, forward);
	boost::put(boost::edge_weight, graph, forward, weight);
	// The residual partner must undo the cost when flow is pushed back.
	boost::put(boost::edge_weight, graph, backward, -weight);
	return forward;
}

//! Everything one solved flow problem needs to report itself.
struct FlowProblem {
	FlowGraph graph;
	VertexIndex index;
	FlowVertex super_source = 0;
	FlowVertex super_sink = 0;
	bool solvable = false;
	//! For each reportable arc: which input edge it came from, and which way
	//! round it runs in the user's terms.
	std::vector<FlowEdge> arcs;
	std::vector<int64_t> arc_edge_ids;
	std::vector<int64_t> arc_from;
	std::vector<int64_t> arc_to;
};

//! Builds the flow network. Several sources or sinks are handled by attaching a
//! super-source and super-sink with unbounded capacity, which is how pgRouting
//! turns a many-to-many question into a single max-flow problem.
FlowProblem BuildFlowProblem(const std::vector<FlowEdgeRow> &edges, const std::vector<int64_t> &sources,
                             const std::vector<int64_t> &sinks, bool use_cost, bool unit_capacity) {
	FlowProblem problem;

	std::vector<FlowEdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(),
	                 [](const FlowEdgeRow &a, const FlowEdgeRow &b) { return a.id < b.id; });

	for (size_t i = 0; i < ordered.size(); i++) {
		problem.index.GetOrCreate(ordered[i].source);
		problem.index.GetOrCreate(ordered[i].target);
	}
	const uint64_t real_vertices = problem.index.Size();
	// Two extra slots for the super-source and super-sink.
	problem.graph = FlowGraph(real_vertices + 2);
	problem.super_source = real_vertices;
	problem.super_sink = real_vertices + 1;

	for (size_t i = 0; i < ordered.size(); i++) {
		const FlowEdgeRow &edge = ordered[i];
		const double forward = unit_capacity ? (IsTraversable(edge.cost) ? 1 : 0) : edge.capacity;
		const double backward = unit_capacity ? (IsTraversable(edge.reverse_cost) ? 1 : 0) : edge.reverse_capacity;
		uint64_t from = 0;
		uint64_t to = 0;
		problem.index.Find(edge.source, from);
		problem.index.Find(edge.target, to);

		if (forward > 0) {
			const double weight = use_cost && edge.cost >= 0 ? edge.cost : 0;
			problem.arcs.push_back(AddArc(problem.graph, from, to, forward, weight));
			problem.arc_edge_ids.push_back(edge.id);
			problem.arc_from.push_back(edge.source);
			problem.arc_to.push_back(edge.target);
		}
		if (backward > 0) {
			const double weight = use_cost && edge.reverse_cost >= 0 ? edge.reverse_cost : 0;
			problem.arcs.push_back(AddArc(problem.graph, to, from, backward, weight));
			problem.arc_edge_ids.push_back(edge.id);
			problem.arc_from.push_back(edge.target);
			problem.arc_to.push_back(edge.source);
		}
	}

	const double unbounded = (std::numeric_limits<double>::max)() / 4;
	bool any_source = false;
	for (size_t i = 0; i < sources.size(); i++) {
		uint64_t v = 0;
		if (!problem.index.Find(sources[i], v)) {
			continue;
		}
		AddArc(problem.graph, problem.super_source, v, unbounded, 0);
		any_source = true;
	}
	bool any_sink = false;
	for (size_t i = 0; i < sinks.size(); i++) {
		uint64_t v = 0;
		if (!problem.index.Find(sinks[i], v)) {
			continue;
		}
		// A vertex that is both a source and a sink would short-circuit.
		if (std::find(sources.begin(), sources.end(), sinks[i]) != sources.end()) {
			continue;
		}
		AddArc(problem.graph, v, problem.super_sink, unbounded, 0);
		any_sink = true;
	}
	problem.solvable = any_source && any_sink;
	return problem;
}

double Solve(FlowProblem &problem, FlowAlgorithm algorithm) {
	if (!problem.solvable) {
		return 0;
	}
	switch (algorithm) {
	case FlowAlgorithm::EdmondsKarp:
		return boost::edmonds_karp_max_flow(problem.graph, problem.super_source, problem.super_sink);
	case FlowAlgorithm::BoykovKolmogorov: {
		// The short form of this one wants predecessor, colour and distance as
		// interior vertex properties; supplying them externally keeps the graph
		// type shared with the other algorithms.
		const size_t n = boost::num_vertices(problem.graph);
		std::vector<FlowEdge> predecessor(n);
		std::vector<boost::default_color_type> color(n);
		std::vector<long> distance(n);
		return boost::boykov_kolmogorov_max_flow(
		    problem.graph, boost::get(boost::edge_capacity, problem.graph),
		    boost::get(boost::edge_residual_capacity, problem.graph), boost::get(boost::edge_reverse, problem.graph),
		    &predecessor[0], &color[0], &distance[0], boost::get(boost::vertex_index, problem.graph),
		    problem.super_source, problem.super_sink);
	}
	case FlowAlgorithm::MinCost:
		boost::successive_shortest_path_nonnegative_weights(problem.graph, problem.super_source, problem.super_sink);
		return 0;
	case FlowAlgorithm::PushRelabel:
	default:
		return boost::push_relabel_max_flow(problem.graph, problem.super_source, problem.super_sink);
	}
}

//! Flow actually pushed along one arc, ignoring the bookkeeping that residuals
//! leave behind on reverse arcs.
double ArcFlow(const FlowGraph &graph, FlowEdge arc) {
	const double capacity = boost::get(boost::edge_capacity, graph, arc);
	const double residual = boost::get(boost::edge_residual_capacity, graph, arc);
	return capacity - residual;
}

} // namespace

std::vector<FlowRow> MaxFlow(const std::vector<FlowEdgeRow> &edges, const std::vector<int64_t> &sources,
                             const std::vector<int64_t> &sinks, FlowAlgorithm algorithm) {
	const bool min_cost = algorithm == FlowAlgorithm::MinCost;
	FlowProblem problem = BuildFlowProblem(edges, sources, sinks, min_cost, false);
	Solve(problem, algorithm);

	std::vector<FlowRow> rows;
	double agg_cost = 0;
	for (size_t i = 0; i < problem.arcs.size(); i++) {
		const double flow = ArcFlow(problem.graph, problem.arcs[i]);
		if (flow <= 0) {
			continue;
		}
		FlowRow row {};
		row.edge = problem.arc_edge_ids[i];
		row.start_vid = problem.arc_from[i];
		row.end_vid = problem.arc_to[i];
		row.flow = flow;
		row.residual_capacity = boost::get(boost::edge_residual_capacity, problem.graph, problem.arcs[i]);
		row.cost = flow * boost::get(boost::edge_weight, problem.graph, problem.arcs[i]);
		agg_cost += row.cost;
		row.agg_cost = agg_cost;
		rows.push_back(row);
	}
	return rows;
}

double MaxFlowValue(const std::vector<FlowEdgeRow> &edges, const std::vector<int64_t> &sources,
                    const std::vector<int64_t> &sinks, FlowAlgorithm algorithm) {
	const bool min_cost = algorithm == FlowAlgorithm::MinCost;
	FlowProblem problem = BuildFlowProblem(edges, sources, sinks, min_cost, false);
	const double value = Solve(problem, algorithm);
	if (!min_cost) {
		return value;
	}
	// Summing the reportable arcs directly, rather than calling find_flow_cost,
	// which also walks the negative-weight residual partners and cancels most
	// of the total away.
	double total = 0;
	for (size_t i = 0; i < problem.arcs.size(); i++) {
		const double flow = ArcFlow(problem.graph, problem.arcs[i]);
		if (flow > 0) {
			total += flow * boost::get(boost::edge_weight, problem.graph, problem.arcs[i]);
		}
	}
	return total;
}

std::vector<PathRow> EdgeDisjointPaths(const std::vector<FlowEdgeRow> &edges, const std::vector<int64_t> &starts,
                                       const std::vector<int64_t> &ends, bool directed) {
	std::vector<PathRow> rows;
	// Each (start, end) pair is its own flow problem, so the paths reported
	// belong to a single pair rather than to the union of all of them.
	for (size_t s = 0; s < starts.size(); s++) {
		for (size_t e = 0; e < ends.size(); e++) {
			if (starts[s] == ends[e]) {
				continue;
			}
			std::vector<FlowEdgeRow> prepared(edges);
			if (!directed) {
				// Undirected: an edge usable either way is usable both ways.
				for (size_t i = 0; i < prepared.size(); i++) {
					if (IsTraversable(prepared[i].cost) || IsTraversable(prepared[i].reverse_cost)) {
						const double cost =
						    IsTraversable(prepared[i].cost) ? prepared[i].cost : prepared[i].reverse_cost;
						prepared[i].cost = cost;
						prepared[i].reverse_cost = cost;
					}
				}
			}

			FlowProblem problem = BuildFlowProblem(prepared, std::vector<int64_t>(1, starts[s]),
			                                       std::vector<int64_t>(1, ends[e]), false, true);
			Solve(problem, FlowAlgorithm::PushRelabel);
			if (!problem.solvable) {
				continue;
			}

			// Decompose the unit flow back into paths: repeatedly walk from the
			// start to the end along arcs still carrying flow, consuming them.
			std::vector<double> remaining(problem.arcs.size());
			for (size_t i = 0; i < problem.arcs.size(); i++) {
				remaining[i] = ArcFlow(problem.graph, problem.arcs[i]);
			}

			// A maximum flow may settle with flow circulating around a cycle.
			// It does not change the value, but decomposing it would produce a
			// path that doubles back on itself, so cancel opposing flow between
			// the same pair of vertices first.
			for (size_t i = 0; i < problem.arcs.size(); i++) {
				if (remaining[i] <= 0) {
					continue;
				}
				for (size_t j = 0; j < problem.arcs.size(); j++) {
					if (remaining[j] <= 0 || problem.arc_from[j] != problem.arc_to[i] ||
					    problem.arc_to[j] != problem.arc_from[i]) {
						continue;
					}
					const double cancelled = (std::min)(remaining[i], remaining[j]);
					remaining[i] -= cancelled;
					remaining[j] -= cancelled;
					if (remaining[i] <= 0) {
						break;
					}
				}
			}

			std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> outgoing;
			for (size_t i = 0; i < problem.arcs.size(); i++) {
				if (remaining[i] > 0) {
					outgoing[std::make_pair(problem.arc_from[i], problem.arc_to[i])].push_back(i);
				}
			}

			int64_t path_id = 0;
			while (true) {
				std::vector<size_t> used;
				std::vector<int64_t> nodes(1, starts[s]);
				std::set<int64_t> visited;
				visited.insert(starts[s]);
				int64_t at = starts[s];
				bool advanced = true;
				while (at != ends[e] && advanced) {
					advanced = false;
					for (auto entry = outgoing.begin(); entry != outgoing.end(); ++entry) {
						if (entry->first.first != at) {
							continue;
						}
						// Never step onto a vertex this path already used.
						if (entry->first.second != ends[e] && visited.count(entry->first.second)) {
							continue;
						}
						for (size_t k = 0; k < entry->second.size(); k++) {
							const size_t arc = entry->second[k];
							if (remaining[arc] <= 0) {
								continue;
							}
							remaining[arc] -= 1;
							used.push_back(arc);
							at = entry->first.second;
							visited.insert(at);
							nodes.push_back(at);
							advanced = true;
							break;
						}
						if (advanced) {
							break;
						}
					}
				}
				if (at != ends[e]) {
					break;
				}
				path_id++;
				double agg_cost = 0;
				for (size_t i = 0; i < nodes.size(); i++) {
					PathRow row {};
					row.path_seq = static_cast<int64_t>(i) + 1;
					row.start_vid = starts[s];
					row.end_vid = ends[e];
					row.node = nodes[i];
					row.agg_cost = agg_cost;
					if (i < used.size()) {
						row.edge = problem.arc_edge_ids[used[i]];
						row.cost = 1;
						agg_cost += 1;
					} else {
						row.edge = -1;
						row.cost = 0;
					}
					rows.push_back(row);
				}
			}
		}
	}
	return rows;
}

std::vector<IdentifierRow> MaxCardinalityMatch(const std::vector<FlowEdgeRow> &edges) {
	typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS> MatchGraph;

	std::vector<FlowEdgeRow> ordered(edges);
	std::stable_sort(ordered.begin(), ordered.end(),
	                 [](const FlowEdgeRow &a, const FlowEdgeRow &b) { return a.id < b.id; });

	VertexIndex index;
	for (size_t i = 0; i < ordered.size(); i++) {
		index.GetOrCreate(ordered[i].source);
		index.GetOrCreate(ordered[i].target);
	}

	MatchGraph graph(index.Size());
	// Matching is structural, so remember which input edge joined each pair.
	std::map<std::pair<uint64_t, uint64_t>, int64_t> edge_of;
	for (size_t i = 0; i < ordered.size(); i++) {
		if (!IsTraversable(ordered[i].cost) && !IsTraversable(ordered[i].reverse_cost) && ordered[i].capacity < 0 &&
		    ordered[i].reverse_capacity < 0) {
			continue;
		}
		uint64_t from = 0;
		uint64_t to = 0;
		index.Find(ordered[i].source, from);
		index.Find(ordered[i].target, to);
		boost::add_edge(from, to, graph);
		const std::pair<uint64_t, uint64_t> key = from < to ? std::make_pair(from, to) : std::make_pair(to, from);
		if (!edge_of.count(key)) {
			edge_of[key] = ordered[i].id;
		}
	}

	std::vector<boost::graph_traits<MatchGraph>::vertex_descriptor> mate(boost::num_vertices(graph));
	boost::edmonds_maximum_cardinality_matching(graph, &mate[0]);

	std::vector<IdentifierRow> rows;
	const auto null_vertex = boost::graph_traits<MatchGraph>::null_vertex();
	for (uint64_t v = 0; v < index.Size(); v++) {
		const auto partner = mate[v];
		// Report each matched pair once, from its lower-numbered end.
		if (partner == null_vertex || static_cast<uint64_t>(partner) < v) {
			continue;
		}
		auto entry = edge_of.find(std::make_pair(v, static_cast<uint64_t>(partner)));
		if (entry != edge_of.end()) {
			rows.push_back(IdentifierRow {entry->second});
		}
	}
	std::sort(rows.begin(), rows.end(), [](const IdentifierRow &a, const IdentifierRow &b) { return a.id < b.id; });
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

struct FlowBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::vector<int64_t> sources;
	std::vector<int64_t> sinks;
	bool directed = true;
};

template <typename Row>
struct FlowGlobalState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

std::vector<int64_t> FlowVertexIds(const Value &value, const char *what) {
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

void ReadFlowArguments(TableFunctionBindInput &input, FlowBindData &bind_data) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	bind_data.edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data.sources = FlowVertexIds(input.inputs[1], "the source vertex");
	bind_data.sinks = FlowVertexIds(input.inputs[2], "the sink vertex");
	if (input.inputs.size() > 3 && !input.inputs[3].IsNull()) {
		bind_data.directed = input.inputs[3].GetValue<bool>();
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

//! (seq, edge, start_vid, end_vid, flow, residual_capacity) for the three
//! max-flow algorithms, with two more columns for the min-cost variant.
template <bool WithCost>
duckdb::unique_ptr<FunctionData> FlowBind(ClientContext &, TableFunctionBindInput &input,
                                          duckdb::vector<LogicalType> &return_types,
                                          duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<FlowBindData>();
	ReadFlowArguments(input, *bind_data);
	if (WithCost) {
		names = {"seq", "edge", "source", "target", "flow", "residual_capacity", "cost", "agg_cost"};
		return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
		                LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE};
	} else {
		names = {"seq", "edge", "start_vid", "end_vid", "flow", "residual_capacity"};
		return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
		                LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	}
	return std::move(bind_data);
}

template <FlowAlgorithm Algorithm, bool RequireCost>
duckdb::unique_ptr<GlobalTableFunctionState> FlowInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<FlowBindData>();
	auto state = duckdb::make_uniq<FlowGlobalState<FlowRow>>();
	auto edges = LoadFlowEdges(context, bind_data.edges_sql, true, RequireCost);
	state->rows = MaxFlow(edges, bind_data.sources, bind_data.sinks, Algorithm);
	return std::move(state);
}

template <bool WithCost>
void FlowScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<FlowGlobalState<FlowRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.edge));
		output.SetValue(2, i, Value::BIGINT(row.start_vid));
		output.SetValue(3, i, Value::BIGINT(row.end_vid));
		output.SetValue(4, i, Value::DOUBLE(row.flow));
		output.SetValue(5, i, Value::DOUBLE(row.residual_capacity));
		if (WithCost) {
			output.SetValue(6, i, Value::DOUBLE(row.cost));
			output.SetValue(7, i, Value::DOUBLE(row.agg_cost));
		}
	}
	state.offset += count;
}

//! A single total: max_flow and max_flow_min_cost_cost.
template <bool IsCost>
duckdb::unique_ptr<FunctionData> TotalBind(ClientContext &, TableFunctionBindInput &input,
                                           duckdb::vector<LogicalType> &return_types,
                                           duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<FlowBindData>();
	ReadFlowArguments(input, *bind_data);
	names = {IsCost ? "cost" : "flow"};
	return_types = {LogicalType::DOUBLE};
	return std::move(bind_data);
}

template <FlowAlgorithm Algorithm, bool RequireCost>
duckdb::unique_ptr<GlobalTableFunctionState> TotalInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<FlowBindData>();
	auto state = duckdb::make_uniq<FlowGlobalState<Value>>();
	auto edges = LoadFlowEdges(context, bind_data.edges_sql, true, RequireCost);
	state->rows.push_back(Value::DOUBLE(MaxFlowValue(edges, bind_data.sources, bind_data.sinks, Algorithm)));
	return std::move(state);
}

void TotalScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<FlowGlobalState<Value>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		output.SetValue(0, i, state.rows[state.offset + i]);
	}
	state.offset += count;
}

//! Edge-disjoint paths use pgRouting's dijkstra column shape.
duckdb::unique_ptr<FunctionData> DisjointBind(ClientContext &, TableFunctionBindInput &input,
                                              duckdb::vector<LogicalType> &return_types,
                                              duckdb::vector<std::string> &names) {
	auto bind_data = duckdb::make_uniq<FlowBindData>();
	ReadFlowArguments(input, *bind_data);
	names = {"seq", "path_id", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> DisjointInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<FlowBindData>();
	auto state = duckdb::make_uniq<FlowGlobalState<PathRow>>();
	auto edges = LoadFlowEdges(context, bind_data.edges_sql, false, false);
	state->rows = EdgeDisjointPaths(edges, bind_data.sources, bind_data.sinks, bind_data.directed);
	return std::move(state);
}

void DisjointScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<FlowGlobalState<PathRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	// path_id restarts for each (start, end) pair, so recover it from the
	// path_seq resets rather than storing it on every row.
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		int64_t path_id = 0;
		for (idx_t j = 0; j <= state.offset + i; j++) {
			if (state.rows[j].path_seq == 1) {
				path_id++;
			}
		}
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(path_id));
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

//! A bare `edge` column: max_cardinality_match.
duckdb::unique_ptr<FunctionData> MatchBind(ClientContext &, TableFunctionBindInput &input,
                                           duckdb::vector<LogicalType> &return_types,
                                           duckdb::vector<std::string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the edges query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<FlowBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	names = {"edge"};
	return_types = {LogicalType::BIGINT};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> MatchInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<FlowBindData>();
	auto state = duckdb::make_uniq<FlowGlobalState<IdentifierRow>>();
	state->rows = MaxCardinalityMatch(LoadFlowEdges(context, bind_data.edges_sql, false, false));
	return std::move(state);
}

void MatchScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<FlowGlobalState<IdentifierRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		output.SetValue(0, i, Value::BIGINT(state.rows[state.offset + i].id));
	}
	state.offset += count;
}

//! All the flow functions take (edges_sql, source, sink) with either spelling
//! of the vertex arguments.
TableFunctionSet FlowSet(const char *name, duckdb::table_function_t scan, duckdb::table_function_bind_t bind,
                         duckdb::table_function_init_global_t init, bool accepts_directed) {
	TableFunctionSet set(name);
	const LogicalType list = LogicalType::LIST(LogicalType::BIGINT);
	for (size_t source_is_list = 0; source_is_list < 2; source_is_list++) {
		for (size_t sink_is_list = 0; sink_is_list < 2; sink_is_list++) {
			const size_t variants = accepts_directed ? 2u : 1u;
			for (size_t with_flag = 0; with_flag < variants; with_flag++) {
				duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR,
				                                       source_is_list ? list : LogicalType::BIGINT,
				                                       sink_is_list ? list : LogicalType::BIGINT};
				if (with_flag) {
					arguments.push_back(LogicalType::BOOLEAN);
				}
				TableFunction function(arguments, scan, bind, init);
				if (accepts_directed) {
					function.named_parameters["directed"] = LogicalType::BOOLEAN;
				}
				set.AddFunction(function);
			}
		}
	}
	return set;
}

} // namespace

TableFunctionSet GetMaxFlowFunction() {
	return FlowSet("duckrouting_max_flow", TotalScan, TotalBind<false>, TotalInit<FlowAlgorithm::PushRelabel, false>,
	               false);
}

TableFunctionSet GetPushRelabelFunction() {
	return FlowSet("duckrouting_push_relabel", FlowScan<false>, FlowBind<false>,
	               FlowInit<FlowAlgorithm::PushRelabel, false>, false);
}

TableFunctionSet GetEdmondsKarpFunction() {
	return FlowSet("duckrouting_edmonds_karp", FlowScan<false>, FlowBind<false>,
	               FlowInit<FlowAlgorithm::EdmondsKarp, false>, false);
}

TableFunctionSet GetBoykovKolmogorovFunction() {
	return FlowSet("duckrouting_boykov_kolmogorov", FlowScan<false>, FlowBind<false>,
	               FlowInit<FlowAlgorithm::BoykovKolmogorov, false>, false);
}

TableFunctionSet GetMaxFlowMinCostFunction() {
	return FlowSet("duckrouting_max_flow_min_cost", FlowScan<true>, FlowBind<true>,
	               FlowInit<FlowAlgorithm::MinCost, true>, false);
}

TableFunctionSet GetMaxFlowMinCostCostFunction() {
	return FlowSet("duckrouting_max_flow_min_cost_cost", TotalScan, TotalBind<true>,
	               TotalInit<FlowAlgorithm::MinCost, true>, false);
}

TableFunctionSet GetEdgeDisjointPathsFunction() {
	return FlowSet("duckrouting_edge_disjoint_paths", DisjointScan, DisjointBind, DisjointInit, true);
}

TableFunctionSet GetMaxCardinalityMatchFunction() {
	TableFunctionSet set("duckrouting_max_cardinality_match");
	set.AddFunction(TableFunction({LogicalType::VARCHAR}, MatchScan, MatchBind, MatchInit));
	return set;
}

} // namespace duckrouting
