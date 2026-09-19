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
	auto duckrouting_version_scalar_function = ScalarFunction("duckrouting_version", {LogicalType::VARCHAR},
	                                                          LogicalType::VARCHAR, DuckroutingVersionScalarFun);
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

	loader.RegisterFunction(duckrouting::GetKruskalFunction());
	loader.RegisterFunction(duckrouting::GetPrimFunction());
	loader.RegisterFunction(duckrouting::GetKruskalBFSFunction());
	loader.RegisterFunction(duckrouting::GetKruskalDFSFunction());
	loader.RegisterFunction(duckrouting::GetKruskalDDFunction());
	loader.RegisterFunction(duckrouting::GetPrimBFSFunction());
	loader.RegisterFunction(duckrouting::GetPrimDFSFunction());
	loader.RegisterFunction(duckrouting::GetPrimDDFunction());

	loader.RegisterFunction(duckrouting::GetBreadthFirstSearchFunction());
	loader.RegisterFunction(duckrouting::GetDepthFirstSearchFunction());
	loader.RegisterFunction(duckrouting::GetBellmanFordFunction());
	loader.RegisterFunction(duckrouting::GetDagShortestPathFunction());
	loader.RegisterFunction(duckrouting::GetTransitiveClosureFunction());
	loader.RegisterFunction(duckrouting::GetCuthillMckeeOrderingFunction());
	loader.RegisterFunction(duckrouting::GetKingOrderingFunction());
	loader.RegisterFunction(duckrouting::GetSloanOrderingFunction());
	loader.RegisterFunction(duckrouting::GetTopologicalSortFunction());

	loader.RegisterFunction(duckrouting::GetSequentialVertexColoringFunction());
	loader.RegisterFunction(duckrouting::GetEdgeColoringFunction());
	loader.RegisterFunction(duckrouting::GetBipartiteFunction());
	loader.RegisterFunction(duckrouting::GetIsPlanarFunction());
	loader.RegisterFunction(duckrouting::GetBoyerMyrvoldFunction());
	loader.RegisterFunction(duckrouting::GetBandwidthFunction());
	loader.RegisterFunction(duckrouting::GetBetweennessCentralityFunction());
	loader.RegisterFunction(duckrouting::GetStoerWagnerFunction());
	loader.RegisterFunction(duckrouting::GetHawickCircuitsFunction());
	loader.RegisterFunction(duckrouting::GetDominatorTreeFunction());

	loader.RegisterFunction(duckrouting::GetMaxFlowFunction());
	loader.RegisterFunction(duckrouting::GetPushRelabelFunction());
	loader.RegisterFunction(duckrouting::GetEdmondsKarpFunction());
	loader.RegisterFunction(duckrouting::GetBoykovKolmogorovFunction());
	loader.RegisterFunction(duckrouting::GetMaxFlowMinCostFunction());
	loader.RegisterFunction(duckrouting::GetMaxFlowMinCostCostFunction());
	loader.RegisterFunction(duckrouting::GetEdgeDisjointPathsFunction());
	loader.RegisterFunction(duckrouting::GetMaxCardinalityMatchFunction());

	loader.RegisterFunction(duckrouting::GetAStarFunction());
	loader.RegisterFunction(duckrouting::GetAStarCostFunction());
	loader.RegisterFunction(duckrouting::GetAStarCostMatrixFunction());

	loader.RegisterFunction(duckrouting::GetTspFunction());
	loader.RegisterFunction(duckrouting::GetTspEuclideanFunction());

	loader.RegisterFunction(duckrouting::GetContractionHierarchiesFunction());
	loader.RegisterFunction(duckrouting::GetWithPointsDDFunction());

	loader.RegisterFunction(duckrouting::GetBdDijkstraFunction());
	loader.RegisterFunction(duckrouting::GetBdDijkstraCostFunction());
	loader.RegisterFunction(duckrouting::GetBdDijkstraCostMatrixFunction());
	loader.RegisterFunction(duckrouting::GetBdAStarFunction());
	loader.RegisterFunction(duckrouting::GetBdAStarCostFunction());
	loader.RegisterFunction(duckrouting::GetBdAStarCostMatrixFunction());
	loader.RegisterFunction(duckrouting::GetEdwardMooreFunction());
	loader.RegisterFunction(duckrouting::GetBinaryBreadthFirstSearchFunction());
	loader.RegisterFunction(duckrouting::GetExtractVerticesFunction());
	loader.RegisterFunction(duckrouting::GetDegreeFunction());
	loader.RegisterFunction(duckrouting::GetFullVersionFunction());

	loader.RegisterFunction(duckrouting::GetWithPointsFunction());
	loader.RegisterFunction(duckrouting::GetWithPointsCostFunction());
	loader.RegisterFunction(duckrouting::GetWithPointsCostMatrixFunction());
	loader.RegisterFunction(duckrouting::GetWithPointsViaFunction());
	loader.RegisterFunction(duckrouting::GetWithPointsKspFunction());

	loader.RegisterFunction(duckrouting::GetContractionFunction());
	loader.RegisterFunction(duckrouting::GetDeadEndContractionFunction());
	loader.RegisterFunction(duckrouting::GetLinearContractionFunction());

	loader.RegisterFunction(duckrouting::GetLineGraphFunction());
	loader.RegisterFunction(duckrouting::GetLineGraphFullFunction());
	loader.RegisterFunction(duckrouting::GetChinesePostmanFunction());
	loader.RegisterFunction(duckrouting::GetChinesePostmanCostFunction());

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
