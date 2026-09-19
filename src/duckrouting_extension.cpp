#define DUCKDB_EXTENSION_MAIN

#include "duckrouting_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void DuckroutingScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void DuckroutingOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Duckrouting " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto duckrouting_scalar_function =
	    ScalarFunction("duckrouting", {LogicalType::VARCHAR}, LogicalType::VARCHAR, DuckroutingScalarFun);

	loader.RegisterFunction(duckrouting_scalar_function);

	// Register another scalar function
	auto duckrouting_openssl_version_scalar_function = ScalarFunction("duckrouting_openssl_version", {LogicalType::VARCHAR},
	                                                             LogicalType::VARCHAR, DuckroutingOpenSSLVersionScalarFun);
	loader.RegisterFunction(duckrouting_openssl_version_scalar_function);
}

void DuckroutingExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string DuckroutingExtension::Name() {
	return "duckrouting";
}

std::string DuckroutingExtension::Version() const {
#ifdef EXT_VERSION_DUCKROUTING
	return EXT_VERSION_DUCKROUTING;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duckrouting, loader) {
	duckdb::LoadInternal(loader);
}
}
