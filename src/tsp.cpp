#include "duckrouting/graph_functions.hpp"
#include "duckrouting/compat.hpp"

#include "duckrouting/graph.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <boost/graph/metric_tsp_approx.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace duckrouting {

namespace {

//! metric_tsp_approx wants a complete weighted graph.
typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS, boost::no_property,
                              boost::property<boost::edge_weight_t, double>>
    TourGraph;

//! Turns a tour of vertex descriptors into pgRouting's rows. `cost` is the cost
//! of reaching each stop, so the first is 0 and the last closes the loop.
std::vector<TourRow> ToRows(const std::vector<uint64_t> &tour, const VertexIndex &index,
                            const std::vector<std::vector<double>> &distance) {
	std::vector<TourRow> rows;
	double agg_cost = 0;
	for (size_t i = 0; i < tour.size(); i++) {
		TourRow row {};
		row.node = index.IdOf(tour[i]);
		row.cost = i == 0 ? 0 : distance[tour[i - 1]][tour[i]];
		agg_cost += row.cost;
		row.agg_cost = agg_cost;
		rows.push_back(row);
	}
	return rows;
}

//! Rotates a closed tour so it begins at `start`, and orients it so `end` is
//! the last stop before closing. A cycle can be entered anywhere and walked in
//! either direction, so both are free to choose.
void Orient(std::vector<uint64_t> &tour, bool has_start, uint64_t start, bool has_end, uint64_t end) {
	if (tour.size() < 2) {
		return;
	}
	// The tour comes back with its first vertex repeated at the end; work with
	// the open cycle and close it again afterwards.
	std::vector<uint64_t> cycle(tour.begin(), tour.end() - 1);
	if (cycle.empty()) {
		return;
	}

	if (has_start) {
		auto at = std::find(cycle.begin(), cycle.end(), start);
		if (at != cycle.end()) {
			std::rotate(cycle.begin(), at, cycle.end());
		}
	}
	if (has_end && cycle.size() > 1 && cycle.back() != end) {
		// Walking the cycle backwards keeps the same stops and the same total.
		std::reverse(cycle.begin() + 1, cycle.end());
	}

	tour = cycle;
	tour.push_back(cycle.front());
}

std::vector<TourRow> SolveTour(const std::vector<std::vector<double>> &distance, const VertexIndex &index,
                               bool has_start, uint64_t start, bool has_end, uint64_t end) {
	const size_t n = index.Size();
	if (n == 0) {
		return {};
	}
	if (n == 1) {
		return std::vector<TourRow>(1, TourRow {index.IdOf(0), 0, 0});
	}

	TourGraph graph(n);
	for (size_t i = 0; i < n; i++) {
		for (size_t j = i + 1; j < n; j++) {
			boost::add_edge(i, j, distance[i][j], graph);
		}
	}

	std::vector<boost::graph_traits<TourGraph>::vertex_descriptor> tour;
	if (has_start) {
		boost::metric_tsp_approx_tour_from_vertex(graph, static_cast<TourGraph::vertex_descriptor>(start),
		                                          std::back_inserter(tour));
	} else {
		boost::metric_tsp_approx_tour(graph, std::back_inserter(tour));
	}

	std::vector<uint64_t> ordered;
	for (size_t i = 0; i < tour.size(); i++) {
		ordered.push_back(static_cast<uint64_t>(tour[i]));
	}
	Orient(ordered, has_start, start, has_end, end);
	return ToRows(ordered, index, distance);
}

} // namespace

std::vector<TourRow> Tsp(const std::vector<MatrixCell> &matrix, int64_t start_id, int64_t end_id) {
	VertexIndex index;
	for (size_t i = 0; i < matrix.size(); i++) {
		index.GetOrCreate(matrix[i].start_vid);
		index.GetOrCreate(matrix[i].end_vid);
	}
	const size_t n = index.Size();
	if (n == 0) {
		return {};
	}

	// Missing cells stay at infinity, which surfaces as an unusable tour rather
	// than a silently wrong one.
	std::vector<std::vector<double>> distance(n, std::vector<double>(n, std::numeric_limits<double>::infinity()));
	for (size_t i = 0; i < n; i++) {
		distance[i][i] = 0;
	}
	for (size_t i = 0; i < matrix.size(); i++) {
		uint64_t from = 0;
		uint64_t to = 0;
		index.Find(matrix[i].start_vid, from);
		index.Find(matrix[i].end_vid, to);
		distance[from][to] = matrix[i].agg_cost;
	}

	// metric_tsp_approx needs a symmetric metric; pgRouting's own cost matrices
	// are symmetric when built undirected, and taking the cheaper direction is
	// the sensible reading when they are not.
	for (size_t i = 0; i < n; i++) {
		for (size_t j = i + 1; j < n; j++) {
			const double best = (std::min)(distance[i][j], distance[j][i]);
			distance[i][j] = best;
			distance[j][i] = best;
		}
	}

	uint64_t start = 0;
	uint64_t end = 0;
	const bool has_start = start_id != 0 && index.Find(start_id, start);
	const bool has_end = end_id != 0 && index.Find(end_id, end);
	return SolveTour(distance, index, has_start, start, has_end, end);
}

std::vector<TourRow> TspEuclidean(const std::vector<PlacedPoint> &points, int64_t start_id, int64_t end_id) {
	VertexIndex index;
	std::vector<PlacedPoint> placed;
	for (size_t i = 0; i < points.size(); i++) {
		const uint64_t descriptor = index.GetOrCreate(points[i].id);
		if (descriptor == placed.size()) {
			placed.push_back(points[i]);
		}
	}
	const size_t n = index.Size();
	if (n == 0) {
		return {};
	}

	std::vector<std::vector<double>> distance(n, std::vector<double>(n, 0));
	for (size_t i = 0; i < n; i++) {
		for (size_t j = 0; j < n; j++) {
			const double dx = placed[i].x - placed[j].x;
			const double dy = placed[i].y - placed[j].y;
			distance[i][j] = std::sqrt(dx * dx + dy * dy);
		}
	}

	uint64_t start = 0;
	uint64_t end = 0;
	const bool has_start = start_id != 0 && index.Find(start_id, start);
	const bool has_end = end_id != 0 && index.Find(end_id, end);
	return SolveTour(distance, index, has_start, start, has_end, end);
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

struct TspBindData : public duckdb::TableFunctionData {
	std::string query;
	int64_t start_id = 0;
	int64_t end_id = 0;
};

struct TspGlobalState : public GlobalTableFunctionState {
	std::vector<TourRow> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

duckdb::unique_ptr<FunctionData> TspBind(ClientContext &, TableFunctionBindInput &input,
                                         duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("duckrouting: the query must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<TspBindData>();
	bind_data->query = input.inputs[0].GetValue<std::string>();
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		bind_data->start_id = input.inputs[1].GetValue<int64_t>();
	}
	if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
		bind_data->end_id = input.inputs[2].GetValue<int64_t>();
	}
	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw BinderException("duckrouting: '%s' must not be NULL", parameter.first.c_str());
		}
		if (NameMatches(parameter.first, "start_id")) {
			bind_data->start_id = parameter.second.GetValue<int64_t>();
		} else if (NameMatches(parameter.first, "end_id")) {
			bind_data->end_id = parameter.second.GetValue<int64_t>();
		}
	}

	names = {"seq", "node", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

template <bool Euclidean>
duckdb::unique_ptr<GlobalTableFunctionState> TspInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TspBindData>();
	auto state = duckdb::make_uniq<TspGlobalState>();
	state->rows = Euclidean ? TspEuclidean(LoadPoints(context, bind_data.query), bind_data.start_id, bind_data.end_id)
	                        : Tsp(LoadCostMatrix(context, bind_data.query), bind_data.start_id, bind_data.end_id);
	return std::move(state);
}

void TspScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<TspGlobalState>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.node));
		output.SetValue(2, i, Value::DOUBLE(row.cost));
		output.SetValue(3, i, Value::DOUBLE(row.agg_cost));
	}
	state.offset += count;
}

template <bool Euclidean>
TableFunctionSet TspSet(const char *name) {
	TableFunctionSet set(name);
	// pgRouting spells start_id and end_id positionally as well as by name.
	for (size_t trailing = 0; trailing < 3; trailing++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR};
		for (size_t i = 0; i < trailing; i++) {
			arguments.push_back(LogicalType::BIGINT);
		}
		TableFunction function(arguments, TspScan, TspBind, TspInit<Euclidean>);
		function.named_parameters["start_id"] = LogicalType::BIGINT;
		function.named_parameters["end_id"] = LogicalType::BIGINT;
		set.AddFunction(function);
	}
	return set;
}

} // namespace

TableFunctionSet GetTspFunction() {
	return TspSet<false>("duckrouting_tsp");
}

TableFunctionSet GetTspEuclideanFunction() {
	return TspSet<true>("duckrouting_tsp_euclidean");
}

} // namespace duckrouting
