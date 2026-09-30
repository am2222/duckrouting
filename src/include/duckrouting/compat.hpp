#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include <string>
#include <type_traits>
#include <vector>

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

//! Overload ranking for the helpers below: a call made with Rank<2> prefers
//! the Rank<2> candidate, falls back to Rank<1>, and to Rank<0> last. Each
//! candidate is only viable where the DuckDB API it reaches for exists.
namespace detail {

template <int N>
struct Rank : Rank<N - 1> {};
template <>
struct Rank<0> {};

} // namespace detail

//! The positional argument types of one function overload. v1.5 exposes them as
//! a plain member; an earlier v2 added an accessor and kept the member; the
//! current v2 keeps them inside a FunctionSignature, as the parameters a caller
//! can pass by position. The three are ranked rather than overloaded, because
//! where two spellings exist an unranked pair is ambiguous rather than merely
//! redundant.
namespace detail {

template <typename Function>
inline auto ArgumentTypes(const Function &function, Rank<2>) -> decltype((function.GetArguments())) {
	return function.GetArguments();
}

template <typename Function>
inline auto ArgumentTypes(const Function &function, Rank<1>) -> decltype((function.arguments)) {
	return function.arguments;
}

template <typename Function>
inline auto ArgumentTypes(const Function &function, Rank<0>)
    -> decltype(function.GetSignature().GetParameterCount(), duckdb::vector<duckdb::LogicalType>()) {
	duckdb::vector<duckdb::LogicalType> types;
	const auto &signature = function.GetSignature();
	for (duckdb::idx_t i = 0; i < signature.GetParameterCount(); i++) {
		const auto &parameter = signature.GetParameter(i);
		if (parameter.AcceptsPosition()) {
			types.push_back(parameter.GetType());
		}
	}
	return types;
}

} // namespace detail

template <typename Function>
inline auto ArgumentTypes(const Function &function) -> decltype(detail::ArgumentTypes(function, detail::Rank<2>())) {
	return detail::ArgumentTypes(function, detail::Rank<2>());
}

//! Declares a named parameter on a table function. v1.5 keeps a map of them on
//! the function; the current v2 has no such map, and instead takes them as the
//! typed options of a "**options" parameter on the function's signature, one
//! call to declare the first and another to extend the set.
namespace detail {

template <typename Function>
inline auto AddNamedParameter(Function &function, const char *name, const duckdb::LogicalType &type, Rank<1>)
    -> decltype(function.named_parameters[name] = type, void()) {
	function.named_parameters[name] = type;
}

template <typename Function>
inline auto AddNamedParameter(Function &function, const char *name, const duckdb::LogicalType &type, Rank<0>)
    -> decltype(function.GetSignature().GetTypedKwargs(), void()) {
	// The options type is only spelled through the signature, so this
	// candidate stays a template that v1.5 never has to instantiate.
	typedef typename std::remove_const<
	    typename std::remove_reference<decltype(*function.GetSignature().GetTypedKwargs())>::type>::type Options;
	auto &signature = function.GetSignature();
	// A literal converts to v2's Identifier implicitly; a std::string does not.
	auto add = [&](Options &options) {
		options.Add(name, type);
	};
	if (signature.GetTypedKwargs()) {
		signature.ExtendTypedKwargs(add);
	} else {
		signature.WithTypedKwargs("options", add);
	}
}

} // namespace detail

template <typename Function>
inline void AddNamedParameter(Function &function, const char *name, const duckdb::LogicalType &type) {
	detail::AddNamedParameter(function, name, type, detail::Rank<1>());
}

//! The names of a table function's named parameters, in the order DuckDB
//! reports them, read back off wherever the version at hand keeps them.
namespace detail {

template <typename Function>
inline auto NamedParameterNames(const Function &function, Rank<1>)
    -> decltype(function.named_parameters.begin(), std::vector<std::string>()) {
	std::vector<std::string> names;
	for (auto &entry : function.named_parameters) {
		names.push_back(NameText(entry.first));
	}
	return names;
}

template <typename Function>
inline auto NamedParameterNames(const Function &function, Rank<0>)
    -> decltype(function.GetSignature().GetTypedKwargs(), std::vector<std::string>()) {
	std::vector<std::string> names;
	auto options = function.GetSignature().GetTypedKwargs();
	if (options) {
		for (auto &name : options->GetNames()) {
			names.push_back(NameText(name));
		}
	}
	return names;
}

} // namespace detail

template <typename Function>
inline std::vector<std::string> NamedParameterNames(const Function &function) {
	return detail::NamedParameterNames(function, detail::Rank<1>());
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
