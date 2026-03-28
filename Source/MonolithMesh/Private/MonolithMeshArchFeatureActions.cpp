#if WITH_GEOMETRYSCRIPT

#include "MonolithMeshArchFeatureActions.h"
#include "MonolithMeshProceduralActions.h"
#include "MonolithMeshHandlePool.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"
#include "MonolithAssetUtils.h"

#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"

#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"

#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Editor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using namespace UE::Geometry;

static const FString GS_ERROR_ARCH = TEXT("Enable the GeometryScripting plugin in your .uproject to use architectural feature generation.");

UMonolithMeshHandlePool* FMonolithMeshArchFeatureActions::Pool = nullptr;

void FMonolithMeshArchFeatureActions::SetHandlePool(UMonolithMeshHandlePool* InPool)
{
	Pool = InPool;
}

// ============================================================================
// Helpers
// ============================================================================

float FMonolithMeshArchFeatureActions::GetFloat(const TSharedPtr<FJsonObject>& P, const FString& Key, float Default)
{
	return P->HasField(Key) ? static_cast<float>(P->GetNumberField(Key)) : Default;
}

int32 FMonolithMeshArchFeatureActions::GetInt(const TSharedPtr<FJsonObject>& P, const FString& Key, int32 Default)
{
	return P->HasField(Key) ? static_cast<int32>(P->GetNumberField(Key)) : Default;
}

FString FMonolithMeshArchFeatureActions::GetString(const TSharedPtr<FJsonObject>& P, const FString& Key, const FString& Default)
{
	FString Val;
	return P->TryGetStringField(Key, Val) ? Val : Default;
}

bool FMonolithMeshArchFeatureActions::GetBool(const TSharedPtr<FJsonObject>& P, const FString& Key, bool Default)
{
	return P->HasField(Key) ? P->GetBoolField(Key) : Default;
}

void FMonolithMeshArchFeatureActions::FinalizeGeometry(UDynamicMesh* Mesh)
{
	if (!Mesh) return;

	// Box UV projection
	{
		FGeometryScriptMeshSelection EmptySelection;
		FTransform UVBox = FTransform::Identity;
		UVBox.SetScale3D(FVector(100.0f));
		UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
			Mesh, 0, UVBox, EmptySelection, 2);
	}

	// Compute normals (additive-only geometry, no booleans)
	FMonolithMeshProceduralActions::CleanupMesh(Mesh, /*bHadBooleans=*/false);
}

FString FMonolithMeshArchFeatureActions::SaveAndPlace(UDynamicMesh* Mesh, const TSharedPtr<FJsonObject>& Params,
	const TSharedPtr<FJsonObject>& Result)
{
	FString SavePath;
	Params->TryGetStringField(TEXT("save_path"), SavePath);
	if (SavePath.IsEmpty())
	{
		return TEXT("save_path is required");
	}

	bool bOverwrite = Params->HasField(TEXT("overwrite")) ? Params->GetBoolField(TEXT("overwrite")) : false;
	FString SaveErr;
	if (!FMonolithMeshProceduralActions::SaveMeshToAsset(Mesh, SavePath, bOverwrite, SaveErr))
	{
		int32 TriCount = Mesh->GetTriangleCount();
		return FString::Printf(TEXT("Mesh generated (%d tris) but save failed: %s"), TriCount, *SaveErr);
	}
	Result->SetStringField(TEXT("asset_path"), SavePath);

	// Place in scene
	FVector Location = FVector::ZeroVector;
	MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location);

	float YawDeg = GetFloat(Params, TEXT("rotation"), 0.0f);
	FRotator Rotation(0.0f, YawDeg, 0.0f);

	FString Label;
	Params->TryGetStringField(TEXT("label"), Label);

	FString Folder;
	Params->TryGetStringField(TEXT("folder"), Folder);
	if (Folder.IsEmpty())
	{
		Folder = TEXT("Procedural/ArchFeatures");
	}

	AActor* Actor = FMonolithMeshProceduralActions::PlaceMeshInScene(
		SavePath, Location, Rotation, Label, /*bSnapToFloor=*/false, Folder);
	if (Actor)
	{
		Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	}

	return FString(); // success
}

// ============================================================================
// Railing Builder (shared by balcony, porch, fire escape, ramp, create_railing)
// ============================================================================

void FMonolithMeshArchFeatureActions::BuildRailingGeometry(UDynamicMesh* Mesh, const TArray<FVector>& Path,
	float Height, const FString& Style, float PostSpacing, float PostWidth,
	float RailWidth, float BarSpacing, float BarWidth, float PanelThickness,
	bool bClosedLoop)
{
	if (Path.Num() < 2) return;

	FGeometryScriptPrimitiveOptions Opts;
	const float TopRailThickness = RailWidth;

	// Build segment list (pairs of points)
	int32 SegCount = bClosedLoop ? Path.Num() : Path.Num() - 1;

	for (int32 Seg = 0; Seg < SegCount; ++Seg)
	{
		const FVector& A = Path[Seg];
		const FVector& B = Path[(Seg + 1) % Path.Num()];
		FVector Dir = B - A;
		float SegLen = Dir.Size();
		if (SegLen < 1.0f) continue;
		Dir /= SegLen;

		// Compute rotation from X-axis to segment direction
		FRotator SegRot = Dir.Rotation();

		// Top rail along this segment
		FVector RailCenter = (A + B) * 0.5f + FVector(0, 0, Height - TopRailThickness * 0.5f);
		FTransform RailXf(SegRot, RailCenter, FVector::OneVector);
		// AppendBox: DimX = along segment direction, DimY = width, DimZ = height
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			Mesh, Opts, RailXf, SegLen, TopRailThickness, TopRailThickness, 0, 0, 0,
			EGeometryScriptPrimitiveOriginMode::Center);

		// Posts along this segment
		int32 NumPosts = FMath::Max(2, FMath::CeilToInt32(SegLen / PostSpacing) + 1);
		for (int32 Pi = 0; Pi < NumPosts; ++Pi)
		{
			float T = (NumPosts > 1) ? static_cast<float>(Pi) / static_cast<float>(NumPosts - 1) : 0.0f;
			FVector PostBase = FMath::Lerp(A, B, T);
			FTransform PostXf(FRotator::ZeroRotator, PostBase, FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, PostXf, PostWidth, PostWidth, Height, 0, 0, 0,
				EGeometryScriptPrimitiveOriginMode::Base);
		}

		// Style-specific infill
		if (Style == TEXT("bars"))
		{
			// Vertical bars between posts
			float FillHeight = Height - TopRailThickness;
			int32 NumBars = FMath::Max(0, FMath::FloorToInt32(SegLen / BarSpacing) - 1);
			for (int32 Bi = 1; Bi <= NumBars; ++Bi)
			{
				float BT = static_cast<float>(Bi) * BarSpacing / SegLen;
				if (BT >= 1.0f) break;
				FVector BarBase = FMath::Lerp(A, B, BT);
				FTransform BarXf(FRotator::ZeroRotator, BarBase, FVector::OneVector);
				UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
					Mesh, Opts, BarXf, BarWidth, BarWidth, FillHeight, 0, 0, 0,
					EGeometryScriptPrimitiveOriginMode::Base);
			}
		}
		else if (Style == TEXT("solid"))
		{
			// Solid panel
			float PanelHeight = Height - TopRailThickness;
			FVector PanelCenter = (A + B) * 0.5f + FVector(0, 0, PanelHeight * 0.5f);
			FTransform PanelXf(SegRot, PanelCenter, FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, PanelXf, SegLen, PanelThickness, PanelHeight, 0, 0, 0,
				EGeometryScriptPrimitiveOriginMode::Center);
		}
		// "simple" style = posts + top rail only (no infill)
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshArchFeatureActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// ---- create_balcony ----
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_balcony"),
		TEXT("Generate a balcony: floor slab + railing extending from an upper floor wall face. "
			"Styles: simple (posts + top rail), bars (vertical balusters), solid (solid panel)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshArchFeatureActions::CreateBalcony),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path (e.g. /Game/Town/SM_Balcony_01)"))
			.Required(TEXT("width"), TEXT("number"), TEXT("Balcony width along the wall (cm)"))
			.Required(TEXT("depth"), TEXT("number"), TEXT("How far it extends from the wall (cm)"))
			.Optional(TEXT("floor_thickness"), TEXT("number"), TEXT("Slab thickness (cm)"), TEXT("15"))
			.Optional(TEXT("railing_height"), TEXT("number"), TEXT("Railing height (cm)"), TEXT("100"))
			.Optional(TEXT("railing_style"), TEXT("string"), TEXT("simple, bars, or solid"), TEXT("bars"))
			.Optional(TEXT("bar_spacing"), TEXT("number"), TEXT("Space between railing bars (cm)"), TEXT("12"))
			.Optional(TEXT("bar_diameter"), TEXT("number"), TEXT("Bar thickness (cm)"), TEXT("2"))
			.Optional(TEXT("has_floor_drain"), TEXT("boolean"), TEXT("Slight slope for drainage"), TEXT("true"))
			.Optional(TEXT("material_slab"), TEXT("string"), TEXT("Material for slab (slot 0)"))
			.Optional(TEXT("material_railing"), TEXT("string"), TEXT("Material for railing (slot 1)"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x,y,z] — bottom of slab at wall face"))
			.Optional(TEXT("rotation"), TEXT("number"), TEXT("Yaw rotation in degrees"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder path"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Overwrite existing asset"), TEXT("false"))
			.Build());

	// ---- create_porch ----
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_porch"),
		TEXT("Generate a covered porch: floor platform, support columns, roof slab, and optional entry steps with railings. "
			"Configurable column count, step geometry, roof overhang."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshArchFeatureActions::CreatePorch),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path (e.g. /Game/Town/SM_Porch_01)"))
			.Required(TEXT("width"), TEXT("number"), TEXT("Porch width (cm)"))
			.Required(TEXT("depth"), TEXT("number"), TEXT("Porch depth (cm)"))
			.Optional(TEXT("height"), TEXT("number"), TEXT("Porch roof height from ground (cm)"), TEXT("270"))
			.Optional(TEXT("column_count"), TEXT("number"), TEXT("Number of support columns"), TEXT("2"))
			.Optional(TEXT("column_diameter"), TEXT("number"), TEXT("Column diameter (cm)"), TEXT("20"))
			.Optional(TEXT("has_roof"), TEXT("boolean"), TEXT("Generate roof slab"), TEXT("true"))
			.Optional(TEXT("roof_overhang"), TEXT("number"), TEXT("Roof extends beyond columns (cm)"), TEXT("15"))
			.Optional(TEXT("has_steps"), TEXT("boolean"), TEXT("Generate entry steps"), TEXT("true"))
			.Optional(TEXT("step_count"), TEXT("number"), TEXT("Number of steps (auto from height if 0)"), TEXT("3"))
			.Optional(TEXT("step_depth"), TEXT("number"), TEXT("Each step depth (cm)"), TEXT("30"))
			.Optional(TEXT("step_height"), TEXT("number"), TEXT("Each step height (cm)"), TEXT("18"))
			.Optional(TEXT("railing_height"), TEXT("number"), TEXT("Step railing height (0 = no railing)"), TEXT("90"))
			.Optional(TEXT("material_floor"), TEXT("string"), TEXT("Floor material (slot 0)"))
			.Optional(TEXT("material_column"), TEXT("string"), TEXT("Column material (slot 1)"))
			.Optional(TEXT("material_roof"), TEXT("string"), TEXT("Roof material (slot 2)"))
			.Optional(TEXT("material_steps"), TEXT("string"), TEXT("Steps material (slot 3)"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x,y,z]"))
			.Optional(TEXT("rotation"), TEXT("number"), TEXT("Yaw rotation in degrees"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder path"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Overwrite existing asset"), TEXT("false"))
			.Build());

	// ---- create_fire_escape ----
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_fire_escape"),
		TEXT("Generate a multi-story fire escape: zigzag exterior stairs between floor landings. "
			"Each floor gets a landing platform. Stairs alternate left/right. Optional roof ladder."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshArchFeatureActions::CreateFireEscape),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path (e.g. /Game/Town/SM_FireEscape_01)"))
			.Required(TEXT("floor_count"), TEXT("number"), TEXT("Number of floors to span"))
			.Required(TEXT("floor_height"), TEXT("number"), TEXT("Height per floor (cm)"))
			.Optional(TEXT("landing_width"), TEXT("number"), TEXT("Landing platform width (cm)"), TEXT("150"))
			.Optional(TEXT("landing_depth"), TEXT("number"), TEXT("Landing platform depth (cm)"), TEXT("120"))
			.Optional(TEXT("stair_width"), TEXT("number"), TEXT("Stair run width (cm)"), TEXT("90"))
			.Optional(TEXT("railing_height"), TEXT("number"), TEXT("Railing height (cm)"), TEXT("100"))
			.Optional(TEXT("has_ladder"), TEXT("boolean"), TEXT("Top floor ladder to roof"), TEXT("true"))
			.Optional(TEXT("ladder_height"), TEXT("number"), TEXT("Ladder extension above top landing (cm)"), TEXT("150"))
			.Optional(TEXT("material_platform"), TEXT("string"), TEXT("Platform material (slot 0)"))
			.Optional(TEXT("material_railing"), TEXT("string"), TEXT("Railing material (slot 1)"))
			.Optional(TEXT("material_stairs"), TEXT("string"), TEXT("Stairs material (slot 2)"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x,y,z] — base attachment point"))
			.Optional(TEXT("rotation"), TEXT("number"), TEXT("Yaw rotation in degrees"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder path"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Overwrite existing asset"), TEXT("false"))
			.Build());

	// ---- create_ramp_connector ----
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_ramp_connector"),
		TEXT("Generate an ADA-compliant ramp between two heights. Auto-computes run length from rise and slope ratio. "
			"Adds intermediate switchback landings if rise exceeds max_rise_per_run."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshArchFeatureActions::CreateRampConnector),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path (e.g. /Game/Town/SM_Ramp_01)"))
			.Required(TEXT("rise"), TEXT("number"), TEXT("Total height change (cm)"))
			.Optional(TEXT("width"), TEXT("number"), TEXT("Ramp width (cm)"), TEXT("120"))
			.Optional(TEXT("slope_ratio"), TEXT("number"), TEXT("Rise-to-run ratio (1/12 for ADA)"), TEXT("0.0833"))
			.Optional(TEXT("max_rise_per_run"), TEXT("number"), TEXT("Max rise before landing (cm, 76 for ADA)"), TEXT("76"))
			.Optional(TEXT("landing_length"), TEXT("number"), TEXT("Landing pad length (cm)"), TEXT("150"))
			.Optional(TEXT("railing_height"), TEXT("number"), TEXT("Railing height (cm)"), TEXT("90"))
			.Optional(TEXT("railing_style"), TEXT("string"), TEXT("simple or bars"), TEXT("simple"))
			.Optional(TEXT("material_ramp"), TEXT("string"), TEXT("Ramp material (slot 0)"))
			.Optional(TEXT("material_railing"), TEXT("string"), TEXT("Railing material (slot 1)"))
			.Optional(TEXT("material_landing"), TEXT("string"), TEXT("Landing material (slot 2)"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x,y,z] — base of ramp"))
			.Optional(TEXT("rotation"), TEXT("number"), TEXT("Yaw rotation in degrees"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder path"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Overwrite existing asset"), TEXT("false"))
			.Build());

	// ---- create_railing ----
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_railing"),
		TEXT("Generate a railing along an arbitrary path defined by 3D points. "
			"Styles: simple (posts + top rail), bars (+ vertical balusters), solid (+ panel infill)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshArchFeatureActions::CreateRailing),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path (e.g. /Game/Town/SM_Railing_01)"))
			.Required(TEXT("points"), TEXT("array"), TEXT("Array of [x,y,z] points defining the railing path"))
			.Optional(TEXT("height"), TEXT("number"), TEXT("Railing height (cm)"), TEXT("100"))
			.Optional(TEXT("style"), TEXT("string"), TEXT("simple, bars, or solid"), TEXT("bars"))
			.Optional(TEXT("post_spacing"), TEXT("number"), TEXT("Distance between posts (cm)"), TEXT("120"))
			.Optional(TEXT("post_width"), TEXT("number"), TEXT("Post cross-section (cm)"), TEXT("4"))
			.Optional(TEXT("rail_width"), TEXT("number"), TEXT("Top rail cross-section (cm)"), TEXT("5"))
			.Optional(TEXT("bar_spacing"), TEXT("number"), TEXT("Vertical bar spacing for bars style (cm)"), TEXT("12"))
			.Optional(TEXT("bar_width"), TEXT("number"), TEXT("Bar cross-section (cm)"), TEXT("2"))
			.Optional(TEXT("panel_thickness"), TEXT("number"), TEXT("Panel thickness for solid style (cm)"), TEXT("3"))
			.Optional(TEXT("closed_loop"), TEXT("boolean"), TEXT("Connect last point to first"), TEXT("false"))
			.Optional(TEXT("material"), TEXT("string"), TEXT("Material path (slot 0)"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x,y,z]"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder path"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Overwrite existing asset"), TEXT("false"))
			.Build());
}

// ============================================================================
// create_balcony
// ============================================================================

FMonolithActionResult FMonolithMeshArchFeatureActions::CreateBalcony(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool) return FMonolithActionResult::Error(GS_ERROR_ARCH);

	// Required params
	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));

	float Width = 0.0f, Depth = 0.0f;
	if (!Params->HasField(TEXT("width")))
		return FMonolithActionResult::Error(TEXT("Missing required param: width"));
	if (!Params->HasField(TEXT("depth")))
		return FMonolithActionResult::Error(TEXT("Missing required param: depth"));

	Width = static_cast<float>(Params->GetNumberField(TEXT("width")));
	Depth = static_cast<float>(Params->GetNumberField(TEXT("depth")));

	if (Width <= 0.0f || Depth <= 0.0f)
		return FMonolithActionResult::Error(TEXT("width and depth must be positive"));

	// Optional params
	const float FloorThick   = GetFloat(Params, TEXT("floor_thickness"), 15.0f);
	const float RailHeight   = GetFloat(Params, TEXT("railing_height"), 100.0f);
	const FString RailStyle  = GetString(Params, TEXT("railing_style"), TEXT("bars")).ToLower();
	const float BarSpacing   = GetFloat(Params, TEXT("bar_spacing"), 12.0f);
	const float BarDiam      = GetFloat(Params, TEXT("bar_diameter"), 2.0f);
	const bool bFloorDrain   = GetBool(Params, TEXT("has_floor_drain"), true);

	// Validate railing style
	if (RailStyle != TEXT("simple") && RailStyle != TEXT("bars") && RailStyle != TEXT("solid"))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid railing_style '%s'. Valid: simple, bars, solid"), *RailStyle));

	// Create mesh
	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh) return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));

	FGeometryScriptPrimitiveOptions Opts;

	// ---- Floor slab ----
	// Slab sits at Z=0 (bottom), extends in +Y direction (away from wall)
	// Origin mode Base: Z=0 is bottom of the box
	{
		FVector SlabCenter(0.0f, Depth * 0.5f, 0.0f);
		FTransform SlabXf(FRotator::ZeroRotator, SlabCenter, FVector::OneVector);

		float SlabThick = FloorThick;
		if (bFloorDrain)
		{
			// Slight slope: make the slab slightly thinner at the outer edge.
			// Since AppendBox can't do tapered geometry, we just use the slab as-is.
			// The "drain" is represented by a tiny wedge — for blockout fidelity, flat slab is fine.
			SlabThick = FloorThick;
		}

		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			Mesh, Opts, SlabXf, Width, Depth, SlabThick, 0, 0, 0,
			EGeometryScriptPrimitiveOriginMode::Center);
	}

	// ---- Railing ----
	// Three-sided railing: left edge, front edge, right edge (not against wall)
	if (RailHeight > 0.0f)
	{
		const float HalfW = Width * 0.5f;
		const float RailZ = FloorThick * 0.5f; // top of slab

		TArray<FVector> RailPath;
		RailPath.Add(FVector(-HalfW, 0.0f, RailZ));        // left-wall corner
		RailPath.Add(FVector(-HalfW, Depth, RailZ));        // left-front corner
		RailPath.Add(FVector(HalfW, Depth, RailZ));          // right-front corner
		RailPath.Add(FVector(HalfW, 0.0f, RailZ));          // right-wall corner

		const float PostWidth = 4.0f;
		const float RailWidth = 5.0f;
		const float PanelThick = 3.0f;

		BuildRailingGeometry(Mesh, RailPath, RailHeight, RailStyle,
			/*PostSpacing=*/FMath::Max(BarSpacing * 10.0f, 100.0f), PostWidth, RailWidth,
			BarSpacing, BarDiam, PanelThick, /*bClosedLoop=*/false);
	}

	// Finalize
	FinalizeGeometry(Mesh);

	int32 TriCount = Mesh->GetTriangleCount();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), TEXT("balcony"));
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("depth"), Depth);
	Result->SetStringField(TEXT("railing_style"), RailStyle);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);

	FString FinalErr = SaveAndPlace(Mesh, Params, Result);
	if (!FinalErr.IsEmpty())
		return FMonolithActionResult::Error(FinalErr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// create_porch
// ============================================================================

FMonolithActionResult FMonolithMeshArchFeatureActions::CreatePorch(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool) return FMonolithActionResult::Error(GS_ERROR_ARCH);

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));

	if (!Params->HasField(TEXT("width")))
		return FMonolithActionResult::Error(TEXT("Missing required param: width"));
	if (!Params->HasField(TEXT("depth")))
		return FMonolithActionResult::Error(TEXT("Missing required param: depth"));

	const float Width   = static_cast<float>(Params->GetNumberField(TEXT("width")));
	const float Depth   = static_cast<float>(Params->GetNumberField(TEXT("depth")));

	if (Width <= 0.0f || Depth <= 0.0f)
		return FMonolithActionResult::Error(TEXT("width and depth must be positive"));

	const float Height     = GetFloat(Params, TEXT("height"), 270.0f);
	const int32 ColCount   = GetInt(Params, TEXT("column_count"), 2);
	const float ColDiam    = GetFloat(Params, TEXT("column_diameter"), 20.0f);
	const bool bHasRoof    = GetBool(Params, TEXT("has_roof"), true);
	const float RoofOver   = GetFloat(Params, TEXT("roof_overhang"), 15.0f);
	const bool bHasSteps   = GetBool(Params, TEXT("has_steps"), true);
	const float StepDepth  = GetFloat(Params, TEXT("step_depth"), 30.0f);
	const float StepHeight = GetFloat(Params, TEXT("step_height"), 18.0f);
	const float RailHeight = GetFloat(Params, TEXT("railing_height"), 90.0f);

	// Floor height: if steps are present, porch floor = step_count * step_height
	int32 StepCount = GetInt(Params, TEXT("step_count"), 3);
	if (StepCount <= 0 && bHasSteps)
	{
		// Auto-compute step count from a reasonable porch floor height
		float PorchFloorH = 54.0f; // 3 steps * 18cm default
		StepCount = FMath::Max(1, FMath::RoundToInt32(PorchFloorH / StepHeight));
	}

	const float PorchFloorZ = bHasSteps ? (StepCount * StepHeight) : 0.0f;
	const float FloorThick = 10.0f;

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh) return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));

	FGeometryScriptPrimitiveOptions Opts;

	// ---- Floor platform ----
	{
		FVector FloorPos(0.0f, Depth * 0.5f, PorchFloorZ);
		FTransform FloorXf(FRotator::ZeroRotator, FloorPos, FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			Mesh, Opts, FloorXf, Width, Depth, FloorThick, 0, 0, 0,
			EGeometryScriptPrimitiveOriginMode::Center);
	}

	// ---- Columns ----
	if (ColCount > 0)
	{
		const float ColRadius = ColDiam * 0.5f;
		const float ColHeight = Height - PorchFloorZ - FloorThick * 0.5f;
		const float ColZ = PorchFloorZ + FloorThick * 0.5f;

		// Distribute columns evenly along the front edge
		for (int32 Ci = 0; Ci < ColCount; ++Ci)
		{
			float T = (ColCount > 1) ? static_cast<float>(Ci) / static_cast<float>(ColCount - 1) : 0.5f;
			float X = FMath::Lerp(-Width * 0.5f + ColRadius + 5.0f, Width * 0.5f - ColRadius - 5.0f, T);

			FTransform ColXf(FRotator::ZeroRotator, FVector(X, Depth - ColRadius - 5.0f, ColZ), FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
				Mesh, Opts, ColXf, ColRadius, ColHeight, 12, 0, true,
				EGeometryScriptPrimitiveOriginMode::Base);
		}
	}

	// ---- Roof ----
	if (bHasRoof)
	{
		const float RoofThick = 8.0f;
		const float RoofW = Width + RoofOver * 2.0f;
		const float RoofD = Depth + RoofOver * 2.0f;

		FVector RoofPos(0.0f, Depth * 0.5f, Height - RoofThick * 0.5f);
		FTransform RoofXf(FRotator::ZeroRotator, RoofPos, FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			Mesh, Opts, RoofXf, RoofW, RoofD, RoofThick, 0, 0, 0,
			EGeometryScriptPrimitiveOriginMode::Center);
	}

	// ---- Steps ----
	if (bHasSteps && StepCount > 0)
	{
		for (int32 Si = 0; Si < StepCount; ++Si)
		{
			float SZ = Si * StepHeight;
			float SY = -(Si + 1) * StepDepth + StepDepth * 0.5f; // steps extend in -Y (towards viewer)

			// Each step is full-width
			FVector StepPos(0.0f, Depth + (Si + 1) * StepDepth - StepDepth * 0.5f, SZ + StepHeight * 0.5f);
			// Actually, steps descend from the porch front edge outward (in +Y direction from origin)
			// Recalculate: step 0 is the lowest (farthest from porch), step N-1 is at porch floor
			float StepZ = (Si) * StepHeight;
			float StepY = Depth + (StepCount - Si) * StepDepth - StepDepth * 0.5f;

			FTransform StepXf(FRotator::ZeroRotator, FVector(0.0f, StepY, StepZ + StepHeight * 0.5f), FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, StepXf, Width, StepDepth, StepHeight, 0, 0, 0,
				EGeometryScriptPrimitiveOriginMode::Center);
		}

		// ---- Step railings ----
		if (RailHeight > 0.0f)
		{
			const float HalfW = Width * 0.5f;
			const float TotalStepRun = StepCount * StepDepth;

			// Left side railing (following the step diagonal)
			for (int32 Side = 0; Side < 2; ++Side)
			{
				float X = (Side == 0) ? -HalfW : HalfW;
				TArray<FVector> StepRailPath;

				// Bottom of stairs
				StepRailPath.Add(FVector(X, Depth + TotalStepRun, 0.0f));
				// Top of stairs (porch floor level)
				StepRailPath.Add(FVector(X, Depth, PorchFloorZ));

				BuildRailingGeometry(Mesh, StepRailPath, RailHeight, TEXT("simple"),
					/*PostSpacing=*/100.0f, /*PostWidth=*/4.0f, /*RailWidth=*/5.0f,
					/*BarSpacing=*/12.0f, /*BarWidth=*/2.0f, /*PanelThick=*/3.0f,
					/*bClosedLoop=*/false);
			}
		}
	}

	// Finalize
	FinalizeGeometry(Mesh);

	int32 TriCount = Mesh->GetTriangleCount();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), TEXT("porch"));
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("depth"), Depth);
	Result->SetNumberField(TEXT("height"), Height);
	Result->SetNumberField(TEXT("column_count"), ColCount);
	Result->SetNumberField(TEXT("step_count"), StepCount);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);

	FString FinalErr = SaveAndPlace(Mesh, Params, Result);
	if (!FinalErr.IsEmpty())
		return FMonolithActionResult::Error(FinalErr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// create_fire_escape
// ============================================================================

FMonolithActionResult FMonolithMeshArchFeatureActions::CreateFireEscape(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool) return FMonolithActionResult::Error(GS_ERROR_ARCH);

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));

	if (!Params->HasField(TEXT("floor_count")))
		return FMonolithActionResult::Error(TEXT("Missing required param: floor_count"));
	if (!Params->HasField(TEXT("floor_height")))
		return FMonolithActionResult::Error(TEXT("Missing required param: floor_height"));

	const int32 FloorCount   = GetInt(Params, TEXT("floor_count"), 2);
	const float FloorHeight  = static_cast<float>(Params->GetNumberField(TEXT("floor_height")));

	if (FloorCount < 1) return FMonolithActionResult::Error(TEXT("floor_count must be >= 1"));
	if (FloorHeight <= 0.0f) return FMonolithActionResult::Error(TEXT("floor_height must be positive"));

	const float LandingW    = GetFloat(Params, TEXT("landing_width"), 150.0f);
	const float LandingD    = GetFloat(Params, TEXT("landing_depth"), 120.0f);
	const float StairW      = GetFloat(Params, TEXT("stair_width"), 90.0f);
	const float RailHeight  = GetFloat(Params, TEXT("railing_height"), 100.0f);
	const bool bHasLadder   = GetBool(Params, TEXT("has_ladder"), true);
	const float LadderH     = GetFloat(Params, TEXT("ladder_height"), 150.0f);

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh) return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));

	FGeometryScriptPrimitiveOptions Opts;
	const float PlatformThick = 5.0f;
	const float TreadThick = 3.0f;
	const float StringerW = 4.0f;

	// Stair geometry constants
	const float StairStepH = 20.0f;
	const int32 StepsPerFlight = FMath::Max(2, FMath::RoundToInt32(FloorHeight / StairStepH));
	const float ActualStepH = FloorHeight / static_cast<float>(StepsPerFlight);
	const float StepDepth = LandingD / static_cast<float>(StepsPerFlight); // fit stairs within landing depth

	// Fire escape layout: landings at each floor, stairs zigzag between them.
	// Landings are stacked vertically. Stairs alternate offset in X.
	// Landing 0 = bottom floor at Z = FloorHeight (first floor level).
	// Landing N-1 = top floor.

	for (int32 Fi = 0; Fi < FloorCount; ++Fi)
	{
		float LandingZ = static_cast<float>(Fi + 1) * FloorHeight;

		// ---- Landing platform ----
		{
			FTransform PlatXf(FRotator::ZeroRotator,
				FVector(0.0f, LandingD * 0.5f, LandingZ),
				FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, PlatXf, LandingW, LandingD, PlatformThick, 0, 0, 0,
				EGeometryScriptPrimitiveOriginMode::Center);
		}

		// ---- Landing railing (three sides: left, front, right) ----
		if (RailHeight > 0.0f)
		{
			const float HalfW = LandingW * 0.5f;
			const float RailZ = LandingZ + PlatformThick * 0.5f;

			TArray<FVector> RailPath;
			RailPath.Add(FVector(-HalfW, 0.0f, RailZ));
			RailPath.Add(FVector(-HalfW, LandingD, RailZ));
			RailPath.Add(FVector(HalfW, LandingD, RailZ));
			RailPath.Add(FVector(HalfW, 0.0f, RailZ));

			BuildRailingGeometry(Mesh, RailPath, RailHeight, TEXT("bars"),
				/*PostSpacing=*/100.0f, /*PostWidth=*/3.0f, /*RailWidth=*/4.0f,
				/*BarSpacing=*/12.0f, /*BarWidth=*/2.0f, /*PanelThick=*/3.0f,
				/*bClosedLoop=*/false);
		}

		// ---- Stairs down to the floor below (skip for bottom floor — that one has stairs to ground) ----
		if (Fi > 0)
		{
			// Zigzag: even floors have stairs on the left side (-X), odd on the right (+X)
			float StairOffsetX = (Fi % 2 == 0) ? -(LandingW * 0.5f + StairW * 0.5f) : (LandingW * 0.5f + StairW * 0.5f);

			float BottomZ = static_cast<float>(Fi) * FloorHeight + PlatformThick * 0.5f;
			float TopZ = LandingZ - PlatformThick * 0.5f;

			// Stair treads between this landing and the one below
			for (int32 Si = 0; Si < StepsPerFlight; ++Si)
			{
				float T = static_cast<float>(Si) / static_cast<float>(StepsPerFlight);
				float TreadZ = FMath::Lerp(BottomZ, TopZ, T);
				float TreadY = LandingD * T;

				FTransform TreadXf(FRotator::ZeroRotator,
					FVector(StairOffsetX, TreadY, TreadZ),
					FVector::OneVector);
				UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
					Mesh, Opts, TreadXf, StairW, StepDepth, TreadThick, 0, 0, 0,
					EGeometryScriptPrimitiveOriginMode::Center);
			}

			// Stringer (left and right side beams of the staircase)
			{
				float StringerLen = FMath::Sqrt(FMath::Square(LandingD) + FMath::Square(TopZ - BottomZ));
				FVector StringerDir = FVector(0.0f, LandingD, TopZ - BottomZ);
				StringerDir.Normalize();
				FRotator StringerRot = StringerDir.Rotation();
				FVector StringerMid(StairOffsetX, LandingD * 0.5f, (BottomZ + TopZ) * 0.5f);

				for (int32 Side = 0; Side < 2; ++Side)
				{
					float SX = StairOffsetX + (Side == 0 ? -StairW * 0.5f : StairW * 0.5f);
					FTransform StrXf(StringerRot, FVector(SX, LandingD * 0.5f, (BottomZ + TopZ) * 0.5f), FVector::OneVector);
					UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
						Mesh, Opts, StrXf, StringerLen, StringerW, StringerW, 0, 0, 0,
						EGeometryScriptPrimitiveOriginMode::Center);
				}
			}

			// Stair railing
			if (RailHeight > 0.0f)
			{
				for (int32 Side = 0; Side < 2; ++Side)
				{
					float SX = StairOffsetX + (Side == 0 ? -StairW * 0.5f : StairW * 0.5f);
					TArray<FVector> StairRailPath;
					StairRailPath.Add(FVector(SX, 0.0f, BottomZ));
					StairRailPath.Add(FVector(SX, LandingD, TopZ));

					BuildRailingGeometry(Mesh, StairRailPath, RailHeight, TEXT("bars"),
						/*PostSpacing=*/80.0f, /*PostWidth=*/3.0f, /*RailWidth=*/4.0f,
						/*BarSpacing=*/12.0f, /*BarWidth=*/2.0f, /*PanelThick=*/3.0f,
						/*bClosedLoop=*/false);
				}
			}
		}
	}

	// ---- Ground-level stairs (from first landing down to ground) ----
	{
		float TopZ = FloorHeight - PlatformThick * 0.5f;
		float BottomZ = 0.0f;
		float StairOffsetX = -(LandingW * 0.5f + StairW * 0.5f); // always left for ground stairs

		for (int32 Si = 0; Si < StepsPerFlight; ++Si)
		{
			float T = static_cast<float>(Si) / static_cast<float>(StepsPerFlight);
			float TreadZ = FMath::Lerp(BottomZ, TopZ, T);
			float TreadY = LandingD * T;

			FTransform TreadXf(FRotator::ZeroRotator,
				FVector(StairOffsetX, TreadY, TreadZ),
				FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, TreadXf, StairW, StepDepth, TreadThick, 0, 0, 0,
				EGeometryScriptPrimitiveOriginMode::Center);
		}

		// Stringers
		{
			float StringerLen = FMath::Sqrt(FMath::Square(LandingD) + FMath::Square(TopZ - BottomZ));
			FVector StringerDir = FVector(0.0f, LandingD, TopZ - BottomZ);
			StringerDir.Normalize();
			FRotator StringerRot = StringerDir.Rotation();

			for (int32 Side = 0; Side < 2; ++Side)
			{
				float SX = StairOffsetX + (Side == 0 ? -StairW * 0.5f : StairW * 0.5f);
				FTransform StrXf(StringerRot, FVector(SX, LandingD * 0.5f, (BottomZ + TopZ) * 0.5f), FVector::OneVector);
				UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
					Mesh, Opts, StrXf, StringerLen, StringerW, StringerW, 0, 0, 0,
					EGeometryScriptPrimitiveOriginMode::Center);
			}
		}

		// Ground stair railing
		if (RailHeight > 0.0f)
		{
			for (int32 Side = 0; Side < 2; ++Side)
			{
				float SX = StairOffsetX + (Side == 0 ? -StairW * 0.5f : StairW * 0.5f);
				TArray<FVector> StairRailPath;
				StairRailPath.Add(FVector(SX, 0.0f, BottomZ));
				StairRailPath.Add(FVector(SX, LandingD, TopZ));

				BuildRailingGeometry(Mesh, StairRailPath, RailHeight, TEXT("bars"),
					/*PostSpacing=*/80.0f, /*PostWidth=*/3.0f, /*RailWidth=*/4.0f,
					/*BarSpacing=*/12.0f, /*BarWidth=*/2.0f, /*PanelThick=*/3.0f,
					/*bClosedLoop=*/false);
			}
		}
	}

	// ---- Roof ladder ----
	if (bHasLadder && FloorCount > 0)
	{
		float TopLandingZ = static_cast<float>(FloorCount) * FloorHeight + PlatformThick * 0.5f;
		float LadderW = 40.0f; // ladder width
		float RungSpacing = 30.0f;
		float RungDiam = 2.5f;
		float RailDiam = 3.0f;

		int32 RungCount = FMath::Max(2, FMath::FloorToInt32(LadderH / RungSpacing));

		// Side rails
		for (int32 Side = 0; Side < 2; ++Side)
		{
			float X = (Side == 0) ? -LadderW * 0.5f : LadderW * 0.5f;
			FTransform RailXf(FRotator::ZeroRotator,
				FVector(X, LandingD * 0.5f, TopLandingZ),
				FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, RailXf, RailDiam, RailDiam, LadderH, 0, 0, 0,
				EGeometryScriptPrimitiveOriginMode::Base);
		}

		// Rungs
		for (int32 Ri = 0; Ri < RungCount; ++Ri)
		{
			float RungZ = TopLandingZ + RungSpacing * (Ri + 1);
			if (RungZ > TopLandingZ + LadderH) break;

			FTransform RungXf(FRotator(0, 0, 90), FVector(0.0f, LandingD * 0.5f, RungZ), FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, RungXf, LadderW, RungDiam, RungDiam, 0, 0, 0,
				EGeometryScriptPrimitiveOriginMode::Center);
		}
	}

	// Finalize
	FinalizeGeometry(Mesh);

	int32 TriCount = Mesh->GetTriangleCount();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), TEXT("fire_escape"));
	Result->SetNumberField(TEXT("floor_count"), FloorCount);
	Result->SetNumberField(TEXT("floor_height"), FloorHeight);
	Result->SetNumberField(TEXT("landing_width"), LandingW);
	Result->SetNumberField(TEXT("landing_depth"), LandingD);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);
	Result->SetBoolField(TEXT("has_ladder"), bHasLadder);

	FString FinalErr = SaveAndPlace(Mesh, Params, Result);
	if (!FinalErr.IsEmpty())
		return FMonolithActionResult::Error(FinalErr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// create_ramp_connector
// ============================================================================

FMonolithActionResult FMonolithMeshArchFeatureActions::CreateRampConnector(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool) return FMonolithActionResult::Error(GS_ERROR_ARCH);

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));

	if (!Params->HasField(TEXT("rise")))
		return FMonolithActionResult::Error(TEXT("Missing required param: rise"));

	const float Rise = static_cast<float>(Params->GetNumberField(TEXT("rise")));
	if (Rise <= 0.0f) return FMonolithActionResult::Error(TEXT("rise must be positive"));

	const float Width         = GetFloat(Params, TEXT("width"), 120.0f);
	const float SlopeRatio    = GetFloat(Params, TEXT("slope_ratio"), 1.0f / 12.0f);
	const float MaxRisePerRun = GetFloat(Params, TEXT("max_rise_per_run"), 76.0f);
	const float LandingLen    = GetFloat(Params, TEXT("landing_length"), 150.0f);
	const float RailHeight    = GetFloat(Params, TEXT("railing_height"), 90.0f);
	const FString RailStyle   = GetString(Params, TEXT("railing_style"), TEXT("simple")).ToLower();

	if (SlopeRatio <= 0.0f || SlopeRatio > 1.0f)
		return FMonolithActionResult::Error(TEXT("slope_ratio must be between 0 and 1 (exclusive)"));

	// Compute number of ramp segments
	int32 NumSegments = FMath::Max(1, FMath::CeilToInt32(Rise / MaxRisePerRun));
	float RisePerSeg = Rise / static_cast<float>(NumSegments);
	float RunPerSeg = RisePerSeg / SlopeRatio;

	const float RampThick = 10.0f;
	const float LandingThick = 10.0f;

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh) return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));

	FGeometryScriptPrimitiveOptions Opts;

	// Layout: ramp segments go in +Y direction, alternating between +Y and -Y for switchbacks.
	// Segment 0 starts at origin (0,0,0) going in +Y.
	// If switchback: segment 1 starts at (0, RunPerSeg, RisePerSeg) and goes in -Y direction,
	// connected by a landing.

	// Bottom landing
	{
		FTransform LandXf(FRotator::ZeroRotator, FVector(0.0f, -LandingLen * 0.5f, LandingThick * 0.5f), FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			Mesh, Opts, LandXf, Width, LandingLen, LandingThick, 0, 0, 0,
			EGeometryScriptPrimitiveOriginMode::Center);
	}

	float CurrentZ = 0.0f;
	float CurrentY = 0.0f;
	int32 Direction = 1; // +1 = going in +Y, -1 = going in -Y

	for (int32 Seg = 0; Seg < NumSegments; ++Seg)
	{
		float SegStartZ = CurrentZ;
		float SegEndZ = CurrentZ + RisePerSeg;
		float SegStartY = CurrentY;
		float SegEndY = CurrentY + Direction * RunPerSeg;

		// Ramp surface: a tilted box
		float RampLen = FMath::Sqrt(FMath::Square(RunPerSeg) + FMath::Square(RisePerSeg));
		float Angle = FMath::Atan2(RisePerSeg, RunPerSeg);
		float AngleDeg = FMath::RadiansToDegrees(Angle);

		// Pitch: rotation around right axis (X). Positive pitch = nose up.
		// When going in +Y, we pitch up. When going in -Y, we pitch down.
		float PitchDeg = (Direction > 0) ? -AngleDeg : AngleDeg;

		FVector RampCenter(0.0f, (SegStartY + SegEndY) * 0.5f, (SegStartZ + SegEndZ) * 0.5f);
		FRotator RampRot(PitchDeg, (Direction > 0) ? 0.0f : 180.0f, 0.0f);
		FTransform RampXf(RampRot, RampCenter, FVector::OneVector);

		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			Mesh, Opts, RampXf, Width, RampLen, RampThick, 0, 0, 0,
			EGeometryScriptPrimitiveOriginMode::Center);

		// Railings on both sides of the ramp
		if (RailHeight > 0.0f)
		{
			const float HalfW = Width * 0.5f;
			for (int32 Side = 0; Side < 2; ++Side)
			{
				float X = (Side == 0) ? -HalfW : HalfW;
				TArray<FVector> RailPath;
				RailPath.Add(FVector(X, SegStartY, SegStartZ + RampThick * 0.5f));
				RailPath.Add(FVector(X, SegEndY, SegEndZ + RampThick * 0.5f));

				BuildRailingGeometry(Mesh, RailPath, RailHeight, RailStyle,
					/*PostSpacing=*/100.0f, /*PostWidth=*/4.0f, /*RailWidth=*/5.0f,
					/*BarSpacing=*/12.0f, /*BarWidth=*/2.0f, /*PanelThick=*/3.0f,
					/*bClosedLoop=*/false);
			}
		}

		CurrentZ = SegEndZ;
		CurrentY = SegEndY;

		// Intermediate landing (between segments, not after the last one)
		if (Seg < NumSegments - 1)
		{
			FTransform LandXf(FRotator::ZeroRotator,
				FVector(0.0f, CurrentY, CurrentZ),
				FVector::OneVector);
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
				Mesh, Opts, LandXf, Width, LandingLen, LandingThick, 0, 0, 0,
				EGeometryScriptPrimitiveOriginMode::Center);

			// Railings around the landing (three sides, not the side connecting to ramps)
			if (RailHeight > 0.0f)
			{
				const float HalfW = Width * 0.5f;
				const float HalfL = LandingLen * 0.5f;
				float LandZ = CurrentZ + LandingThick * 0.5f;

				// Front edge (perpendicular to ramp direction)
				float FrontY = CurrentY + Direction * HalfL;
				TArray<FVector> FrontRail;
				FrontRail.Add(FVector(-HalfW, FrontY, LandZ));
				FrontRail.Add(FVector(HalfW, FrontY, LandZ));

				BuildRailingGeometry(Mesh, FrontRail, RailHeight, RailStyle,
					/*PostSpacing=*/100.0f, /*PostWidth=*/4.0f, /*RailWidth=*/5.0f,
					/*BarSpacing=*/12.0f, /*BarWidth=*/2.0f, /*PanelThick=*/3.0f,
					/*bClosedLoop=*/false);
			}

			// Switch direction for next segment
			Direction = -Direction;
		}
	}

	// Top landing
	{
		FTransform LandXf(FRotator::ZeroRotator,
			FVector(0.0f, CurrentY, CurrentZ),
			FVector::OneVector);
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			Mesh, Opts, LandXf, Width, LandingLen, LandingThick, 0, 0, 0,
			EGeometryScriptPrimitiveOriginMode::Center);
	}

	// Finalize
	FinalizeGeometry(Mesh);

	int32 TriCount = Mesh->GetTriangleCount();

	// Compute total run length
	float TotalRun = RunPerSeg * NumSegments;
	float TotalLandingLength = (NumSegments > 1) ? LandingLen * (NumSegments - 1) : 0.0f;

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), TEXT("ramp_connector"));
	Result->SetNumberField(TEXT("rise"), Rise);
	Result->SetNumberField(TEXT("total_run"), TotalRun);
	Result->SetNumberField(TEXT("segments"), NumSegments);
	Result->SetNumberField(TEXT("slope_ratio"), SlopeRatio);
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("total_length"), TotalRun + TotalLandingLength + LandingLen * 2.0f);
	Result->SetBoolField(TEXT("ada_compliant"), SlopeRatio <= (1.0f / 12.0f) + 0.001f);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);

	FString FinalErr = SaveAndPlace(Mesh, Params, Result);
	if (!FinalErr.IsEmpty())
		return FMonolithActionResult::Error(FinalErr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// create_railing
// ============================================================================

FMonolithActionResult FMonolithMeshArchFeatureActions::CreateRailing(const TSharedPtr<FJsonObject>& Params)
{
	if (!Pool) return FMonolithActionResult::Error(GS_ERROR_ARCH);

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));

	// Parse points array
	const TArray<TSharedPtr<FJsonValue>>* PointsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("points"), PointsArr) || !PointsArr || PointsArr->Num() < 2)
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'points' array (need at least 2 points as [x,y,z])"));

	TArray<FVector> Points;
	Points.Reserve(PointsArr->Num());
	for (int32 Pi = 0; Pi < PointsArr->Num(); ++Pi)
	{
		const TArray<TSharedPtr<FJsonValue>>* PtArr = nullptr;
		if (!(*PointsArr)[Pi]->TryGetArray(PtArr) || !PtArr || PtArr->Num() < 3)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Point %d must be an array of [x,y,z]"), Pi));
		}
		FVector Pt(
			(*PtArr)[0]->AsNumber(),
			(*PtArr)[1]->AsNumber(),
			(*PtArr)[2]->AsNumber()
		);
		Points.Add(Pt);
	}

	const float Height       = GetFloat(Params, TEXT("height"), 100.0f);
	const FString Style      = GetString(Params, TEXT("style"), TEXT("bars")).ToLower();
	const float PostSpacing  = GetFloat(Params, TEXT("post_spacing"), 120.0f);
	const float PostWidth    = GetFloat(Params, TEXT("post_width"), 4.0f);
	const float RailWidth    = GetFloat(Params, TEXT("rail_width"), 5.0f);
	const float BarSpacing   = GetFloat(Params, TEXT("bar_spacing"), 12.0f);
	const float BarWidth     = GetFloat(Params, TEXT("bar_width"), 2.0f);
	const float PanelThick   = GetFloat(Params, TEXT("panel_thickness"), 3.0f);
	const bool bClosedLoop   = GetBool(Params, TEXT("closed_loop"), false);

	if (Style != TEXT("simple") && Style != TEXT("bars") && Style != TEXT("solid"))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid style '%s'. Valid: simple, bars, solid"), *Style));

	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(Pool);
	if (!Mesh) return FMonolithActionResult::Error(TEXT("Failed to allocate UDynamicMesh"));

	BuildRailingGeometry(Mesh, Points, Height, Style, PostSpacing, PostWidth, RailWidth,
		BarSpacing, BarWidth, PanelThick, bClosedLoop);

	// Finalize
	FinalizeGeometry(Mesh);

	int32 TriCount = Mesh->GetTriangleCount();

	// Compute total path length
	float PathLen = 0.0f;
	for (int32 Pi = 1; Pi < Points.Num(); ++Pi)
	{
		PathLen += FVector::Dist(Points[Pi - 1], Points[Pi]);
	}
	if (bClosedLoop && Points.Num() > 1)
	{
		PathLen += FVector::Dist(Points.Last(), Points[0]);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), TEXT("railing"));
	Result->SetStringField(TEXT("style"), Style);
	Result->SetNumberField(TEXT("point_count"), Points.Num());
	Result->SetNumberField(TEXT("path_length"), PathLen);
	Result->SetNumberField(TEXT("height"), Height);
	Result->SetBoolField(TEXT("closed_loop"), bClosedLoop);
	Result->SetNumberField(TEXT("triangle_count"), TriCount);

	FString FinalErr = SaveAndPlace(Mesh, Params, Result);
	if (!FinalErr.IsEmpty())
		return FMonolithActionResult::Error(FinalErr);

	return FMonolithActionResult::Success(Result);
}

#endif // WITH_GEOMETRYSCRIPT
