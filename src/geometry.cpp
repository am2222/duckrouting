#include "duckrouting/graph_functions.hpp"
#include "duckrouting/function_docs.hpp"

#include "duckdb/catalog/default/default_table_functions.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckrouting {

namespace {

//! These three functions are pure SQL in pgRouting too -- they are geometry
//! questions, not graph ones. Registering them as table macros keeps them that
//! way, and means the spatial extension is only needed when one is actually
//! called rather than when duckrouting is loaded.
//!
//! Each takes a query string, so they read `query_table(...)` to turn the
//! argument into a relation, the same trick DuckDB's own table macros use.

const duckdb::DefaultTableMacro kGeometryMacros[] = {
    // Splits every edge at the points where it crosses another edge.
    {DEFAULT_SCHEMA,
     "duckrouting_separate_crossing",
     {"edges_sql", nullptr},
     {{"tolerance", "0.01"}, {nullptr, nullptr}},
     R"(
      WITH __edges AS (SELECT id, geom FROM query_table(edges_sql)),
      __cuts AS (
        SELECT a.id AS id,
               ST_LineLocatePoint(a.geom, ST_Intersection(a.geom, b.geom)) AS fraction
        FROM __edges a JOIN __edges b ON a.id <> b.id
        WHERE ST_Crosses(a.geom, b.geom)
          AND ST_GeometryType(ST_Intersection(a.geom, b.geom)) = 'POINT'
      ),
      __grouped AS (
        SELECT id, list_sort(list_distinct(list(fraction) || [0.0, 1.0])) AS fractions
        FROM __cuts GROUP BY id
      )
      SELECT row_number() OVER (ORDER BY g.id, s.i) AS seq,
             g.id AS id,
             s.i AS sub_id,
             ST_LineSubstring(e.geom, g.fractions[s.i], g.fractions[s.i + 1]) AS geom
      FROM __grouped g
      JOIN __edges e USING (id),
      LATERAL (SELECT unnest(range(1, len(g.fractions))) AS i) s
      WHERE g.fractions[s.i + 1] - g.fractions[s.i] > tolerance
     )"},

    // Splits every edge at the points where another edge's endpoint touches
    // its interior -- a junction that was never noded.
    {DEFAULT_SCHEMA,
     "duckrouting_separate_touching",
     {"edges_sql", nullptr},
     {{"tolerance", "0.01"}, {nullptr, nullptr}},
     R"(
      WITH __edges AS (SELECT id, geom FROM query_table(edges_sql)),
      __ends AS (
        SELECT id, ST_StartPoint(geom) AS endpoint FROM __edges
        UNION ALL
        SELECT id, ST_EndPoint(geom) FROM __edges
      ),
      __cuts AS (
        SELECT a.id AS id, ST_LineLocatePoint(a.geom, b.endpoint) AS fraction
        FROM __edges a JOIN __ends b ON a.id <> b.id
        WHERE ST_DWithin(a.geom, b.endpoint, tolerance)
      ),
      __grouped AS (
        SELECT id, list_sort(list_distinct(list(fraction) || [0.0, 1.0])) AS fractions
        FROM __cuts GROUP BY id
      )
      SELECT row_number() OVER (ORDER BY g.id, s.i) AS seq,
             g.id AS id,
             s.i AS sub_id,
             ST_LineSubstring(e.geom, g.fractions[s.i], g.fractions[s.i + 1]) AS geom
      FROM __grouped g
      JOIN __edges e USING (id),
      LATERAL (SELECT unnest(range(1, len(g.fractions))) AS i) s
      WHERE g.fractions[s.i + 1] - g.fractions[s.i] > tolerance
     )"},

    // For each point, the nearest edges within tolerance, reported in the
    // shape the withPoints family expects: an edge id and a fraction along it.
    {DEFAULT_SCHEMA,
     "duckrouting_find_close_edges",
     {"edges_sql", "points_sql", nullptr},
     {{"tolerance", "0.01"}, {"cap", "1"}, {nullptr, nullptr}},
     R"(
      WITH __edges AS (SELECT id, geom FROM query_table(edges_sql)),
      __points AS (SELECT pid, geom FROM query_table(points_sql)),
      __near AS (
        SELECT p.pid AS pid,
               e.id AS edge_id,
               ST_Distance(e.geom, p.geom) AS distance,
               ST_LineLocatePoint(e.geom, p.geom) AS fraction,
               ST_MakeLine(p.geom, ST_ClosestPoint(e.geom, p.geom)) AS edge,
               ST_ClosestPoint(e.geom, p.geom) AS geom,
               row_number() OVER (PARTITION BY p.pid ORDER BY ST_Distance(e.geom, p.geom), e.id) AS rank
        FROM __points p JOIN __edges e ON ST_DWithin(e.geom, p.geom, tolerance)
      )
      SELECT row_number() OVER (ORDER BY pid, rank) AS seq,
             pid, edge_id, fraction, distance, geom, edge
      FROM __near WHERE rank <= cap
     )"},
    {nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}};

} // namespace

void RegisterGeometryMacros(duckdb::ExtensionLoader &loader) {
	for (size_t i = 0; kGeometryMacros[i].name != nullptr; i++) {
		auto info = duckdb::DefaultTableFunctionGenerator::CreateTableMacroInfo(kGeometryMacros[i]);
		// A macro already carries its own parameter names, so this only adds the
		// prose duckdb_functions() would otherwise report as NULL.
		Document(*info);
		loader.RegisterFunction(*info);
	}
}

} // namespace duckrouting
