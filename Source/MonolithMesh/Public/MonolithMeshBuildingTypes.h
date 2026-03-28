#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * SP1: Building Descriptor types — the contract consumed by ALL downstream sub-projects (SP2-SP10).
 * These structs define room grids, doors, stairwells, floor plans, and the complete building descriptor.
 */

/** A single room definition for the grid */
struct FRoomDef
{
	FString RoomId;                // "kitchen", "bedroom_1", etc.
	FString RoomType;              // "kitchen", "bedroom", "bathroom", "corridor", "lobby", etc.
	TArray<FIntPoint> GridCells;   // Which cells this room occupies

	// Computed at build time:
	FBox WorldBounds = FBox(ForceInit);
	FBox LocalBounds = FBox(ForceInit);

	TSharedPtr<FJsonObject> ToJson() const
	{
		auto J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("room_id"), RoomId);
		J->SetStringField(TEXT("room_type"), RoomType);

		TArray<TSharedPtr<FJsonValue>> CellsArr;
		for (const FIntPoint& C : GridCells)
		{
			TArray<TSharedPtr<FJsonValue>> CellPair;
			CellPair.Add(MakeShared<FJsonValueNumber>(C.X));
			CellPair.Add(MakeShared<FJsonValueNumber>(C.Y));
			CellsArr.Add(MakeShared<FJsonValueArray>(CellPair));
		}
		J->SetArrayField(TEXT("grid_cells"), CellsArr);

		if (WorldBounds.IsValid)
		{
			auto WB = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> MinArr, MaxArr;
			MinArr.Add(MakeShared<FJsonValueNumber>(WorldBounds.Min.X));
			MinArr.Add(MakeShared<FJsonValueNumber>(WorldBounds.Min.Y));
			MinArr.Add(MakeShared<FJsonValueNumber>(WorldBounds.Min.Z));
			MaxArr.Add(MakeShared<FJsonValueNumber>(WorldBounds.Max.X));
			MaxArr.Add(MakeShared<FJsonValueNumber>(WorldBounds.Max.Y));
			MaxArr.Add(MakeShared<FJsonValueNumber>(WorldBounds.Max.Z));
			WB->SetArrayField(TEXT("min"), MinArr);
			WB->SetArrayField(TEXT("max"), MaxArr);
			J->SetObjectField(TEXT("world_bounds"), WB);
		}

		if (LocalBounds.IsValid)
		{
			auto LB = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> MinArr, MaxArr;
			MinArr.Add(MakeShared<FJsonValueNumber>(LocalBounds.Min.X));
			MinArr.Add(MakeShared<FJsonValueNumber>(LocalBounds.Min.Y));
			MinArr.Add(MakeShared<FJsonValueNumber>(LocalBounds.Min.Z));
			MaxArr.Add(MakeShared<FJsonValueNumber>(LocalBounds.Max.X));
			MaxArr.Add(MakeShared<FJsonValueNumber>(LocalBounds.Max.Y));
			MaxArr.Add(MakeShared<FJsonValueNumber>(LocalBounds.Max.Z));
			LB->SetArrayField(TEXT("min"), MinArr);
			LB->SetArrayField(TEXT("max"), MaxArr);
			J->SetObjectField(TEXT("local_bounds"), LB);
		}

		return J;
	}
};

/** A door between two rooms */
struct FDoorDef
{
	FString DoorId;
	FString RoomA;             // Room ID on one side
	FString RoomB;             // Room ID on other side
	FIntPoint EdgeStart;       // Grid edge start cell
	FIntPoint EdgeEnd;         // Grid edge end cell (for multi-cell doors)
	FString Wall;              // "north", "south", "east", "west" (auto-computed)
	float Width = 90.0f;
	float Height = 220.0f;
	bool bTraversable = true;

	// Computed at build time:
	FVector WorldPosition = FVector::ZeroVector;

	TSharedPtr<FJsonObject> ToJson() const
	{
		auto J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("door_id"), DoorId);

		TArray<TSharedPtr<FJsonValue>> Connects;
		Connects.Add(MakeShared<FJsonValueString>(RoomA));
		Connects.Add(MakeShared<FJsonValueString>(RoomB));
		J->SetArrayField(TEXT("connects"), Connects);

		TArray<TSharedPtr<FJsonValue>> WPos;
		WPos.Add(MakeShared<FJsonValueNumber>(WorldPosition.X));
		WPos.Add(MakeShared<FJsonValueNumber>(WorldPosition.Y));
		WPos.Add(MakeShared<FJsonValueNumber>(WorldPosition.Z));
		J->SetArrayField(TEXT("world_position"), WPos);

		J->SetStringField(TEXT("wall"), Wall);
		J->SetNumberField(TEXT("width"), Width);
		J->SetNumberField(TEXT("height"), Height);
		J->SetBoolField(TEXT("traversable"), bTraversable);
		return J;
	}
};

/** Stairwell connecting floors */
struct FStairwellDef
{
	FString StairwellId;
	TArray<FIntPoint> GridCells;    // Cells that are stairwell (suppress floor/ceiling)
	int32 ConnectsFloorA = 0;
	int32 ConnectsFloorB = 1;
	FVector WorldPosition = FVector::ZeroVector;

	TSharedPtr<FJsonObject> ToJson() const
	{
		auto J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("stairwell_id"), StairwellId);

		TArray<TSharedPtr<FJsonValue>> CellsArr;
		for (const FIntPoint& C : GridCells)
		{
			TArray<TSharedPtr<FJsonValue>> Pair;
			Pair.Add(MakeShared<FJsonValueNumber>(C.X));
			Pair.Add(MakeShared<FJsonValueNumber>(C.Y));
			CellsArr.Add(MakeShared<FJsonValueArray>(Pair));
		}
		J->SetArrayField(TEXT("grid_cells"), CellsArr);

		TArray<TSharedPtr<FJsonValue>> Floors;
		Floors.Add(MakeShared<FJsonValueNumber>(ConnectsFloorA));
		Floors.Add(MakeShared<FJsonValueNumber>(ConnectsFloorB));
		J->SetArrayField(TEXT("connects_floors"), Floors);

		TArray<TSharedPtr<FJsonValue>> WPos;
		WPos.Add(MakeShared<FJsonValueNumber>(WorldPosition.X));
		WPos.Add(MakeShared<FJsonValueNumber>(WorldPosition.Y));
		WPos.Add(MakeShared<FJsonValueNumber>(WorldPosition.Z));
		J->SetArrayField(TEXT("world_position"), WPos);

		return J;
	}
};

/** Exterior face descriptor — consumed by SP3 (Facades) */
struct FExteriorFaceDef
{
	FString Wall;            // "north", "south", "east", "west"
	int32 FloorIndex = 0;
	FVector WorldOrigin = FVector::ZeroVector;
	FVector Normal = FVector::ZeroVector;
	float Width = 0.0f;
	float Height = 0.0f;

	TSharedPtr<FJsonObject> ToJson() const
	{
		auto J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("wall"), Wall);
		J->SetNumberField(TEXT("floor_index"), FloorIndex);

		TArray<TSharedPtr<FJsonValue>> Origin, Norm;
		Origin.Add(MakeShared<FJsonValueNumber>(WorldOrigin.X));
		Origin.Add(MakeShared<FJsonValueNumber>(WorldOrigin.Y));
		Origin.Add(MakeShared<FJsonValueNumber>(WorldOrigin.Z));
		J->SetArrayField(TEXT("world_origin"), Origin);

		Norm.Add(MakeShared<FJsonValueNumber>(Normal.X));
		Norm.Add(MakeShared<FJsonValueNumber>(Normal.Y));
		Norm.Add(MakeShared<FJsonValueNumber>(Normal.Z));
		J->SetArrayField(TEXT("normal"), Norm);

		J->SetNumberField(TEXT("width"), Width);
		J->SetNumberField(TEXT("height"), Height);
		J->SetBoolField(TEXT("is_exterior"), true);
		return J;
	}
};

/** A complete floor plan (one floor) */
struct FFloorPlan
{
	int32 FloorIndex = 0;
	float ZOffset = 0.0f;
	float Height = 270.0f;
	TArray<TArray<int32>> Grid;    // 2D grid of room indices (-1 = empty, -2 = stairwell)
	TArray<FRoomDef> Rooms;
	TArray<FDoorDef> Doors;
	TArray<FStairwellDef> Stairwells;

	TSharedPtr<FJsonObject> ToJson() const
	{
		auto J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("floor_index"), FloorIndex);
		J->SetNumberField(TEXT("z_offset"), ZOffset);
		J->SetNumberField(TEXT("height"), Height);

		TArray<TSharedPtr<FJsonValue>> RoomArr;
		for (const FRoomDef& R : Rooms)
		{
			RoomArr.Add(MakeShared<FJsonValueObject>(R.ToJson()));
		}
		J->SetArrayField(TEXT("rooms"), RoomArr);

		TArray<TSharedPtr<FJsonValue>> DoorArr;
		for (const FDoorDef& D : Doors)
		{
			DoorArr.Add(MakeShared<FJsonValueObject>(D.ToJson()));
		}
		J->SetArrayField(TEXT("doors"), DoorArr);

		TArray<TSharedPtr<FJsonValue>> StairArr;
		for (const FStairwellDef& S : Stairwells)
		{
			StairArr.Add(MakeShared<FJsonValueObject>(S.ToJson()));
		}
		J->SetArrayField(TEXT("stairwells"), StairArr);

		return J;
	}
};

/** The Building Descriptor — output of create_building_from_grid, consumed by all downstream SPs */
struct FBuildingDescriptor
{
	FString BuildingId;
	FString AssetPath;
	FVector WorldOrigin = FVector::ZeroVector;
	float GridCellSize = 50.0f;
	float ExteriorWallThickness = 15.0f;
	float InteriorWallThickness = 10.0f;
	TArray<FFloorPlan> Floors;
	TArray<FVector2D> FootprintPolygon;    // Outer footprint for roof generation
	TArray<FExteriorFaceDef> ExteriorFaces;
	TArray<FString> ActorNames;
	TArray<FString> TagsApplied;

	TSharedPtr<FJsonObject> ToJson() const
	{
		auto J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("building_id"), BuildingId);
		J->SetStringField(TEXT("asset_path"), AssetPath);

		TArray<TSharedPtr<FJsonValue>> OriginArr;
		OriginArr.Add(MakeShared<FJsonValueNumber>(WorldOrigin.X));
		OriginArr.Add(MakeShared<FJsonValueNumber>(WorldOrigin.Y));
		OriginArr.Add(MakeShared<FJsonValueNumber>(WorldOrigin.Z));
		J->SetArrayField(TEXT("world_origin"), OriginArr);

		// Actors
		TArray<TSharedPtr<FJsonValue>> ActorsArr;
		for (const FString& AN : ActorNames)
		{
			auto AJ = MakeShared<FJsonObject>();
			AJ->SetStringField(TEXT("actor_name"), AN);
			ActorsArr.Add(MakeShared<FJsonValueObject>(AJ));
		}
		J->SetArrayField(TEXT("actors"), ActorsArr);

		// Footprint polygon
		TArray<TSharedPtr<FJsonValue>> FootArr;
		for (const FVector2D& V : FootprintPolygon)
		{
			TArray<TSharedPtr<FJsonValue>> Pt;
			Pt.Add(MakeShared<FJsonValueNumber>(V.X));
			Pt.Add(MakeShared<FJsonValueNumber>(V.Y));
			FootArr.Add(MakeShared<FJsonValueArray>(Pt));
		}
		J->SetArrayField(TEXT("footprint_polygon"), FootArr);

		// Floors
		TArray<TSharedPtr<FJsonValue>> FloorArr;
		for (const FFloorPlan& F : Floors)
		{
			FloorArr.Add(MakeShared<FJsonValueObject>(F.ToJson()));
		}
		J->SetArrayField(TEXT("floors"), FloorArr);

		// Exterior faces
		TArray<TSharedPtr<FJsonValue>> FaceArr;
		for (const FExteriorFaceDef& F : ExteriorFaces)
		{
			FaceArr.Add(MakeShared<FJsonValueObject>(F.ToJson()));
		}
		J->SetArrayField(TEXT("exterior_faces"), FaceArr);

		// Tags
		TArray<TSharedPtr<FJsonValue>> TagArr;
		for (const FString& T : TagsApplied)
		{
			TagArr.Add(MakeShared<FJsonValueString>(T));
		}
		J->SetArrayField(TEXT("tags_applied"), TagArr);

		// Config
		J->SetNumberField(TEXT("grid_cell_size"), GridCellSize);
		auto WallThick = MakeShared<FJsonObject>();
		WallThick->SetNumberField(TEXT("exterior"), ExteriorWallThickness);
		WallThick->SetNumberField(TEXT("interior"), InteriorWallThickness);
		J->SetObjectField(TEXT("wall_thickness"), WallThick);

		return J;
	}

	static FBuildingDescriptor FromJson(const TSharedPtr<FJsonObject>& Json)
	{
		FBuildingDescriptor D;
		if (!Json.IsValid()) return D;

		Json->TryGetStringField(TEXT("building_id"), D.BuildingId);
		Json->TryGetStringField(TEXT("asset_path"), D.AssetPath);

		if (Json->HasField(TEXT("grid_cell_size")))
			D.GridCellSize = static_cast<float>(Json->GetNumberField(TEXT("grid_cell_size")));

		const TSharedPtr<FJsonObject>* WallObj = nullptr;
		if (Json->TryGetObjectField(TEXT("wall_thickness"), WallObj) && WallObj && (*WallObj).IsValid())
		{
			if ((*WallObj)->HasField(TEXT("exterior")))
				D.ExteriorWallThickness = static_cast<float>((*WallObj)->GetNumberField(TEXT("exterior")));
			if ((*WallObj)->HasField(TEXT("interior")))
				D.InteriorWallThickness = static_cast<float>((*WallObj)->GetNumberField(TEXT("interior")));
		}

		return D;
	}
};
