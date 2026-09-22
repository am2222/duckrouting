#define DUCKDB_EXTENSION_MAIN

#include "duckrouting_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckrouting/dijkstra.hpp"
#include "duckrouting/function_docs.hpp"
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

//! Every registration goes through duckrouting::Documented, which swaps the
//! bare RegisterFunction overload -- the one with nowhere to put a description
//! -- for the CreateInfo form, and fills it in from src/function_docs.cpp.
//! Without it duckdb_functions() reports these as undocumented `col0`
//! signatures, which is all an agent connected to a database can see.
using duckrouting::Documented;

static void LoadInternal(ExtensionLoader &loader) {
	auto duckrouting_version_scalar_function = ScalarFunction("duckrouting_version", {LogicalType::VARCHAR},
	                                                          LogicalType::VARCHAR, DuckroutingVersionScalarFun);
	loader.RegisterFunction(Documented(duckrouting_version_scalar_function));

	loader.RegisterFunction(Documented(duckrouting::GetDijkstraFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDijkstraCostFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDijkstraCostMatrixFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDrivingDistanceFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDijkstraViaFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDijkstraNearFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDijkstraNearCostFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetKspFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetFloydWarshallFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetJohnsonFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetConnectedComponentsFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetStrongComponentsFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBiconnectedComponentsFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetArticulationPointsFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBridgesFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetMakeConnectedFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetKruskalFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetPrimFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetKruskalBFSFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetKruskalDFSFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetKruskalDDFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetPrimBFSFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetPrimDFSFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetPrimDDFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetBreadthFirstSearchFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDepthFirstSearchFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBellmanFordFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDagShortestPathFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetTransitiveClosureFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetCuthillMckeeOrderingFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetKingOrderingFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetSloanOrderingFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetTopologicalSortFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetSequentialVertexColoringFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetEdgeColoringFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBipartiteFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetIsPlanarFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBoyerMyrvoldFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBandwidthFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBetweennessCentralityFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetStoerWagnerFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetHawickCircuitsFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDominatorTreeFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetMaxFlowFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetPushRelabelFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetEdmondsKarpFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBoykovKolmogorovFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetMaxFlowMinCostFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetMaxFlowMinCostCostFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetEdgeDisjointPathsFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetMaxCardinalityMatchFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetAStarFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetAStarCostFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetAStarCostMatrixFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetTspFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetTspEuclideanFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetContractionHierarchiesFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetWithPointsDDFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetBdDijkstraFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBdDijkstraCostFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBdDijkstraCostMatrixFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBdAStarFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBdAStarCostFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBdAStarCostMatrixFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetEdwardMooreFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetBinaryBreadthFirstSearchFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetExtractVerticesFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDegreeFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetFullVersionFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetWithPointsFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetWithPointsCostFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetWithPointsCostMatrixFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetWithPointsViaFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetWithPointsKspFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetContractionFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetDeadEndContractionFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetLinearContractionFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetTrspFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetTrspViaFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetTrspWithPointsFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetTrspViaWithPointsFunction()));

	loader.RegisterFunction(Documented(duckrouting::GetLineGraphFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetLineGraphFullFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetChinesePostmanFunction()));
	loader.RegisterFunction(Documented(duckrouting::GetChinesePostmanCostFunction()));

	duckrouting::RegisterGeometryMacros(loader);
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
