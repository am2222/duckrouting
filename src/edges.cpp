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

std::vector<EdgeRow> LoadEdges(ClientContext &context, const string &edges_sql) {
	Connection connection(DatabaseInstance::GetDatabase(context));

	// Prepare (but do not run) the user's query so we can inspect its columns
	// and decide whether the optional reverse_cost is present.
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
	const bool has_reverse_cost = HasColumn(names, "reverse_cost");

	// Let DuckDB do the type coercion, so any numeric input type works.
	string projection = "SELECT CAST(id AS BIGINT) AS id, CAST(source AS BIGINT) AS source, "
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
			edges.push_back(EdgeRow {ids[row], sources[row], targets[row], costs[row], reverse_cost});
		}
	}
	return edges;
}

} // namespace duckrouting
