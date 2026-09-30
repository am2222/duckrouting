#include "duckrouting/compat.hpp"
#include "duckrouting/graph.hpp"
#include "duckrouting/graph_functions.hpp"
#include "duckrouting/yen.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

//! Map matching after Yang & Gidofalvi, "Fast map matching, an algorithm
//! integrating hidden Markov model with precomputation" (2018), as implemented
//! in https://github.com/cyang-kth/fmm. Each GPS fix has a handful of
//! candidate edges; the emission probability of a candidate is a Gaussian on
//! the fix's distance to it, and the transition probability between two
//! candidates of consecutive fixes is the straight-line distance between the
//! fixes divided by the shortest-path distance between the candidates. Viterbi
//! picks the likeliest candidate sequence.
//!
//! FMM measures along edges in metres of geometry. Here a candidate is a point
//! partway along an edge, exactly as the withPoints family understands one, so
//! the partial edges cost `fraction * cost` and the shortest paths between
//! candidates are ordinary shortest paths over the split graph. That is what
//! lets the whole thing run on an edges query with no geometry at all.
//!
//! FMM's two variants differ only in how those shortest paths are found: FMM
//! proper looks them up in a precomputed table, STMATCH runs a bounded search
//! per candidate. This is the STMATCH strategy.

namespace duckrouting {

namespace {

using duckdb::BinderException;
using duckdb::ClientContext;
using duckdb::DataChunk;
using duckdb::FunctionData;
using duckdb::GlobalTableFunctionState;
using duckdb::idx_t;
using duckdb::InvalidInputException;
using duckdb::LogicalType;
using duckdb::TableFunction;
using duckdb::TableFunctionBindInput;
using duckdb::TableFunctionInitInput;
using duckdb::TableFunctionInput;
using duckdb::TableFunctionSet;
using duckdb::Value;

const double kInfinity = std::numeric_limits<double>::infinity();

// --- shortest paths ----------------------------------------------------------

//! Shortest-path distance from `source` to each of `targets`, giving up once
//! every target is settled or the frontier passes `bound`. This is STMATCH's
//! shortest_path_upperbound. Targets that were not reached come back infinite.
std::vector<double> BoundedDistances(const Adjacency &adjacency, uint64_t source, const std::vector<uint64_t> &targets,
                                     double bound) {
	std::vector<double> result(targets.size(), kInfinity);
	std::unordered_map<uint64_t, std::vector<size_t>> wanted;
	for (size_t i = 0; i < targets.size(); i++) {
		wanted[targets[i]].push_back(i);
	}

	typedef std::pair<double, uint64_t> Entry;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
	std::unordered_map<uint64_t, double> distance;
	distance[source] = 0;
	queue.push(Entry(0, source));

	while (!queue.empty() && !wanted.empty()) {
		const Entry top = queue.top();
		queue.pop();
		if (top.first > bound) {
			break;
		}
		// A stale entry: this vertex was settled by a cheaper one already.
		if (top.first > distance[top.second]) {
			continue;
		}
		auto hit = wanted.find(top.second);
		if (hit != wanted.end()) {
			for (size_t i = 0; i < hit->second.size(); i++) {
				result[hit->second[i]] = top.first;
			}
			wanted.erase(hit);
		}
		const std::vector<Arc> &arcs = adjacency[top.second];
		for (size_t i = 0; i < arcs.size(); i++) {
			const double next = top.first + arcs[i].cost;
			auto known = distance.find(arcs[i].to);
			if (known == distance.end() || next < known->second) {
				distance[arcs[i].to] = next;
				queue.push(Entry(next, arcs[i].to));
			}
		}
	}
	return result;
}

// --- one trajectory ----------------------------------------------------------

//! The candidates of one GPS fix, as a range over the trajectory's candidate
//! list, plus the fix itself.
struct Layer {
	int64_t pid;
	size_t begin;
	size_t end;
	double x;
	double y;
};

//! FMM's TGNode: what Viterbi knows about one candidate.
struct Node {
	double ep = 0;
	double tp = 0;
	double sp_dist = 0;
	double cumulative = -std::numeric_limits<double>::infinity();
	//! Index of the best predecessor, or -1 in the first layer.
	int64_t previous = -1;
};

//! Groups a trajectory's candidates, already sorted by pid, into layers.
std::vector<Layer> MakeLayers(const std::vector<Candidate> &candidates) {
	std::vector<Layer> layers;
	for (size_t i = 0; i < candidates.size(); i++) {
		if (layers.empty() || layers.back().pid != candidates[i].pid) {
			layers.push_back(Layer {candidates[i].pid, i, i, candidates[i].x, candidates[i].y});
		}
		layers.back().end = i + 1;
	}
	return layers;
}

//! Every candidate becomes a vertex numbered -(its position + 1), so the
//! same edge can carry several candidates of the same fix without any two
//! colliding, whatever the user's pids look like.
int64_t CandidateVertex(size_t position) {
	return -static_cast<int64_t>(position + 1);
}

//! Turns candidates into the withPoints shape. A candidate on an edge the
//! graph does not have is a mistake in the candidates query, not a candidate
//! that happens to be unreachable.
std::vector<PointOnEdge> CandidatePoints(const std::vector<EdgeRow> &edges, const std::vector<Candidate> &candidates,
                                         bool by_position) {
	std::set<int64_t> known;
	for (size_t i = 0; i < edges.size(); i++) {
		known.insert(edges[i].id);
	}
	std::vector<PointOnEdge> points;
	points.reserve(candidates.size());
	for (size_t i = 0; i < candidates.size(); i++) {
		if (!known.count(candidates[i].edge_id)) {
			throw InvalidInputException("duckrouting: candidate for pid %lld refers to edge %lld, which the edges "
			                            "query does not contain",
			                            static_cast<long long>(candidates[i].pid),
			                            static_cast<long long>(candidates[i].edge_id));
		}
		const int64_t pid = by_position ? -CandidateVertex(i) : candidates[i].pid;
		points.push_back(PointOnEdge {pid, candidates[i].edge_id, candidates[i].fraction, 'b'});
	}
	return points;
}

//! FMM treats a small step backwards along the same edge as GPS jitter rather
//! than a trip round the block: within `reverse_tolerance` of the edge's
//! length it costs nothing.
bool WithinReverseTolerance(const Candidate &from, const Candidate &to, double reverse_tolerance) {
	return from.edge_id == to.edge_id && from.fraction > to.fraction && from.fraction - to.fraction < reverse_tolerance;
}

//! Viterbi over one trajectory. Returns the chosen candidate per layer, in
//! order, or nothing when some fix cannot be reached from the previous one --
//! FMM reports such a trajectory as unmatched rather than matching a part.
std::vector<size_t> MatchTrajectory(const std::vector<EdgeRow> &edges, const std::vector<Candidate> &candidates,
                                    const MapMatchOptions &options, std::vector<Node> &nodes) {
	const std::vector<Layer> layers = MakeLayers(candidates);
	nodes.assign(candidates.size(), Node());
	if (layers.empty()) {
		return std::vector<size_t>();
	}

	VertexIndex index;
	const Adjacency adjacency = BuildAdjacency(SplitEdgesAtPoints(edges, CandidatePoints(edges, candidates, true), 'b'),
	                                           index, options.directed);
	std::vector<uint64_t> vertex(candidates.size());
	for (size_t i = 0; i < candidates.size(); i++) {
		index.Find(CandidateVertex(i), vertex[i]);
	}

	for (size_t i = 0; i < candidates.size(); i++) {
		const double a = candidates[i].distance / options.gps_error;
		nodes[i].ep = std::exp(-0.5 * a * a);
	}
	for (size_t i = layers[0].begin; i < layers[0].end; i++) {
		nodes[i].cumulative = std::log(nodes[i].ep);
	}

	for (size_t level = 0; level + 1 < layers.size(); level++) {
		const Layer &from = layers[level];
		const Layer &to = layers[level + 1];
		const double euclidean = std::hypot(to.x - from.x, to.y - from.y);

		std::vector<uint64_t> targets;
		for (size_t b = to.begin; b < to.end; b++) {
			targets.push_back(vertex[b]);
		}

		bool connected = false;
		for (size_t a = from.begin; a < from.end; a++) {
			if (nodes[a].cumulative == -kInfinity) {
				continue;
			}
			const std::vector<double> distances = BoundedDistances(adjacency, vertex[a], targets, options.delta);
			for (size_t b = to.begin; b < to.end; b++) {
				double sp_dist = distances[b - to.begin];
				if (WithinReverseTolerance(candidates[a], candidates[b], options.reverse_tolerance)) {
					sp_dist = 0;
				}
				const double tp = euclidean >= sp_dist ? 1.0 : euclidean / sp_dist;
				const double candidate = nodes[a].cumulative + std::log(tp) + std::log(nodes[b].ep);
				// `>=` so that a later candidate wins a tie, as FMM's does.
				if (candidate >= nodes[b].cumulative) {
					nodes[b].cumulative = candidate;
					nodes[b].previous = static_cast<int64_t>(a);
					nodes[b].tp = tp;
					nodes[b].sp_dist = sp_dist;
					connected = connected || candidate > -kInfinity;
				}
			}
		}
		if (!connected) {
			return std::vector<size_t>();
		}
	}

	// The likeliest candidate of the last fix, then back along the predecessors.
	const Layer &last = layers.back();
	size_t best = last.begin;
	for (size_t b = last.begin; b < last.end; b++) {
		if (nodes[best].cumulative < nodes[b].cumulative) {
			best = b;
		}
	}
	if (nodes[best].cumulative == -kInfinity) {
		return std::vector<size_t>();
	}
	std::vector<size_t> chosen;
	for (int64_t at = static_cast<int64_t>(best); at >= 0; at = nodes[at].previous) {
		chosen.push_back(static_cast<size_t>(at));
	}
	std::reverse(chosen.begin(), chosen.end());
	return chosen;
}

//! The route through the network that visits the matched candidates in
//! order: FMM's cpath, in pgRouting's path shape. Here the vertices are the
//! user's pids, negated, since there is now exactly one candidate per fix.
std::vector<KspRow> BuildPath(const std::vector<EdgeRow> &edges, const std::vector<Candidate> &matched,
                              const MapMatchOptions &options, bool details) {
	std::vector<KspRow> rows;
	if (matched.empty()) {
		return rows;
	}

	VertexIndex index;
	const Adjacency adjacency =
	    BuildAdjacency(SplitEdgesAtPoints(edges, CandidatePoints(edges, matched, false), 'b'), index, options.directed);

	const int64_t start_vid = -matched.front().pid;
	const int64_t end_vid = -matched.back().pid;
	for (size_t i = 0; i + 1 < matched.size(); i++) {
		const Candidate &from = matched[i];
		const Candidate &to = matched[i + 1];
		if (WithinReverseTolerance(from, to, options.reverse_tolerance)) {
			// Viterbi charged nothing for this step, so the path stays put.
			rows.push_back(KspRow {0, 0, start_vid, end_vid, -from.pid, from.edge_id, 0, 0});
			continue;
		}
		uint64_t source = 0;
		uint64_t sink = 0;
		index.Find(-from.pid, source);
		index.Find(-to.pid, sink);
		SimplePath leg;
		if (!ConstrainedShortestPath(adjacency, source, sink, std::set<uint64_t>(), std::set<ArcKey>(), leg)) {
			return std::vector<KspRow>();
		}
		// The last step is the sink itself, which the next leg starts from.
		for (size_t s = 0; s + 1 < leg.steps.size(); s++) {
			rows.push_back(KspRow {0, 0, start_vid, end_vid, index.IdOf(leg.steps[s].vertex), leg.steps[s].edge,
			                       leg.steps[s].cost, 0});
		}
	}
	rows.push_back(KspRow {0, 0, start_vid, end_vid, end_vid, -1, 0, 0});

	if (!details) {
		// Hide the fixes in between: each one's segment is folded into the
		// row before it, so an edge appears once with its whole cost. The
		// same goes for any further piece of the edge just reported -- a fix
		// sitting exactly on an endpoint is joined to it by a free edge, and
		// that piece is not a second traversal.
		std::vector<KspRow> merged;
		for (size_t i = 0; i < rows.size(); i++) {
			const bool inner = i > 0 && i + 1 < rows.size();
			if (inner && (rows[i].node < 0 || rows[i].edge == merged.back().edge)) {
				merged.back().cost += rows[i].cost;
				continue;
			}
			merged.push_back(rows[i]);
		}
		rows.swap(merged);
	}

	double agg_cost = 0;
	for (size_t i = 0; i < rows.size(); i++) {
		rows[i].path_seq = static_cast<int64_t>(i) + 1;
		rows[i].agg_cost = agg_cost;
		agg_cost += rows[i].cost;
	}
	return rows;
}

//! Sorts into trajectories, and within each into fixes, with the candidates
//! of a fix closest first -- the order FMM's own search reports them in,
//! which is what its tie-breaking assumes.
std::vector<Candidate> Sorted(const std::vector<Candidate> &candidates) {
	std::vector<Candidate> sorted(candidates);
	std::stable_sort(sorted.begin(), sorted.end(), [](const Candidate &a, const Candidate &b) {
		if (a.traj_id != b.traj_id) {
			return a.traj_id < b.traj_id;
		}
		if (a.pid != b.pid) {
			return a.pid < b.pid;
		}
		if (a.distance != b.distance) {
			return a.distance < b.distance;
		}
		return a.edge_id < b.edge_id;
	});
	return sorted;
}

//! Calls `visit(trajectory_candidates)` once per trajectory.
template <typename Visit>
void ForEachTrajectory(const std::vector<Candidate> &sorted, Visit visit) {
	size_t begin = 0;
	while (begin < sorted.size()) {
		size_t end = begin;
		while (end < sorted.size() && sorted[end].traj_id == sorted[begin].traj_id) {
			end++;
		}
		visit(std::vector<Candidate>(sorted.begin() + begin, sorted.begin() + end));
		begin = end;
	}
}

} // namespace

std::vector<MatchedPointRow> MapMatch(const std::vector<EdgeRow> &edges, const std::vector<Candidate> &candidates,
                                      const MapMatchOptions &options) {
	std::vector<MatchedPointRow> rows;
	ForEachTrajectory(Sorted(candidates), [&](const std::vector<Candidate> &trajectory) {
		std::vector<Node> nodes;
		const std::vector<size_t> chosen = MatchTrajectory(edges, trajectory, options, nodes);
		for (size_t i = 0; i < chosen.size(); i++) {
			const Candidate &c = trajectory[chosen[i]];
			const Node &node = nodes[chosen[i]];
			rows.push_back(
			    MatchedPointRow {c.traj_id, c.pid, c.edge_id, c.fraction, c.distance, node.ep, node.tp, node.sp_dist});
		}
	});
	return rows;
}

std::vector<KspRow> MapMatchPath(const std::vector<EdgeRow> &edges, const std::vector<Candidate> &candidates,
                                 const MapMatchOptions &options, bool details) {
	std::vector<KspRow> rows;
	ForEachTrajectory(Sorted(candidates), [&](const std::vector<Candidate> &trajectory) {
		std::vector<Node> nodes;
		const std::vector<size_t> chosen = MatchTrajectory(edges, trajectory, options, nodes);
		std::vector<Candidate> matched;
		for (size_t i = 0; i < chosen.size(); i++) {
			matched.push_back(trajectory[chosen[i]]);
		}
		std::vector<KspRow> path = BuildPath(edges, matched, options, details);
		for (size_t i = 0; i < path.size(); i++) {
			path[i].path_id = trajectory.front().traj_id;
			rows.push_back(path[i]);
		}
	});
	return rows;
}

// --- table functions ---------------------------------------------------------

namespace {

struct MapMatchBindData : public duckdb::TableFunctionData {
	std::string edges_sql;
	std::string candidates_sql;
	MapMatchOptions options;
	bool details = false;
};

template <typename Row>
struct MapMatchState : public GlobalTableFunctionState {
	std::vector<Row> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

double PositiveDouble(const Value &value, const char *name) {
	if (value.IsNull()) {
		throw BinderException("duckrouting: '%s' must not be NULL", name);
	}
	const double result = value.GetValue<double>();
	if (!(result > 0)) {
		throw BinderException("duckrouting: '%s' must be greater than 0", name);
	}
	return result;
}

duckdb::unique_ptr<MapMatchBindData> ReadArguments(TableFunctionBindInput &input) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("duckrouting: the edges and candidates queries must not be NULL");
	}
	auto bind_data = duckdb::make_uniq<MapMatchBindData>();
	bind_data->edges_sql = input.inputs[0].GetValue<std::string>();
	bind_data->candidates_sql = input.inputs[1].GetValue<std::string>();
	if (input.inputs.size() > 2) {
		bind_data->options.gps_error = PositiveDouble(input.inputs[2], "gps_error");
	}
	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw BinderException("duckrouting: '%s' must not be NULL", NameText(parameter.first).c_str());
		}
		if (NameMatches(parameter.first, "gps_error")) {
			bind_data->options.gps_error = PositiveDouble(parameter.second, "gps_error");
		} else if (NameMatches(parameter.first, "delta")) {
			bind_data->options.delta = PositiveDouble(parameter.second, "delta");
		} else if (NameMatches(parameter.first, "reverse_tolerance")) {
			bind_data->options.reverse_tolerance = parameter.second.GetValue<double>();
			if (bind_data->options.reverse_tolerance < 0) {
				throw BinderException("duckrouting: 'reverse_tolerance' must not be negative");
			}
		} else if (NameMatches(parameter.first, "directed")) {
			bind_data->options.directed = parameter.second.GetValue<bool>();
		} else if (NameMatches(parameter.first, "details")) {
			bind_data->details = parameter.second.GetValue<bool>();
		}
	}
	return bind_data;
}

duckdb::unique_ptr<FunctionData> MatchBind(ClientContext &, TableFunctionBindInput &input,
                                           duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	auto bind_data = ReadArguments(input);
	names = {"seq", "traj_id", "pid", "edge", "fraction", "distance", "ep", "tp", "sp_dist"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE,
	                LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> MatchInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<MapMatchBindData>();
	auto state = duckdb::make_uniq<MapMatchState<MatchedPointRow>>();
	const auto edges = LoadEdges(context, bind_data.edges_sql);
	const auto candidates = LoadCandidates(context, bind_data.candidates_sql);
	state->rows = MapMatch(edges, candidates, bind_data.options);
	return std::move(state);
}

void MatchScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<MapMatchState<MatchedPointRow>>();
	const idx_t count = duckdb::MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	output.SetCardinality(count);
	for (idx_t i = 0; i < count; i++) {
		auto &row = state.rows[state.offset + i];
		output.SetValue(0, i, Value::BIGINT(static_cast<int64_t>(state.offset + i) + 1));
		output.SetValue(1, i, Value::BIGINT(row.traj_id));
		output.SetValue(2, i, Value::BIGINT(row.pid));
		output.SetValue(3, i, Value::BIGINT(row.edge));
		output.SetValue(4, i, Value::DOUBLE(row.fraction));
		output.SetValue(5, i, Value::DOUBLE(row.distance));
		output.SetValue(6, i, Value::DOUBLE(row.ep));
		output.SetValue(7, i, Value::DOUBLE(row.tp));
		output.SetValue(8, i, Value::DOUBLE(DecodeInfinity(row.sp_dist)));
	}
	state.offset += count;
}

duckdb::unique_ptr<FunctionData> PathBind(ClientContext &, TableFunctionBindInput &input,
                                          duckdb::vector<LogicalType> &return_types, ColumnNames &names) {
	auto bind_data = ReadArguments(input);
	names = {"seq", "path_id", "path_seq", "start_vid", "end_vid", "node", "edge", "cost", "agg_cost"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::DOUBLE};
	return std::move(bind_data);
}

duckdb::unique_ptr<GlobalTableFunctionState> PathInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<MapMatchBindData>();
	auto state = duckdb::make_uniq<MapMatchState<KspRow>>();
	const auto edges = LoadEdges(context, bind_data.edges_sql);
	const auto candidates = LoadCandidates(context, bind_data.candidates_sql);
	state->rows = MapMatchPath(edges, candidates, bind_data.options, bind_data.details);
	return std::move(state);
}

void PathScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<MapMatchState<KspRow>>();
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
		output.SetValue(7, i, Value::DOUBLE(DecodeInfinity(row.cost)));
		output.SetValue(8, i, Value::DOUBLE(DecodeInfinity(row.agg_cost)));
	}
	state.offset += count;
}

//! Both functions take the two queries, optionally followed by gps_error.
template <typename Bind, typename Init, typename Scan>
TableFunctionSet MakeSet(const char *name, Bind bind, Init init, Scan scan, bool with_details) {
	TableFunctionSet set(name);
	for (size_t with_error = 0; with_error < 2; with_error++) {
		duckdb::vector<LogicalType> arguments {LogicalType::VARCHAR, LogicalType::VARCHAR};
		if (with_error) {
			arguments.push_back(LogicalType::DOUBLE);
		}
		TableFunction function(arguments, scan, bind, init);
		AddNamedParameter(function, "gps_error", LogicalType::DOUBLE);
		AddNamedParameter(function, "delta", LogicalType::DOUBLE);
		AddNamedParameter(function, "reverse_tolerance", LogicalType::DOUBLE);
		AddNamedParameter(function, "directed", LogicalType::BOOLEAN);
		if (with_details) {
			AddNamedParameter(function, "details", LogicalType::BOOLEAN);
		}
		set.AddFunction(function);
	}
	return set;
}

} // namespace

TableFunctionSet GetMapMatchFunction() {
	return MakeSet("duckrouting_map_match", MatchBind, MatchInit, MatchScan, false);
}

TableFunctionSet GetMapMatchPathFunction() {
	return MakeSet("duckrouting_map_match_path", PathBind, PathInit, PathScan, true);
}

} // namespace duckrouting
