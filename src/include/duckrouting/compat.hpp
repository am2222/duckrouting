#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include <string>
#include <type_traits>

namespace duckrouting {

//! DuckDB v2 introduced an `Identifier` type and uses it wherever a *name* is
//! meant -- the column names a bind function fills in, the names a prepared
//! statement reports, and the keys of a named-parameter map. v1.5 used plain
//! `string` for all three.
//!
//! Rather than pick one and break on the other, the two helpers here derive
//! what they need from DuckDB's own declarations, so duckrouting compiles
//! against either version without a preprocessor branch.

namespace detail {

template <typename Signature>
struct BindSignature;

//! Picks the fourth parameter out of DuckDB's bind typedef: that is the vector
//! of output column names, whatever DuckDB currently calls its element type.
template <typename Result, typename Context, typename Input, typename Types, typename Names>
struct BindSignature<Result (*)(Context, Input, Types, Names)> {
	typedef typename std::remove_reference<Names>::type type;
};

} // namespace detail

//! The vector of output column names a bind function is handed.
typedef detail::BindSignature<duckdb::table_function_bind_t>::type ColumnNames;

//! Case-insensitive comparison of a DuckDB name against a literal.
//!
//! An `Identifier` already compares case-insensitively, so `==` is the right
//! operator for it. A plain `string` does not, so it needs `CIEquals`. The
//! exact overload wins for `string` and the template picks up everything else,
//! which means neither version's name type has to be spelled out here.
inline bool NameMatches(const std::string &name, const char *wanted) {
	return duckdb::StringUtil::CIEquals(name, wanted);
}

template <typename Name>
inline auto NameMatches(const Name &name, const char *wanted) -> decltype(name == wanted, bool()) {
	return name == wanted;
}

} // namespace duckrouting
