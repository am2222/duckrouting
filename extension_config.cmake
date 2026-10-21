# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(duckrouting
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)
# Building an extension no longer links it into the shell and unittest; without
# this the SQL tests skip on `require duckrouting`.
duckdb_extension_statically_link(duckrouting)

# The three geometry helpers are SQL macros over DuckDB's spatial extension, so
# spatial is only needed at runtime -- `INSTALL spatial; LOAD spatial;` -- not
# at build time. Building it here instead would pull in GDAL, PROJ, GEOS and
# five more vcpkg dependencies on every build, which is a steep price for three
# macros. Uncomment to bundle it:
#
# duckdb_extension_load(spatial
#     LOAD_TESTS
#     GIT_URL https://github.com/duckdb/duckdb-spatial
#     GIT_TAG main
# )