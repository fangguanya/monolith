#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * SP9: Daredevil Debug View — 6 actions for inspecting procedural buildings.
 *
 * toggle_section_view      — Hide actors above a Z height for section-cut inspection
 * toggle_ceiling_visibility — Show/hide actors tagged BuildingCeiling/BuildingRoof
 * capture_floor_plan       — Orthographic top-down scene capture to PNG
 * highlight_room           — Spawn translucent overlay box at room bounds
 * save_camera_bookmark     — Save editor viewport camera to JSON
 * load_camera_bookmark     — Restore editor viewport camera from JSON
 */
class FMonolithMeshDebugViewActions
{
public:
	/** Register all 6 debug view actions */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// ---- Action Handlers ----

	static FMonolithActionResult ToggleSectionView(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ToggleCeilingVisibility(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CaptureFloorPlan(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HighlightRoom(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SaveCameraBookmark(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult LoadCameraBookmark(const TSharedPtr<FJsonObject>& Params);

	// ---- Helpers ----

	/** Get the bookmarks save directory */
	static FString GetBookmarkDirectory();

	/** Get the captures save directory */
	static FString GetCapturesDirectory();

	/** Ensure a directory exists */
	static bool EnsureDirectory(const FString& Path);
};
