#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckrouting {

//! Declares a named option on a table function. DuckDB takes these as the typed
//! kwargs of a "**options" parameter on the function's signature: the first
//! option declares that parameter, and every later one extends it.
inline void AddNamedParameter(duckdb::TableFunction &function, const char *name, const duckdb::LogicalType &type) {
	auto &signature = function.GetSignature();
	auto add = [&](duckdb::TypedKwargs &options) {
		options.Add(name, type);
	};
	if (signature.GetTypedKwargs()) {
		signature.ExtendTypedKwargs(add);
	} else {
		signature.WithTypedKwargs("options", add);
	}
}

} // namespace duckrouting
