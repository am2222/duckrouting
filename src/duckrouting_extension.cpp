#define DUCKDB_EXTENSION_MAIN

#include "duckrouting_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckrouting/dijkstra.hpp"
#include "duckrouting/graph_functions.hpp"

#include <boost/version.hpp>

namespace duckdb {

inline void DuckroutingVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Duckrouting " + name.GetString() + ", built against Boost " +
		                                           BOOST_LIB_VERSION);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	auto duckrouting_version_scalar_function =
	    ScalarFunction("duckrouting_version", {LogicalType::VARCHAR}, LogicalType::VARCHAR, DuckroutingVersionScalarFun);
	loader.RegisterFunction(duckrouting_version_scalar_function);

	loader.RegisterFunction(duckrouting::GetDijkstraFunction());
	loader.RegisterFunction(duckrouting::GetDijkstraCostFunction());
	loader.RegisterFunction(duckrouting::GetDijkstraCostMatrixFunction());
	loader.RegisterFunction(duckrouting::GetDrivingDistanceFunction());
	loader.RegisterFunction(duckrouting::GetDijkstraViaFunction());
	loader.RegisterFunction(duckrouting::GetDijkstraNearFunction());
	loader.RegisterFunction(duckrouting::GetDijkstraNearCostFunction());
	loader.RegisterFunction(duckrouting::GetKspFunction());

	loader.RegisterFunction(duckrouting::GetFloydWarshallFunction());
	loader.RegisterFunction(duckrouting::GetJohnsonFunction());
	loader.RegisterFunction(duckrouting::GetConnectedComponentsFunction());
	loader.RegisterFunction(duckrouting::GetStrongComponentsFunction());
	loader.RegisterFunction(duckrouting::GetBiconnectedComponentsFunction());
	loader.RegisterFunction(duckrouting::GetArticulationPointsFunction());
	loader.RegisterFunction(duckrouting::GetBridgesFunction());
	loader.RegisterFunction(duckrouting::GetMakeConnectedFunction());
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
