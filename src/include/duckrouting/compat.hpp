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

//! The text of a DuckDB name, for the cases where one has to be read back out
//! as a plain `string` -- the keys of a named-parameter map, say. A `string` is
//! already text; v2's `Identifier` keeps its raw value behind
//! `GetIdentifierName()`, because converting one to a string discards its
//! case-insensitive semantics and DuckDB makes you ask for that. The same
//! exact-overload versus template split as above picks between them.
inline const std::string &NameText(const std::string &name) {
	return name;
}

template <typename Name>
inline auto NameText(const Name &name) -> decltype((name.GetIdentifierName())) {
	return name.GetIdentifierName();
}

//! The name a CreateInfo carries. v1.5 has it as a `string` member; v2 moved it
//! into a qualified name reachable through `GetFunctionName()`.
template <typename Info>
inline auto InfoName(const Info &info) -> decltype((info.name)) {
	return info.name;
}

template <typename Info>
inline auto InfoName(const Info &info) -> decltype((info.GetFunctionName())) {
	return info.GetFunctionName();
}

//! The positional argument types of one function overload. v1.5 exposes them as
//! a plain member; v2 adds an accessor and keeps the member. The two candidates
//! are ranked rather than overloaded, because where both spellings exist an
//! unranked pair is ambiguous rather than merely redundant -- `0` is an `int`,
//! so the accessor wins, and the member is only reached when it does not exist.
namespace detail {

template <typename Function>
inline auto ArgumentTypes(const Function &function, int) -> decltype((function.GetArguments())) {
	return function.GetArguments();
}

template <typename Function>
inline auto ArgumentTypes(const Function &function, long) -> decltype((function.arguments)) {
	return function.arguments;
}

} // namespace detail

template <typename Function>
inline auto ArgumentTypes(const Function &function) -> decltype(detail::ArgumentTypes(function, 0)) {
	return detail::ArgumentTypes(function, 0);
}

//! One overload out of a function set. v1.5 stores them by value; v2 shares
//! them behind `shared_ptr` so that a bound function keeps its overload alive.
template <typename Function>
inline const Function &Overload(const Function &function) {
	return function;
}

template <typename Function>
inline const Function &Overload(const duckdb::shared_ptr<const Function> &function) {
	return *function;
}

} // namespace duckrouting
