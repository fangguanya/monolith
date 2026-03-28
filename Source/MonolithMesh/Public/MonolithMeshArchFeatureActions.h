#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

#if WITH_GEOMETRYSCRIPT

class UMonolithMeshHandlePool;
class UDynamicMesh;

/**
 * SP8b: Architectural Feature Actions
 * Standalone procedural geometry for balconies, porches, fire escapes, ADA ramps, and railings.
 * All actions produce static mesh assets via GeometryScript primitives (AppendBox/AppendCylinder).
 * 5 actions: create_balcony, create_porch, create_fire_escape, create_ramp_connector, create_railing.
 */
class FMonolithMeshArchFeatureActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);
	static void SetHandlePool(UMonolithMeshHandlePool* InPool);

private:
	static UMonolithMeshHandlePool* Pool;

	// Action handlers
	static FMonolithActionResult CreateBalcony(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreatePorch(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateFireEscape(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateRampConnector(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateRailing(const TSharedPtr<FJsonObject>& Params);

	// ---- Internal geometry builders ----

	/** Build railing geometry along an edge. Posts + top rail + style-dependent infill.
	 *  Path is an array of 3D points defining the railing baseline.
	 *  All geometry is appended to Mesh in-place. */
	static void BuildRailingGeometry(UDynamicMesh* Mesh, const TArray<FVector>& Path,
		float Height, const FString& Style, float PostSpacing, float PostWidth,
		float RailWidth, float BarSpacing, float BarWidth, float PanelThickness,
		bool bClosedLoop);

	/** Apply box UV projection + compute split normals (standard finalization for additive geometry) */
	static void FinalizeGeometry(UDynamicMesh* Mesh);

	/** Save mesh to asset, optionally place in scene. Uses public helpers from FMonolithMeshProceduralActions.
	 *  Populates Result with save_path, actor_name, etc. Returns empty string on success, error message on failure. */
	static FString SaveAndPlace(UDynamicMesh* Mesh, const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Result);

	/** Helper: parse a float from Params, returning Default if absent */
	static float GetFloat(const TSharedPtr<FJsonObject>& P, const FString& Key, float Default);

	/** Helper: parse an int from Params, returning Default if absent */
	static int32 GetInt(const TSharedPtr<FJsonObject>& P, const FString& Key, int32 Default);

	/** Helper: parse a string from Params, returning Default if absent */
	static FString GetString(const TSharedPtr<FJsonObject>& P, const FString& Key, const FString& Default);

	/** Helper: parse a bool from Params, returning Default if absent */
	static bool GetBool(const TSharedPtr<FJsonObject>& P, const FString& Key, bool Default);
};

#endif // WITH_GEOMETRYSCRIPT
