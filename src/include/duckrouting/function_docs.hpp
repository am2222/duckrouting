#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckrouting {

//! The bare `loader.RegisterFunction(fn)` overloads have nowhere to put a
//! description, so everything an agent can learn about a function over a SQL
//! connection -- `duckdb_functions()` -- comes back empty, down to `col0` for
//! the argument names. These helpers wrap a function in its `Create*Info`
//! form, which does carry documentation, and fill it in from the table in
//! `function_docs.cpp`.
//!
//! One `FunctionDescription` is produced per overload, because that is the
//! granularity `duckdb_functions()` matches at: a single description is only
//! reused across overloads when the argument types do not contradict it, and
//! these functions vary both the count and the type of their arguments.
//!
//! Registering a function with no entry in the table is an error rather than
//! an undocumented function, so the two cannot drift apart.

duckdb::CreateTableFunctionInfo Documented(duckdb::TableFunctionSet set);
duckdb::CreateScalarFunctionInfo Documented(duckdb::ScalarFunction function);

//! Table macros already report their real parameter names, so they only need
//! the prose.
void Document(duckdb::CreateMacroInfo &info);

} // namespace duckrouting
