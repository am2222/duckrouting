#include "duckrouting/edges.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"

#include <algorithm>

namespace duckrouting {

using duckdb::BinderException;
using duckdb::ClientContext;
using duckdb::Connection;
using duckdb::DatabaseInstance;
using duckdb::FlatVector;
using duckdb::InvalidInputException;
using duckdb::string;

namespace {

//! Case-insensitive membership test over the column names of `edges_sql`.
bool HasColumn(const duckdb::vector<string> &names, const char *wanted) {
	return std::any_of(names.begin(), names.end(),
	                   [&](const string &name) { return duckdb::StringUtil::CIEquals(name, wanted); });
}

void RequireColumn(const duckdb::vector<string> &names, const char *wanted) {
	if (!HasColumn(names, wanted)) {
		throw BinderException("duckrouting: the edges query must expose a '%s' column", wanted);
	}
}

} // namespace

std::vector<EdgeRow> LoadEdges(ClientContext &context, const string &edges_sql, bool require_id) {
	Connection connection(DatabaseInstance::GetDatabase(context));

	// Prepare (but do not run) the user's query so we can inspect its columns
	// and decide whether the optional reverse_cost is present.
	const string wrapped = "SELECT * FROM (" + edges_sql + ") AS __duckrouting_edges";
	auto prepared = connection.Prepare(wrapped);
	if (prepared->HasError()) {
		throw BinderException("duckrouting: could not prepare the edges query: %s", prepared->GetError());
	}

	auto &names = prepared->GetNames();
	const bool has_id = HasColumn(names, "id");
	if (require_id && !has_id) {
		RequireColumn(names, "id");
	}
	RequireColumn(names, "source");
	RequireColumn(names, "target");
	RequireColumn(names, "cost");
	const bool has_reverse_cost = HasColumn(names, "reverse_cost");

	// Let DuckDB do the type coercion, so any numeric input type works.
	string projection = "SELECT ";
	// Functions that never report an edge back may omit `id`; a row number
	// stands in so the rest of the pipeline is unchanged.
	projection += has_id ? "CAST(id AS BIGINT) AS id, " : "CAST(row_number() OVER () AS BIGINT) AS id, ";
	projection += "CAST(source AS BIGINT) AS source, "
	              "CAST(target AS BIGINT) AS target, CAST(cost AS DOUBLE) AS cost, ";
	projection += has_reverse_cost ? "CAST(reverse_cost AS DOUBLE) AS reverse_cost " : "CAST(-1 AS DOUBLE) AS reverse_cost ";
	projection += "FROM (" + edges_sql + ") AS __duckrouting_edges";

	auto result = connection.Query(projection);
	if (result->HasError()) {
		throw InvalidInputException("duckrouting: the edges query failed: %s", result->GetError());
	}

	std::vector<EdgeRow> edges;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		chunk->Flatten();
		auto ids = FlatVector::GetData<int64_t>(chunk->data[0]);
		auto sources = FlatVector::GetData<int64_t>(chunk->data[1]);
		auto targets = FlatVector::GetData<int64_t>(chunk->data[2]);
		auto costs = FlatVector::GetData<double>(chunk->data[3]);
		auto reverse_costs = FlatVector::GetData<double>(chunk->data[4]);

		auto &id_valid = FlatVector::Validity(chunk->data[0]);
		auto &source_valid = FlatVector::Validity(chunk->data[1]);
		auto &target_valid = FlatVector::Validity(chunk->data[2]);
		auto &cost_valid = FlatVector::Validity(chunk->data[3]);
		auto &reverse_valid = FlatVector::Validity(chunk->data[4]);

		for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
			if (!id_valid.RowIsValid(row) || !source_valid.RowIsValid(row) || !target_valid.RowIsValid(row) ||
			    !cost_valid.RowIsValid(row)) {
				throw InvalidInputException(
				    "duckrouting: the edges query returned NULL in id, source, target or cost");
			}
			// A NULL reverse_cost means "no reverse edge", same as a negative one.
			const double reverse_cost = reverse_valid.RowIsValid(row) ? reverse_costs[row] : -1.0;
			// Note this also maps -Infinity onto the sentinel, turning what looks
			// like "no edge" into a maximally expensive one. That is pgRouting's
			// behaviour too: it tests std::isinf before it tests cost < 0.
			edges.push_back(EdgeRow {ids[row], sources[row], targets[row], EncodeInfinity(costs[row]),
			                         EncodeInfinity(reverse_cost)});
		}
	}
	return edges;
}

std::vector<FlowEdgeRow> LoadFlowEdges(ClientContext &context, const string &edges_sql, bool require_capacity,
                                       bool require_cost) {
	Connection connection(DatabaseInstance::GetDatabase(context));

	const string wrapped = "SELECT * FROM (" + edges_sql + ") AS __duckrouting_edges";
	auto prepared = connection.Prepare(wrapped);
	if (prepared->HasError()) {
		throw BinderException("duckrouting: could not prepare the edges query: %s", prepared->GetError());
	}

	auto &names = prepared->GetNames();
	RequireColumn(names, "id");
	RequireColumn(names, "source");
	RequireColumn(names, "target");
	if (require_capacity) {
		RequireColumn(names, "capacity");
	}
	if (require_cost) {
		RequireColumn(names, "cost");
	}

	// Absent optional columns become -1, which every caller reads as "this
	// direction is unusable".
	auto column = [&](const char *name) -> string {
		return HasColumn(names, name) ? "CAST(" + string(name) + " AS DOUBLE)" : "CAST(-1 AS DOUBLE)";
	};
	string projection = "SELECT CAST(id AS BIGINT) AS id, CAST(source AS BIGINT) AS source, "
	                    "CAST(target AS BIGINT) AS target, " +
	                    column("capacity") + " AS capacity, " + column("reverse_capacity") +
	                    " AS reverse_capacity, " + column("cost") + " AS cost, " + column("reverse_cost") +
	                    " AS reverse_cost FROM (" + edges_sql + ") AS __duckrouting_edges";

	auto result = connection.Query(projection);
	if (result->HasError()) {
		throw InvalidInputException("duckrouting: the edges query failed: %s", result->GetError());
	}

	std::vector<FlowEdgeRow> edges;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		chunk->Flatten();
		auto ids = FlatVector::GetData<int64_t>(chunk->data[0]);
		auto sources = FlatVector::GetData<int64_t>(chunk->data[1]);
		auto targets = FlatVector::GetData<int64_t>(chunk->data[2]);
		auto capacities = FlatVector::GetData<double>(chunk->data[3]);
		auto reverse_capacities = FlatVector::GetData<double>(chunk->data[4]);
		auto costs = FlatVector::GetData<double>(chunk->data[5]);
		auto reverse_costs = FlatVector::GetData<double>(chunk->data[6]);

		for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
			if (!FlatVector::Validity(chunk->data[0]).RowIsValid(row) ||
			    !FlatVector::Validity(chunk->data[1]).RowIsValid(row) ||
			    !FlatVector::Validity(chunk->data[2]).RowIsValid(row)) {
				throw InvalidInputException("duckrouting: the edges query returned NULL in id, source or target");
			}
			// A NULL in any optional column means the same as a negative one.
			auto value = [&](duckdb::idx_t col, const double *data) {
				return FlatVector::Validity(chunk->data[col]).RowIsValid(row) ? data[row] : -1.0;
			};
			edges.push_back(FlowEdgeRow {ids[row], sources[row], targets[row], value(3, capacities),
			                             value(4, reverse_capacities), value(5, costs),
			                             value(6, reverse_costs)});
		}
	}
	return edges;
}

std::vector<CoordinateEdgeRow> LoadCoordinateEdges(ClientContext &context, const string &edges_sql) {
	Connection connection(DatabaseInstance::GetDatabase(context));

	const string wrapped = "SELECT * FROM (" + edges_sql + ") AS __duckrouting_edges";
	auto prepared = connection.Prepare(wrapped);
	if (prepared->HasError()) {
		throw BinderException("duckrouting: could not prepare the edges query: %s", prepared->GetError());
	}

	auto &names = prepared->GetNames();
	RequireColumn(names, "id");
	RequireColumn(names, "source");
	RequireColumn(names, "target");
	RequireColumn(names, "cost");
	// The heuristic cannot work without knowing where the vertices are.
	RequireColumn(names, "x1");
	RequireColumn(names, "y1");
	RequireColumn(names, "x2");
	RequireColumn(names, "y2");
	const bool has_reverse_cost = HasColumn(names, "reverse_cost");

	string projection = "SELECT CAST(id AS BIGINT) AS id, CAST(source AS BIGINT) AS source, "
	                    "CAST(target AS BIGINT) AS target, CAST(cost AS DOUBLE) AS cost, ";
	projection += has_reverse_cost ? "CAST(reverse_cost AS DOUBLE) AS reverse_cost, "
	                               : "CAST(-1 AS DOUBLE) AS reverse_cost, ";
	projection += "CAST(x1 AS DOUBLE) AS x1, CAST(y1 AS DOUBLE) AS y1, CAST(x2 AS DOUBLE) AS x2, "
	              "CAST(y2 AS DOUBLE) AS y2 FROM (" +
	              edges_sql + ") AS __duckrouting_edges";

	auto result = connection.Query(projection);
	if (result->HasError()) {
		throw InvalidInputException("duckrouting: the edges query failed: %s", result->GetError());
	}

	std::vector<CoordinateEdgeRow> edges;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		chunk->Flatten();
		auto ids = FlatVector::GetData<int64_t>(chunk->data[0]);
		auto sources = FlatVector::GetData<int64_t>(chunk->data[1]);
		auto targets = FlatVector::GetData<int64_t>(chunk->data[2]);
		auto costs = FlatVector::GetData<double>(chunk->data[3]);
		auto reverse_costs = FlatVector::GetData<double>(chunk->data[4]);
		auto x1 = FlatVector::GetData<double>(chunk->data[5]);
		auto y1 = FlatVector::GetData<double>(chunk->data[6]);
		auto x2 = FlatVector::GetData<double>(chunk->data[7]);
		auto y2 = FlatVector::GetData<double>(chunk->data[8]);

		for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
			for (duckdb::idx_t col = 0; col < 4; col++) {
				if (!FlatVector::Validity(chunk->data[col]).RowIsValid(row)) {
					throw InvalidInputException(
					    "duckrouting: the edges query returned NULL in id, source, target or cost");
				}
			}
			auto coordinate = [&](duckdb::idx_t col, const double *data) {
				if (!FlatVector::Validity(chunk->data[col]).RowIsValid(row)) {
					throw InvalidInputException("duckrouting: the edges query returned NULL in x1, y1, x2 or y2");
				}
				return data[row];
			};
			const double reverse_cost =
			    FlatVector::Validity(chunk->data[4]).RowIsValid(row) ? reverse_costs[row] : -1.0;
			CoordinateEdgeRow entry {};
			entry.edge = EdgeRow {ids[row], sources[row], targets[row], EncodeInfinity(costs[row]),
			                      EncodeInfinity(reverse_cost)};
			entry.x1 = coordinate(5, x1);
			entry.y1 = coordinate(6, y1);
			entry.x2 = coordinate(7, x2);
			entry.y2 = coordinate(8, y2);
			edges.push_back(entry);
		}
	}
	return edges;
}

std::vector<MatrixCell> LoadCostMatrix(ClientContext &context, const string &matrix_sql) {
	Connection connection(DatabaseInstance::GetDatabase(context));

	auto prepared = connection.Prepare("SELECT * FROM (" + matrix_sql + ") AS __duckrouting_matrix");
	if (prepared->HasError()) {
		throw BinderException("duckrouting: could not prepare the matrix query: %s", prepared->GetError());
	}
	auto &names = prepared->GetNames();
	RequireColumn(names, "start_vid");
	RequireColumn(names, "end_vid");
	RequireColumn(names, "agg_cost");

	auto result = connection.Query("SELECT CAST(start_vid AS BIGINT) AS start_vid, "
	                               "CAST(end_vid AS BIGINT) AS end_vid, CAST(agg_cost AS DOUBLE) AS agg_cost "
	                               "FROM (" +
	                               matrix_sql + ") AS __duckrouting_matrix");
	if (result->HasError()) {
		throw InvalidInputException("duckrouting: the matrix query failed: %s", result->GetError());
	}

	std::vector<MatrixCell> cells;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		chunk->Flatten();
		auto starts = FlatVector::GetData<int64_t>(chunk->data[0]);
		auto ends = FlatVector::GetData<int64_t>(chunk->data[1]);
		auto costs = FlatVector::GetData<double>(chunk->data[2]);
		for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
			for (duckdb::idx_t col = 0; col < 3; col++) {
				if (!FlatVector::Validity(chunk->data[col]).RowIsValid(row)) {
					throw InvalidInputException(
					    "duckrouting: the matrix query returned NULL in start_vid, end_vid or agg_cost");
				}
			}
			cells.push_back(MatrixCell {starts[row], ends[row], costs[row]});
		}
	}
	return cells;
}

std::vector<PlacedPoint> LoadPoints(ClientContext &context, const string &points_sql) {
	Connection connection(DatabaseInstance::GetDatabase(context));

	auto prepared = connection.Prepare("SELECT * FROM (" + points_sql + ") AS __duckrouting_points");
	if (prepared->HasError()) {
		throw BinderException("duckrouting: could not prepare the coordinates query: %s", prepared->GetError());
	}
	auto &names = prepared->GetNames();
	RequireColumn(names, "id");
	RequireColumn(names, "x");
	RequireColumn(names, "y");

	auto result = connection.Query("SELECT CAST(id AS BIGINT) AS id, CAST(x AS DOUBLE) AS x, "
	                               "CAST(y AS DOUBLE) AS y FROM (" +
	                               points_sql + ") AS __duckrouting_points");
	if (result->HasError()) {
		throw InvalidInputException("duckrouting: the coordinates query failed: %s", result->GetError());
	}

	std::vector<PlacedPoint> points;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		chunk->Flatten();
		auto ids = FlatVector::GetData<int64_t>(chunk->data[0]);
		auto xs = FlatVector::GetData<double>(chunk->data[1]);
		auto ys = FlatVector::GetData<double>(chunk->data[2]);
		for (duckdb::idx_t row = 0; row < chunk->size(); row++) {
			for (duckdb::idx_t col = 0; col < 3; col++) {
				if (!FlatVector::Validity(chunk->data[col]).RowIsValid(row)) {
					throw InvalidInputException("duckrouting: the coordinates query returned NULL in id, x or y");
				}
			}
			points.push_back(PlacedPoint {ids[row], xs[row], ys[row]});
		}
	}
	return points;
}

} // namespace duckrouting
