#include "MonolithPerfActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY(LogMonolithPerf);

// ==================== FPerfLogCapture (module-scoped ring buffer) ====================

FPerfLogCapture* FPerfLogCapture::SingletonInstance = nullptr;

FPerfLogCapture::FPerfLogCapture()
{
	RingBuffer.SetNum(MaxEntries);
}

FPerfLogCapture::~FPerfLogCapture()
{
	Uninstall();
}

FPerfLogCapture* FPerfLogCapture::Get()
{
	return SingletonInstance;
}

void FPerfLogCapture::Install()
{
	FScopeLock L(&Lock);
	if (bInstalled) return;
	if (GLog) { GLog->AddOutputDevice(this); }
	bInstalled = true;
	SingletonInstance = this;
}

void FPerfLogCapture::Uninstall()
{
	FScopeLock L(&Lock);
	if (!bInstalled) return;
	if (GLog) { GLog->RemoveOutputDevice(this); }
	bInstalled = false;
	if (SingletonInstance == this) SingletonInstance = nullptr;
}

void FPerfLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock L(&Lock);
	FEntry& E = RingBuffer[WriteIndex];
	E.Timestamp = FPlatformTime::Seconds();
	E.Category = Category;
	E.Verbosity = Verbosity;
	E.Message = V;
	WriteIndex = (WriteIndex + 1) % MaxEntries;
	if (WriteIndex == 0) bWrapped = true;
}

TArray<FPerfLogCapture::FEntry> FPerfLogCapture::GetEntriesSince(double SinceTs, const TArray<FName>& CategoryFilter, int32 Limit) const
{
	FScopeLock L(&Lock);
	TArray<FEntry> Out;
	Out.Reserve(FMath::Min(Limit, MaxEntries));
	const int32 Count = bWrapped ? MaxEntries : WriteIndex;
	for (int32 i = 0; i < Count; ++i)
	{
		const int32 Idx = (WriteIndex + MaxEntries - Count + i) % MaxEntries;
		const FEntry& E = RingBuffer[Idx];
		if (E.Timestamp < SinceTs) continue;
		if (CategoryFilter.Num() > 0 && !CategoryFilter.Contains(E.Category)) continue;
		Out.Add(E);
		if (Out.Num() >= Limit) break;
	}
	return Out;
}

// ==================== PIE helper ====================

bool IsPerfPIEActive()
{
	return GEditor && GEditor->PlayWorld != nullptr;
}

#define PERF_REQUIRE_PIE() \
	if (!IsPerfPIEActive()) { return FMonolithActionResult::Error(TEXT("API requires PIE running. Call pie_query.start first.")); }

// ==================== FPerfLogScope ====================

FPerfLogScope::FPerfLogScope() {}

FPerfLogScope::~FPerfLogScope()
{
	End();
}

void FPerfLogScope::Begin()
{
	FScopeLock L(&Lock);
	Entries.Reset();
	bActive = true;
	if (GLog)
	{
		GLog->AddOutputDevice(this);
	}
}

void FPerfLogScope::End()
{
	FScopeLock L(&Lock);
	if (bActive && GLog)
	{
		GLog->RemoveOutputDevice(this);
		bActive = false;
	}
}

void FPerfLogScope::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock L(&Lock);
	if (!bActive) return;
	FEntry E;
	E.Timestamp = FPlatformTime::Seconds();
	E.Category = Category;
	E.Verbosity = Verbosity;
	E.Message = V;
	Entries.Add(MoveTemp(E));
}

TArray<FPerfLogScope::FEntry> FPerfLogScope::GetEntries() const
{
	FScopeLock L(&Lock);
	return Entries;
}

// ==================== Helpers ====================

// Run a console command and capture log output emitted during the call + wait window.
// Returns the captured lines as JSON array.
static TArray<TSharedPtr<FJsonValue>> ExecCommandCapture(const FString& Command, int32 WaitMs, const FName& CategoryFilter = NAME_None, int32 Limit = 500)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	if (!GEngine) return Out;

	FPerfLogScope Scope;
	Scope.Begin();
	GEngine->Exec(nullptr, *Command);
	if (WaitMs > 0)
	{
		FPlatformProcess::Sleep(WaitMs * 0.001f);
	}
	Scope.End();

	TArray<FPerfLogScope::FEntry> Entries = Scope.GetEntries();
	for (const FPerfLogScope::FEntry& E : Entries)
	{
		if (!CategoryFilter.IsNone() && E.Category != CategoryFilter) continue;
		if (Out.Num() >= Limit) break;
		Out.Add(MakeShared<FJsonValueString>(E.Message));
	}
	return Out;
}

// Find newest matching file in Dir + one level of sub-dirs (memreport nests).
static FString FindNewestMatchingFile(const FString& Dir, const FString& Pattern, const FDateTime& Since)
{
	IFileManager& FM = IFileManager::Get();
	FString BestPath;
	FDateTime BestTime = FDateTime::MinValue();

	auto ScanDir = [&](const FString& InDir)
	{
		TArray<FString> Names;
		FM.FindFiles(Names, *(InDir / Pattern), true, false);
		for (const FString& Name : Names)
		{
			const FString Full = InDir / Name;
			const FDateTime Mt = FM.GetTimeStamp(*Full);
			if (Mt >= Since && Mt > BestTime) { BestTime = Mt; BestPath = Full; }
		}
	};

	ScanDir(Dir);
	TArray<FString> SubDirs;
	FM.FindFiles(SubDirs, *(Dir / TEXT("*")), false, true);
	for (const FString& Sub : SubDirs) { ScanDir(Dir / Sub); }
	return BestPath;
}

static TSharedPtr<FJsonObject> MakeError(const FString& Msg)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), false);
	Root->SetStringField(TEXT("error"), Msg);
	return Root;
}

// ==================== Action Registration ====================

void FMonolithPerfActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	const TCHAR* NS = TEXT("perf");

	// --- Tier P0: Core ---
	Registry.RegisterAction(NS, TEXT("time_between"),
		TEXT("Measure time (ms) between first log line matching pattern_a and first subsequent line matching pattern_b."),
		FMonolithActionHandler::CreateStatic(&HandleTimeBetween),
		FParamSchemaBuilder()
			.Required(TEXT("pattern_a"), TEXT("string"), TEXT("Regex for start marker"))
			.Required(TEXT("pattern_b"), TEXT("string"), TEXT("Regex for end marker (after pattern_a)"))
			.Optional(TEXT("since"), TEXT("number"), TEXT("Only consider entries with Timestamp >= this"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Log category filter"))
			.Build());

	Registry.RegisterAction(NS, TEXT("run_commandlet"),
		TEXT("Run a UE commandlet in subprocess. Returns exit_code, stdout."),
		FMonolithActionHandler::CreateStatic(&HandleRunCommandlet),
		FParamSchemaBuilder()
			.Required(TEXT("commandlet"), TEXT("string"), TEXT("Commandlet name"))
			.Optional(TEXT("args"), TEXT("array"), TEXT("Extra commandlet args"))
			.Optional(TEXT("timeout_sec"), TEXT("integer"), TEXT(""), TEXT("300"))
			.Optional(TEXT("capture_output"), TEXT("bool"), TEXT(""), TEXT("true"))
			.Build());

	Registry.RegisterAction(NS, TEXT("execute_console_command"),
		TEXT("Run any console cmd via GEngine->Exec, capture log output during wait_ms. Generic primitive for probing CVars/stats not yet in dedicated APIs."),
		FMonolithActionHandler::CreateStatic(&HandleExecuteConsoleCommand),
		FParamSchemaBuilder()
			.Required(TEXT("command"), TEXT("string"), TEXT("Console command"))
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("500"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Category filter"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT(""), TEXT("500"))
			.Build());

	Registry.RegisterAction(NS, TEXT("dump_shader_compile_stats"),
		TEXT("Send DumpShaderCompileStats + scrape LogShaderCompilers. Returns pending_permutations etc."),
		FMonolithActionHandler::CreateStatic(&HandleDumpShaderCompileStats),
		FParamSchemaBuilder()
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("500"))
			.Build());

	Registry.RegisterAction(NS, TEXT("start_trace"),
		TEXT("Begin Unreal Insights .utrace capture via Trace.Start <channels>."),
		FMonolithActionHandler::CreateStatic(&HandleStartTrace),
		FParamSchemaBuilder()
			.Optional(TEXT("channels"), TEXT("string"), TEXT("Comma-separated channels"))
			.Optional(TEXT("session_name"), TEXT("string"), TEXT("Session name (auto-timestamped if empty)"))
			.Build());

	Registry.RegisterAction(NS, TEXT("stop_trace"),
		TEXT("Finalize active trace via Trace.Stop. Returns .utrace path."),
		FMonolithActionHandler::CreateStatic(&HandleStopTrace),
		FParamSchemaBuilder()
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("2000"))
			.Build());

	Registry.RegisterAction(NS, TEXT("capture_stats_session"),
		TEXT("stat startfile + sleep + stat stopfile. Returns .uestats path."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureStatsSession),
		FParamSchemaBuilder()
			.Required(TEXT("duration_sec"), TEXT("number"), TEXT("Duration in seconds (0,300]"))
			.Optional(TEXT("filename"), TEXT("string"), TEXT("Artifact filename (auto-timestamped if empty)"))
			.Build());

	Registry.RegisterAction(NS, TEXT("capture_csv_profile"),
		TEXT("CsvProfile start + sleep + stop. Returns .csv path. Requires PIE."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureCsvProfile),
		FParamSchemaBuilder()
			.Required(TEXT("duration_sec"), TEXT("number"), TEXT("Duration in seconds (0,300]"))
			.Optional(TEXT("filename"), TEXT("string"), TEXT("Artifact filename (auto-timestamped if empty)"))
			.Build());

	Registry.RegisterAction(NS, TEXT("capture_memreport"),
		TEXT("memreport -full + parse header (physical/virtual/peak MB)."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureMemreport),
		FParamSchemaBuilder()
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("2000"))
			.Optional(TEXT("top_n"), TEXT("integer"), TEXT(""), TEXT("20"))
			.Build());

	// --- Tier P1: Diagnostic ---
	Registry.RegisterAction(NS, TEXT("get_frame_stats"),
		TEXT("Current frame timing snapshot via `stat unit` parsing. Returns frame_ms, game_ms, draw_ms, gpu_ms, rhi_ms if detectable."),
		FMonolithActionHandler::CreateStatic(&HandleGetFrameStats),
		FParamSchemaBuilder()
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("500"))
			.Build());

	Registry.RegisterAction(NS, TEXT("profile_gpu_frame"),
		TEXT("Trigger `ProfileGPU` + scrape top-N GPU passes by cost. Returns pass breakdown."),
		FMonolithActionHandler::CreateStatic(&HandleProfileGPUFrame),
		FParamSchemaBuilder()
			.Optional(TEXT("top_n"), TEXT("integer"), TEXT(""), TEXT("15"))
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500"))
			.Build());

	Registry.RegisterAction(NS, TEXT("list_streaming_textures"),
		TEXT("Run `ListStreamingTextures` + parse top-N by size. Returns texture name, max_mb, current_mb, wanted_mb."),
		FMonolithActionHandler::CreateStatic(&HandleListStreamingTextures),
		FParamSchemaBuilder()
			.Optional(TEXT("top_n"), TEXT("integer"), TEXT(""), TEXT("30"))
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500"))
			.Build());

	Registry.RegisterAction(NS, TEXT("dump_memory_by_class"),
		TEXT("Run `obj list class=<class>` + parse top-N by resource size. Common classes: Texture2D, StaticMesh, SkeletalMesh, Material, SoundWave."),
		FMonolithActionHandler::CreateStatic(&HandleDumpMemoryByClass),
		FParamSchemaBuilder()
			.Required(TEXT("class_name"), TEXT("string"), TEXT("UClass name e.g. Texture2D"))
			.Optional(TEXT("top_n"), TEXT("integer"), TEXT(""), TEXT("30"))
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500"))
			.Build());

	Registry.RegisterAction(NS, TEXT("actor_count_by_class"),
		TEXT("Count actors per class in the current world (editor or PIE). Returns top-N by count."),
		FMonolithActionHandler::CreateStatic(&HandleActorCountByClass),
		FParamSchemaBuilder()
			.Optional(TEXT("top_n"), TEXT("integer"), TEXT(""), TEXT("30"))
			.Optional(TEXT("use_pie_world"), TEXT("bool"), TEXT(""), TEXT("true"))
			.Build());

	Registry.RegisterAction(NS, TEXT("get_stats_snapshot"),
		TEXT("Send a `stat <group>` command and return parsed lines. Useful for memory, sceneRendering, fps, streaming, etc."),
		FMonolithActionHandler::CreateStatic(&HandleGetStatsSnapshot),
		FParamSchemaBuilder()
			.Required(TEXT("stat_group"), TEXT("string"), TEXT("e.g. memory / sceneRendering / streaming / fps / unit / nanite"))
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("800"))
			.Build());

	Registry.RegisterAction(NS, TEXT("get_rendering_stats"),
		TEXT("Snapshot of rendering stats: `stat SceneRendering` + `stat InitViews`. Aggregated top-line JSON."),
		FMonolithActionHandler::CreateStatic(&HandleGetRenderingStats),
		FParamSchemaBuilder()
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("800"))
			.Build());

	Registry.RegisterAction(NS, TEXT("get_streaming_stats"),
		TEXT("Texture + level streaming snapshot via `stat Streaming`. Parsed summary."),
		FMonolithActionHandler::CreateStatic(&HandleGetStreamingStats),
		FParamSchemaBuilder()
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("800"))
			.Build());

	Registry.RegisterAction(NS, TEXT("get_ddc_stats"),
		TEXT("DDC / Zen stats via console `stat DDC` + local Zen HTTP query. Returns hit rate etc."),
		FMonolithActionHandler::CreateStatic(&HandleGetDDCStats),
		FParamSchemaBuilder()
			.Optional(TEXT("zen_endpoint"), TEXT("string"), TEXT(""), TEXT("http://[::1]:8558/stats"))
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("500"))
			.Build());

	Registry.RegisterAction(NS, TEXT("get_world_partition_status"),
		TEXT("World Partition streaming: loaded cells, unloaded cells, pending loads."),
		FMonolithActionHandler::CreateStatic(&HandleGetWorldPartitionStatus),
		FParamSchemaBuilder()
			.Optional(TEXT("use_pie_world"), TEXT("bool"), TEXT(""), TEXT("true"))
			.Build());

	Registry.RegisterAction(NS, TEXT("get_pie_perf_snapshot"),
		TEXT("One-shot PIE perf summary: frame stats + actor count + rendering + streaming + memory. Aggregates multiple perf.* calls into single JSON for quick LLM overview."),
		FMonolithActionHandler::CreateStatic(&HandleGetPIEPerfSnapshot),
		FParamSchemaBuilder()
			.Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1000"))
			.Build());

	// =========== M3-M9 bulk registration — 70+ new perf.* actions ===========
	auto RegSimple = [&](const TCHAR* Name, const TCHAR* Desc, FMonolithActionHandler::TFuncType Handler)
	{
		Registry.RegisterAction(NS, Name, Desc, FMonolithActionHandler::CreateStatic(Handler),
			FParamSchemaBuilder().Optional(TEXT("wait_ms"), TEXT("integer"), TEXT("Console output wait ms"), TEXT("800")).Build());
	};

	// D2 Stat primitives
	RegSimple(TEXT("stat_list_groups"), TEXT("List all stat groups available via `stat grouplist`"), &HandleStatListGroups);
	Registry.RegisterAction(NS, TEXT("stat_dumpframe"), TEXT("`stat dumpframe` — single frame structured dump. PIE required."),
		FMonolithActionHandler::CreateStatic(&HandleStatDumpframe),
		FParamSchemaBuilder().Optional(TEXT("root"), TEXT("string"), TEXT("Root stat group")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500")).Build());
	Registry.RegisterAction(NS, TEXT("stat_dumpave"), TEXT("`stat dumpave -num=N` — N-frame averaged stat dump. PIE required."),
		FMonolithActionHandler::CreateStatic(&HandleStatDumpave),
		FParamSchemaBuilder().Optional(TEXT("frames"), TEXT("integer"), TEXT("Frames to average"), TEXT("30")).Optional(TEXT("root"), TEXT("string"), TEXT("Root stat group")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("2500")).Build());
	RegSimple(TEXT("stat_dumphitches"), TEXT("`stat dumphitches` — recent hitch history"), &HandleStatDumphitches);
	Registry.RegisterAction(NS, TEXT("stat_dumpevents"), TEXT("`stat dumpevents -ms=X -all` — event dump above threshold"),
		FMonolithActionHandler::CreateStatic(&HandleStatDumpevents),
		FParamSchemaBuilder().Optional(TEXT("threshold_ms"), TEXT("number"), TEXT(""), TEXT("2.0")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("500")).Build());
	Registry.RegisterAction(NS, TEXT("stat_slow_configure"), TEXT("`stat slow -ms=X` — set long-tick detector threshold"),
		FMonolithActionHandler::CreateStatic(&HandleStatSlowConfigure),
		FParamSchemaBuilder().Optional(TEXT("threshold_ms"), TEXT("number"), TEXT(""), TEXT("5.0")).Build());
	Registry.RegisterAction(NS, TEXT("stat_named_events"), TEXT("`stat namedevents` toggle global named events emission"),
		FMonolithActionHandler::CreateStatic(&HandleStatNamedEvents),
		FParamSchemaBuilder().Optional(TEXT("enable"), TEXT("bool"), TEXT(""), TEXT("true")).Build());
	RegSimple(TEXT("stat_unit_peak"), TEXT("`stat unitmax` — frame time peaks"), &HandleStatUnitPeak);
	RegSimple(TEXT("stat_none"), TEXT("`stat none` — clear all stat overlays"), &HandleStatNone);

	// D2 per-domain wrappers
	RegSimple(TEXT("get_core_stats"), TEXT("`stat unit` — Frame/Game/Draw/GPU/RHI ms snapshot. PIE required."), &HandleGetCoreStats);
	RegSimple(TEXT("get_memory_stats"), TEXT("`stat Memory` — per-subsystem memory breakdown"), &HandleGetMemoryStats);
	RegSimple(TEXT("get_shadow_stats"), TEXT("`stat ShadowRendering` — shadow pass cost. PIE required."), &HandleGetShadowStats);
	RegSimple(TEXT("get_physics_stats"), TEXT("`stat Chaos` — physics solver stats. PIE required."), &HandleGetPhysicsStats);
	RegSimple(TEXT("get_audio_stats"), TEXT("`stat Audio` — audio thread cost + active voices. PIE required."), &HandleGetAudioStats);
	RegSimple(TEXT("get_animation_stats"), TEXT("`stat Anim` — animation eval cost. PIE required."), &HandleGetAnimationStats);
	RegSimple(TEXT("get_network_stats"), TEXT("`stat Net` — replication / bandwidth / RPC. PIE required."), &HandleGetNetworkStats);
	RegSimple(TEXT("get_task_stats"), TEXT("`stat TaskGraph` — task queue depth + thread utilization"), &HandleGetTaskStats);

	// D3 GPU extended
	RegSimple(TEXT("profile_gpu_with_drawcalls"), TEXT("`ProfileGPU` with DrawCallEvents enabled for capture scope. PIE required."), &HandleProfileGPUWithDrawCalls);
	Registry.RegisterAction(NS, TEXT("capture_gpu_frames"), TEXT("`Capture.GPU N` — capture N frames to GPU profiler"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureGPUFrames),
		FParamSchemaBuilder().Optional(TEXT("frames"), TEXT("integer"), TEXT(""), TEXT("1")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("2000")).Build());
	RegSimple(TEXT("get_gpu_realtime"), TEXT("`stat GPU` — realtime GPU overview. PIE required."), &HandleGetGPURealtime);
	RegSimple(TEXT("get_gpu_memory"), TEXT("`stat GPUMemory` — GPU memory breakdown. PIE required."), &HandleGetGPUMemory);
	RegSimple(TEXT("get_nanite_stats"), TEXT("`stat Nanite` — Nanite cluster / cull / triangle rate. PIE required."), &HandleGetNaniteStats);
	RegSimple(TEXT("get_lumen_stats"), TEXT("`stat Lumen` — Lumen GI / reflections cost. PIE required."), &HandleGetLumenStats);
	RegSimple(TEXT("get_raytracing_stats"), TEXT("`stat RayTracing` — RT scene / dispatch cost. PIE required."), &HandleGetRayTracingStats);
	RegSimple(TEXT("get_vsm_stats"), TEXT("`stat VSM` — Virtual Shadow Maps. PIE required."), &HandleGetVSMStats);
	RegSimple(TEXT("get_substrate_stats"), TEXT("`stat Substrate` — Substrate material system (if enabled). PIE required."), &HandleGetSubstrateStats);
	Registry.RegisterAction(NS, TEXT("list_draw_calls"), TEXT("ProfileGPU with draw call events expanded + top-N"),
		FMonolithActionHandler::CreateStatic(&HandleListDrawCalls),
		FParamSchemaBuilder().Optional(TEXT("top_n"), TEXT("integer"), TEXT(""), TEXT("50")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("2000")).Build());
	Registry.RegisterAction(NS, TEXT("profile_gpu_set_sort"), TEXT("Set r.ProfileGPU.Sort mode"),
		FMonolithActionHandler::CreateStatic(&HandleProfileGPUSetSort),
		FParamSchemaBuilder().Optional(TEXT("mode"), TEXT("string"), TEXT("0=time 1=byNum 2=descending 3=alpha"), TEXT("1")).Build());

	// D10+D11 Shader/PSO/DDC
	RegSimple(TEXT("get_shader_compile_jobs"), TEXT("`r.ShaderCompiler.JobTiming 1` — shader compile job timing"), &HandleGetShaderCompileJobs);
	RegSimple(TEXT("get_pso_cache_stats"), TEXT("`r.ShaderPipelineCache.DumpStats` — PSO cache hit/miss stats"), &HandleGetPSOCacheStats);
	RegSimple(TEXT("get_pso_precache_stats"), TEXT("`r.PSOPrecache.Stats` — PSO precache effectiveness"), &HandleGetPSOPrecacheStats);
	RegSimple(TEXT("get_shader_memory"), TEXT("`stat ShaderMemory` — shader code / PSO memory"), &HandleGetShaderMemory);
	Registry.RegisterAction(NS, TEXT("get_material_permutation_count"), TEXT("DumpMaterialShaderMaps for given material"),
		FMonolithActionHandler::CreateStatic(&HandleGetMaterialPermutationCount),
		FParamSchemaBuilder().Required(TEXT("material_path"), TEXT("string"), TEXT("Material asset path")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500")).Build());
	RegSimple(TEXT("list_materials_by_cost"), TEXT("`ListMaterials` — materials in project by complexity"), &HandleListMaterialsByCost);
	Registry.RegisterAction(NS, TEXT("dump_material_shader_map"), TEXT("DumpMaterialShaderMaps for a specific material"),
		FMonolithActionHandler::CreateStatic(&HandleDumpMaterialShaderMap),
		FParamSchemaBuilder().Required(TEXT("material_path"), TEXT("string"), TEXT("Material asset path")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500")).Build());
	Registry.RegisterAction(NS, TEXT("zen_http_stats"), TEXT("HTTP GET Zen stats endpoint"),
		FMonolithActionHandler::CreateStatic(&HandleZenHttpStats),
		FParamSchemaBuilder().Optional(TEXT("endpoint"), TEXT("string"), TEXT("Zen stats URL"), TEXT("http://[::1]:8558/stats")).Build());
	RegSimple(TEXT("zen_health"), TEXT("HTTP GET Zen health endpoint"), &HandleZenHealth);
	RegSimple(TEXT("zen_list_namespaces"), TEXT("HTTP GET Zen namespaces endpoint"), &HandleZenListNamespaces);
	RegSimple(TEXT("ddc_dump"), TEXT("`DumpDDC` — DDC backend dump"), &HandleDDCDump);
	RegSimple(TEXT("ddc_backend_graph"), TEXT("`DerivedDataCache.Dump` — DDC backend config graph"), &HandleDDCBackendGraph);

	// D13 Niagara
	RegSimple(TEXT("niagara_dump_components"), TEXT("`fx.Niagara.DumpComponents` — all Niagara components. PIE required."), &HandleNiagaraDumpComponents);
	RegSimple(TEXT("niagara_list_systems"), TEXT("`fx.Niagara.DumpSystemInstances` — active systems. PIE required."), &HandleNiagaraListSystems);
	RegSimple(TEXT("niagara_emitter_optim_info"), TEXT("`fx.Niagara.DumpEmitterOptimizationRelevancyInfo`. PIE required."), &HandleNiagaraEmitterOptimInfo);
	RegSimple(TEXT("niagara_simcache_stats"), TEXT("`fx.Niagara.SystemSimCacheStats`. PIE required."), &HandleNiagaraSimCacheStats);
	RegSimple(TEXT("niagara_per_system_cost"), TEXT("`stat Niagara` — per-system tick cost. PIE required."), &HandleNiagaraPerSystemCost);
	RegSimple(TEXT("niagara_gpu_cpu_split"), TEXT("GPU vs CPU simulation split via stat Niagara. PIE required."), &HandleNiagaraGPUCPUSplit);
	RegSimple(TEXT("niagara_get_quality_level"), TEXT("`fx.Niagara.QualityLevel` current quality"), &HandleNiagaraGetQualityLevel);
	RegSimple(TEXT("niagara_debug_hud_toggle"), TEXT("`fx.NiagaraDebugHud` toggle"), &HandleNiagaraDebugHUDToggle);

	// D6+D7 Streaming extended
	Registry.RegisterAction(NS, TEXT("list_waiting_textures"), TEXT("Texture streaming items currently waiting. PIE required."),
		FMonolithActionHandler::CreateStatic(&HandleListWaitingTextures),
		FParamSchemaBuilder().Optional(TEXT("top_n"), TEXT("integer"), TEXT(""), TEXT("30")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500")).Build());
	RegSimple(TEXT("get_streaming_pool_size"), TEXT("`r.Streaming.PoolSize` current pool size"), &HandleGetStreamingPoolSize);
	RegSimple(TEXT("get_streaming_pool_status"), TEXT("`stat StreamingOverview` — pool utilization. PIE required."), &HandleGetStreamingPoolStatus);
	RegSimple(TEXT("dump_texture_streaming"), TEXT("`DumpTextureStreamingStats`. PIE required."), &HandleDumpTextureStreaming);
	RegSimple(TEXT("force_stream_all_used"), TEXT("`r.Streaming.FullyLoadUsedTextures 1` — force load all in-use. PIE required."), &HandleForceStreamAllUsed);
	RegSimple(TEXT("get_vt_residency"), TEXT("`r.VT.Residency.Show 1` — Virtual Texturing residency. PIE required."), &HandleGetVTResidency);
	RegSimple(TEXT("dump_vt_pool_usage"), TEXT("`r.VT.DumpPoolUsage`. PIE required."), &HandleDumpVTPoolUsage);
	Registry.RegisterAction(NS, TEXT("list_streaming_levels"), TEXT("Enumerate current streaming levels + status. Walks World->GetStreamingLevels()."),
		FMonolithActionHandler::CreateStatic(&HandleListStreamingLevels),
		FParamSchemaBuilder().Optional(TEXT("use_pie_world"), TEXT("bool"), TEXT(""), TEXT("true")).Build());
	RegSimple(TEXT("wp_cells_at_location"), TEXT("World Partition cells at location (via toggle draw). PIE required."), &HandleWPCellsAtLocation);
	RegSimple(TEXT("wp_list_loaded_cells"), TEXT("`wp.Runtime.ShowStreamingSources`. PIE required."), &HandleWPListLoadedCells);
	RegSimple(TEXT("wp_list_streaming_sources"), TEXT("List WP streaming sources (players + sensors). PIE required."), &HandleWPListStreamingSources);
	RegSimple(TEXT("hlod_list_transitions"), TEXT("`stat HLOD` — HLOD transition info. PIE required."), &HandleHLODListTransitions);
	RegSimple(TEXT("get_hlod_stats"), TEXT("`stat HLOD` stats. PIE required."), &HandleGetHLODStats);

	// D5 Memory extended
	Registry.RegisterAction(NS, TEXT("parse_memreport_section"), TEXT("Extract named section from a .memreport file"),
		FMonolithActionHandler::CreateStatic(&HandleParseMemreportSection),
		FParamSchemaBuilder().Required(TEXT("path"), TEXT("string"), TEXT("Memreport file path")).Optional(TEXT("section"), TEXT("string"), TEXT("Section header"), TEXT("Objects")).Build());
	Registry.RegisterAction(NS, TEXT("diff_memreports"), TEXT("Diff two memreport files at physical/virtual level"),
		FMonolithActionHandler::CreateStatic(&HandleDiffMemreports),
		FParamSchemaBuilder().Required(TEXT("before"), TEXT("string"), TEXT("Before path")).Required(TEXT("after"), TEXT("string"), TEXT("After path")).Build());
	RegSimple(TEXT("get_platform_memory"), TEXT("FPlatformMemory::GetStats — instant RAM/VMem snapshot (no console)"), &HandleGetPlatformMemory);
	RegSimple(TEXT("get_platform_memory_constants"), TEXT("FPlatformMemory::GetConstants — total physical/virtual + page size"), &HandleGetPlatformMemoryConstants);
	RegSimple(TEXT("llm_dump_context"), TEXT("`LLM.DumpLLMContext` — Low-Level Memory Tracker context dump"), &HandleLLMDumpContext);
	RegSimple(TEXT("llm_list_tags"), TEXT("`LLM.ShowTags` — list active LLM tracker tags"), &HandleLLMListTags);
	RegSimple(TEXT("llm_report"), TEXT("`LLM.Report` — LLM tracker summary"), &HandleLLMReport);
	Registry.RegisterAction(NS, TEXT("find_asset_refs"), TEXT("`obj refs Name=X` — who references asset X"),
		FMonolithActionHandler::CreateStatic(&HandleFindAssetRefs),
		FParamSchemaBuilder().Required(TEXT("asset"), TEXT("string"), TEXT("Asset name")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500")).Build());
	Registry.RegisterAction(NS, TEXT("dump_class_hierarchy"), TEXT("`obj list hier=<depth>` — class tree"),
		FMonolithActionHandler::CreateStatic(&HandleDumpClassHierarchy),
		FParamSchemaBuilder().Optional(TEXT("depth"), TEXT("integer"), TEXT(""), TEXT("2")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500")).Build());
	RegSimple(TEXT("gc_collect"), TEXT("Force garbage collection via GEngine->ForceGarbageCollection(true)"), &HandleGCCollect);
	RegSimple(TEXT("gc_dump_reachable"), TEXT("`GC.DumpPoolStats`"), &HandleGCDumpReachable);
	RegSimple(TEXT("malloc_leak_snapshot"), TEXT("`MallocLeak.Snapshot`"), &HandleMallocLeakSnapshot);
	RegSimple(TEXT("malloc_leak_check"), TEXT("`MallocLeak.Check`"), &HandleMallocLeakCheck);

	// D8+D14+D15+D16+D17
	RegSimple(TEXT("get_taskgraph_stats"), TEXT("`stat TaskGraph` — task queue state"), &HandleGetTaskgraphStats);
	RegSimple(TEXT("get_thread_stats"), TEXT("`stat Threading` — per-thread utilization"), &HandleGetThreadStats);
	RegSimple(TEXT("get_async_loading_thread_state"), TEXT("`stat Async` — async loading thread state"), &HandleGetAsyncLoadingThreadState);
	RegSimple(TEXT("get_parallelfor_stats"), TEXT("`stat ParallelTask` — ParallelFor stats"), &HandleGetParallelForStats);
	RegSimple(TEXT("abp_per_actor_cost"), TEXT("`a.AnimNode.Stats` — per-ABP cost. PIE required."), &HandleABPPerActorCost);
	RegSimple(TEXT("abp_dump_states"), TEXT("`a.DumpAnimGraphStates` — state machine eval. PIE required."), &HandleABPDumpStates);
	RegSimple(TEXT("list_active_montages"), TEXT("`stat AnimMontage` — active montages. PIE required."), &HandleListActiveMontages);
	RegSimple(TEXT("get_anim_sharing_stats"), TEXT("`stat AnimationSharing`. PIE required."), &HandleGetAnimSharingStats);
	RegSimple(TEXT("get_chaos_stats"), TEXT("`stat Chaos`. PIE required."), &HandleGetChaosStats);
	RegSimple(TEXT("get_chaos_collisions"), TEXT("`stat ChaosCollisions`. PIE required."), &HandleGetChaosCollisions);
	RegSimple(TEXT("get_active_rigidbody_count"), TEXT("`stat ChaosSolvers`. PIE required."), &HandleGetActiveRigidbodyCount);
	RegSimple(TEXT("chaos_dump_evolution"), TEXT("`DumpChaosEvolutionStats`. PIE required."), &HandleChaosDumpEvolution);
	RegSimple(TEXT("get_active_voice_count"), TEXT("`stat AudioChannels`. PIE required."), &HandleGetActiveVoiceCount);
	RegSimple(TEXT("audio_dump_concurrency"), TEXT("`au.DumpConcurrencyStats`. PIE required."), &HandleAudioDumpConcurrency);
	RegSimple(TEXT("audio_list_active_sounds"), TEXT("`au.DumpActiveSounds`. PIE required."), &HandleAudioListActiveSounds);
	RegSimple(TEXT("metasound_list_active"), TEXT("`meta.Sound.DumpActiveSounds`. PIE required."), &HandleMetaSoundListActive);
	RegSimple(TEXT("get_net_bandwidth"), TEXT("`stat NetPackets`. PIE required + multiplayer."), &HandleGetNetBandwidth);
	RegSimple(TEXT("net_rpc_by_class"), TEXT("`stat NetRPC`. PIE required + multiplayer."), &HandleNetRPCByClass);
	RegSimple(TEXT("net_list_connections"), TEXT("`stat Net`. PIE required + multiplayer."), &HandleNetListConnections);
	RegSimple(TEXT("net_dump_replication_graph"), TEXT("`net.ReplicationGraphDumpAllRepList`. PIE required + multiplayer."), &HandleNetDumpReplicationGraph);

	// CSV extended
	Registry.RegisterAction(NS, TEXT("csv_breadcrumb"), TEXT("Insert a breadcrumb in active CsvProfile"),
		FMonolithActionHandler::CreateStatic(&HandleCsvBreadcrumb),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Breadcrumb name")).Build());
	Registry.RegisterAction(NS, TEXT("csv_metadata"), TEXT("Add CSV metadata key=value"),
		FMonolithActionHandler::CreateStatic(&HandleCsvMetadata),
		FParamSchemaBuilder().Required(TEXT("key"), TEXT("string"), TEXT("Metadata key")).Required(TEXT("value"), TEXT("string"), TEXT("Metadata value")).Build());
	RegSimple(TEXT("csv_list_categories"), TEXT("`CsvProfile list` — list categories"), &HandleCsvListCategories);
	Registry.RegisterAction(NS, TEXT("csv_set_category"), TEXT("Enable/disable CsvProfile category"),
		FMonolithActionHandler::CreateStatic(&HandleCsvSetCategory),
		FParamSchemaBuilder().Required(TEXT("category"), TEXT("string"), TEXT("Category name")).Optional(TEXT("enable"), TEXT("bool"), TEXT(""), TEXT("true")).Build());
	Registry.RegisterAction(NS, TEXT("summarize_csv"), TEXT("Load .csv and return row count / header / size"),
		FMonolithActionHandler::CreateStatic(&HandleSummarizeCsv),
		FParamSchemaBuilder().Required(TEXT("path"), TEXT("string"), TEXT("CSV file path")).Build());
	Registry.RegisterAction(NS, TEXT("detect_csv_hitches"), TEXT("Parse CSV FrameTime column + return frames above threshold"),
		FMonolithActionHandler::CreateStatic(&HandleDetectCsvHitches),
		FParamSchemaBuilder().Required(TEXT("path"), TEXT("string"), TEXT("CSV file path")).Optional(TEXT("threshold_ms"), TEXT("number"), TEXT(""), TEXT("50.0")).Build());
	Registry.RegisterAction(NS, TEXT("csv_list_metrics"), TEXT("List column names from a .csv"),
		FMonolithActionHandler::CreateStatic(&HandleCsvListMetrics),
		FParamSchemaBuilder().Required(TEXT("path"), TEXT("string"), TEXT("CSV file path")).Build());

	// Trace control extended
	RegSimple(TEXT("pause_trace"), TEXT("`Trace.Pause`"), &HandlePauseTrace);
	RegSimple(TEXT("resume_trace"), TEXT("`Trace.Resume`"), &HandleResumeTrace);
	RegSimple(TEXT("trace_status"), TEXT("`Trace.Status`"), &HandleTraceStatus);
	Registry.RegisterAction(NS, TEXT("trace_bookmark"), TEXT("Emit a Trace.Bookmark"),
		FMonolithActionHandler::CreateStatic(&HandleTraceBookmark),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Bookmark name")).Build());
	RegSimple(TEXT("trace_list_channels"), TEXT("`Trace.ListChannels`"), &HandleTraceListChannels);
	RegSimple(TEXT("trace_snapshot"), TEXT("`Trace.Snapshot` — take snapshot of current trace"), &HandleTraceSnapshot);

	// D9 Asset Deps
	Registry.RegisterAction(NS, TEXT("get_asset_dependencies"), TEXT("`obj dependencies Name=X` — forward dependency list"),
		FMonolithActionHandler::CreateStatic(&HandleGetAssetDependencies),
		FParamSchemaBuilder().Required(TEXT("path"), TEXT("string"), TEXT("Asset path")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500")).Build());
	Registry.RegisterAction(NS, TEXT("get_asset_referencers"), TEXT("`obj refs Name=X` — backward references"),
		FMonolithActionHandler::CreateStatic(&HandleGetAssetReferencers),
		FParamSchemaBuilder().Required(TEXT("path"), TEXT("string"), TEXT("Asset path")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500")).Build());
	RegSimple(TEXT("get_asset_registry_stats"), TEXT("`AssetManager.DumpAssetRegistryInfo`"), &HandleGetAssetRegistryStats);

	// D12 Material
	Registry.RegisterAction(NS, TEXT("get_material_stats"), TEXT("DumpMaterialShaderMaps for specific material"),
		FMonolithActionHandler::CreateStatic(&HandleGetMaterialStats),
		FParamSchemaBuilder().Required(TEXT("material_path"), TEXT("string"), TEXT("Material asset path")).Optional(TEXT("wait_ms"), TEXT("integer"), TEXT(""), TEXT("1500")).Build());
	RegSimple(TEXT("get_material_complexity"), TEXT("`r.ShowMaterialStats 1` — toggle material complexity viewmode"), &HandleGetMaterialComplexity);

	// Workflow aggregates
	RegSimple(TEXT("get_full_snapshot"), TEXT("Aggregate: memory + frame + actors + streaming in one call"), &HandleGetFullSnapshot);
	RegSimple(TEXT("stop_all_overlays"), TEXT("`stat none` — clear all HUD stat overlays"), &HandleStopAllOverlays);

	// M10 D1 Trace analysis
	Registry.RegisterAction(TEXT("perf"), TEXT("analyze_trace"),
		TEXT("Parse a .utrace file in-process via TraceServices. Returns top CPU timers (by total + by max single-call), bookmarks, counters (228+ metrics inc. LLM/DDC/RDG/Chaos/Scene/AsyncLoading/Shader). Blocking: may take 5-30s for large traces."),
		FMonolithActionHandler::CreateStatic(&HandleAnalyzeTrace),
		FParamSchemaBuilder()
			.Required(TEXT("trace_path"), TEXT("string"), TEXT("Absolute path to .utrace file, or relative to Saved/Profiling/"))
			.Optional(TEXT("top_n"), TEXT("integer"), TEXT("Number of top timers/hitches to return (default 30, max 200)"), TEXT("30"))
			.Build());

	// M10 D5 LLM programmatic access
	Registry.RegisterAction(TEXT("perf"), TEXT("llm_tag_values"),
		TEXT("Read Low Level Memory Tracker tag values via C++ API (not console command). Returns every LLM tag with current byte count, sorted by size. 109 built-in tags + custom project tags."),
		FMonolithActionHandler::CreateStatic(&HandleLLMTagValues),
		FParamSchemaBuilder()
			.Optional(TEXT("tracker"), TEXT("string"), TEXT("LLM tracker: 'Default' or 'Platform'"), TEXT("Default"))
			.Optional(TEXT("top_n"), TEXT("integer"), TEXT("Max tags to return (default 100)"), TEXT("100"))
			.Build());
}

// ==================== P0 Core handlers (re-implemented) ====================
// These mirror the same logic as MonolithEditorActions' equivalents but live in perf/.

FMonolithActionResult FMonolithPerfActions::HandleTimeBetween(const TSharedPtr<FJsonObject>& Params)
{
	const FString PA = Params->GetStringField(TEXT("pattern_a"));
	const FString PB = Params->GetStringField(TEXT("pattern_b"));
	if (PA.IsEmpty() || PB.IsEmpty()) return FMonolithActionResult::Error(TEXT("pattern_a and pattern_b required"));

	double Since = 0.0;
	if (Params->HasField(TEXT("since"))) Since = Params->GetNumberField(TEXT("since"));

	TArray<FName> CatFilter;
	if (Params->HasField(TEXT("category")))
	{
		FString S = Params->GetStringField(TEXT("category"));
		if (!S.IsEmpty()) CatFilter.Add(FName(*S));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("pattern_a"), PA);
	Root->SetStringField(TEXT("pattern_b"), PB);

	FPerfLogCapture* Cap = FPerfLogCapture::Get();
	if (!Cap)
	{
		Root->SetBoolField(TEXT("found"), false);
		Root->SetStringField(TEXT("error"), TEXT("FPerfLogCapture not installed"));
		return FMonolithActionResult::Success(Root);
	}

	TArray<FPerfLogCapture::FEntry> Entries = Cap->GetEntriesSince(Since, CatFilter, FPerfLogCapture::MaxEntries);
	const FRegexPattern PatA(PA);
	const FRegexPattern PatB(PB);
	double TsA = -1.0, TsB = -1.0;
	FString MatchA, MatchB;
	for (const auto& E : Entries)
	{
		if (TsA < 0.0)
		{
			FRegexMatcher M(PatA, E.Message);
			if (M.FindNext()) { TsA = E.Timestamp; MatchA = E.Message; continue; }
		}
		else
		{
			FRegexMatcher M(PatB, E.Message);
			if (M.FindNext()) { TsB = E.Timestamp; MatchB = E.Message; break; }
		}
	}

	if (TsA < 0.0) { Root->SetBoolField(TEXT("found"), false); Root->SetStringField(TEXT("reason"), TEXT("pattern_a not found")); return FMonolithActionResult::Success(Root); }
	if (TsB < 0.0) { Root->SetBoolField(TEXT("found"), false); Root->SetStringField(TEXT("reason"), TEXT("pattern_b not found after pattern_a")); Root->SetNumberField(TEXT("ts_a"), TsA); Root->SetStringField(TEXT("match_a"), MatchA); return FMonolithActionResult::Success(Root); }

	Root->SetBoolField(TEXT("found"), true);
	Root->SetNumberField(TEXT("duration_ms"), (TsB - TsA) * 1000.0);
	Root->SetNumberField(TEXT("duration_sec"), TsB - TsA);
	Root->SetNumberField(TEXT("ts_a"), TsA);
	Root->SetNumberField(TEXT("ts_b"), TsB);
	Root->SetStringField(TEXT("match_a"), MatchA);
	Root->SetStringField(TEXT("match_b"), MatchB);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleRunCommandlet(const TSharedPtr<FJsonObject>& Params)
{
	const FString CmdName = Params->GetStringField(TEXT("commandlet"));
	if (CmdName.IsEmpty()) return FMonolithActionResult::Error(TEXT("commandlet required"));

	int32 TimeoutSec = 300;
	if (Params->HasField(TEXT("timeout_sec"))) TimeoutSec = static_cast<int32>(Params->GetNumberField(TEXT("timeout_sec")));
	TimeoutSec = FMath::Clamp(TimeoutSec, 1, 3600);

	bool bCapture = true;
	if (Params->HasField(TEXT("capture_output"))) bCapture = Params->GetBoolField(TEXT("capture_output"));

	const FString ExePath = FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealEditor-Cmd.exe");
	if (!FPaths::FileExists(ExePath)) return FMonolithActionResult::Error(FString::Printf(TEXT("cmd exe missing: %s"), *ExePath));

	FString Args = FString::Printf(TEXT("\"%s\" -run=%s"), *FPaths::GetProjectFilePath(), *CmdName);
	if (Params->HasField(TEXT("args")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (Params->TryGetArrayField(TEXT("args"), Arr))
			for (auto& V : *Arr) Args += TEXT(" ") + V->AsString();
	}
	Args += TEXT(" -unattended -nopause -nosplash");

	void* R = nullptr; void* W = nullptr;
	if (bCapture) FPlatformProcess::CreatePipe(R, W);
	uint32 Pid = 0;
	FProcHandle H = FPlatformProcess::CreateProc(*ExePath, *Args, false, true, true, &Pid, 0, nullptr, W, nullptr);
	if (!H.IsValid()) { if (R||W) FPlatformProcess::ClosePipe(R, W); return FMonolithActionResult::Error(TEXT("CreateProc failed")); }

	FString Out;
	const double T0 = FPlatformTime::Seconds();
	int32 Exit = -1;
	while (FPlatformProcess::IsProcRunning(H))
	{
		if (bCapture && R) { FString Chunk = FPlatformProcess::ReadPipe(R); if (!Chunk.IsEmpty()) { Out += Chunk; if (Out.Len() > 1024*1024) Out = TEXT("...(trunc)...") + Out.Right(512*1024); } }
		if (FPlatformTime::Seconds() - T0 > static_cast<double>(TimeoutSec)) { FPlatformProcess::TerminateProc(H, true); break; }
		FPlatformProcess::Sleep(0.1f);
	}
	bool bDone = FPlatformProcess::GetProcReturnCode(H, &Exit);
	if (bCapture && R) { FString Tail = FPlatformProcess::ReadPipe(R); if (!Tail.IsEmpty()) Out += Tail; FPlatformProcess::ClosePipe(R, W); }
	FPlatformProcess::CloseProc(H);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("commandlet"), CmdName);
	Root->SetBoolField(TEXT("completed"), bDone);
	Root->SetNumberField(TEXT("exit_code"), Exit);
	Root->SetNumberField(TEXT("elapsed_sec"), FPlatformTime::Seconds() - T0);
	Root->SetNumberField(TEXT("pid"), Pid);
	if (bCapture) Root->SetStringField(TEXT("stdout"), Out);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleExecuteConsoleCommand(const TSharedPtr<FJsonObject>& Params)
{
	const FString Command = Params->GetStringField(TEXT("command"));
	if (Command.IsEmpty()) return FMonolithActionResult::Error(TEXT("command required"));

	int32 WaitMs = 500;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = static_cast<int32>(Params->GetNumberField(TEXT("wait_ms")));
	WaitMs = FMath::Clamp(WaitMs, 0, 30000);

	FName CategoryFilter = NAME_None;
	if (Params->HasField(TEXT("category")))
	{
		const FString S = Params->GetStringField(TEXT("category"));
		if (!S.IsEmpty()) CategoryFilter = FName(*S);
	}
	int32 Limit = 500;
	if (Params->HasField(TEXT("limit"))) Limit = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("limit"))), 1, 2000);

	TArray<TSharedPtr<FJsonValue>> Lines = ExecCommandCapture(Command, WaitMs, CategoryFilter, Limit);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("command"), Command);
	Root->SetNumberField(TEXT("lines_captured"), Lines.Num());
	Root->SetArrayField(TEXT("raw_lines"), Lines);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleDumpShaderCompileStats(const TSharedPtr<FJsonObject>& Params)
{
	int32 WaitMs = 500;
	if (Params.IsValid() && Params->HasField(TEXT("wait_ms"))) WaitMs = static_cast<int32>(Params->GetNumberField(TEXT("wait_ms")));

	TArray<TSharedPtr<FJsonValue>> Lines = ExecCommandCapture(
		TEXT("DumpShaderCompileStats"), FMath::Clamp(WaitMs, 0, 5000),
		FName(TEXT("LogShaderCompilers")), 500);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetNumberField(TEXT("lines_captured"), Lines.Num());
	Root->SetArrayField(TEXT("raw_lines"), Lines);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleStartTrace(const TSharedPtr<FJsonObject>& Params)
{
	FString Channels = TEXT("default,loadtime,cpu,gpu,frame,bookmark,counters,stats");
	if (Params->HasField(TEXT("channels"))) Channels = Params->GetStringField(TEXT("channels"));

	FString SessionName;
	if (Params->HasField(TEXT("session_name"))) SessionName = Params->GetStringField(TEXT("session_name"));
	if (SessionName.IsEmpty()) SessionName = FString::Printf(TEXT("monolith_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	if (!GEngine) return FMonolithActionResult::Error(TEXT("GEngine unavailable"));

	GEngine->Exec(nullptr, *FString::Printf(TEXT("Trace.Start %s"), *Channels));
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("channels"), Channels);
	Root->SetStringField(TEXT("session_name"), SessionName);
	Root->SetStringField(TEXT("expected_dir"), FPaths::ProjectDir() / TEXT("Saved") / TEXT("Profiling"));
	Root->SetStringField(TEXT("note"), TEXT("Trace capturing. Call perf.stop_trace to finalize."));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleStopTrace(const TSharedPtr<FJsonObject>& Params)
{
	int32 WaitMs = 2000;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = static_cast<int32>(Params->GetNumberField(TEXT("wait_ms")));
	WaitMs = FMath::Clamp(WaitMs, 0, 30000);

	if (!GEngine) return FMonolithActionResult::Error(TEXT("GEngine unavailable"));

	const FDateTime Before = FDateTime::UtcNow() - FTimespan::FromMinutes(10);
	GEngine->Exec(nullptr, TEXT("Trace.Stop"));
	if (WaitMs > 0) FPlatformProcess::Sleep(WaitMs * 0.001f);

	const FString Dir = FPaths::ProjectDir() / TEXT("Saved") / TEXT("Profiling");
	const FString File = FindNewestMatchingFile(Dir, TEXT("*.utrace"), Before);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	if (!File.IsEmpty())
	{
		Root->SetStringField(TEXT("utrace_path"), File);
		Root->SetNumberField(TEXT("size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*File)));
	}
	else
	{
		Root->SetStringField(TEXT("utrace_path"), TEXT(""));
		Root->SetStringField(TEXT("warning"), TEXT("No .utrace found; was trace started?"));
	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleCaptureStatsSession(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params->HasField(TEXT("duration_sec"))) return FMonolithActionResult::Error(TEXT("duration_sec required"));
	const double Dur = Params->GetNumberField(TEXT("duration_sec"));
	if (Dur <= 0.0 || Dur > 300.0) return FMonolithActionResult::Error(TEXT("duration_sec in (0,300]"));

	FString Filename;
	if (Params->HasField(TEXT("filename"))) Filename = Params->GetStringField(TEXT("filename"));
	if (Filename.IsEmpty()) Filename = FString::Printf(TEXT("monolith_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

	if (!GEngine) return FMonolithActionResult::Error(TEXT("GEngine unavailable"));

	const FDateTime Before = FDateTime::UtcNow() - FTimespan::FromMinutes(2);
	GEngine->Exec(nullptr, *FString::Printf(TEXT("stat startfile %s"), *Filename));
	FPlatformProcess::Sleep(static_cast<float>(Dur));
	GEngine->Exec(nullptr, TEXT("stat stopfile"));
	FPlatformProcess::Sleep(0.5f);

	const FString File = FindNewestMatchingFile(FPaths::ProjectDir() / TEXT("Saved/Profiling/UnrealStats"), TEXT("*.uestats"), Before);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("filename"), Filename);
	Root->SetNumberField(TEXT("duration_sec"), Dur);
	if (!File.IsEmpty())
	{
		Root->SetStringField(TEXT("uestats_path"), File);
		Root->SetNumberField(TEXT("size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*File)));
	}
	else { Root->SetStringField(TEXT("uestats_path"), TEXT("")); Root->SetStringField(TEXT("warning"), TEXT("No .uestats emitted"));	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleCaptureCsvProfile(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params->HasField(TEXT("duration_sec"))) return FMonolithActionResult::Error(TEXT("duration_sec required"));
	const double Dur = Params->GetNumberField(TEXT("duration_sec"));
	if (Dur <= 0.0 || Dur > 300.0) return FMonolithActionResult::Error(TEXT("duration_sec in (0,300]"));

	FString Filename;
	if (Params->HasField(TEXT("filename"))) Filename = Params->GetStringField(TEXT("filename"));
	if (Filename.IsEmpty()) Filename = FString::Printf(TEXT("monolith_%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

	if (!GEngine) return FMonolithActionResult::Error(TEXT("GEngine unavailable"));

	const FDateTime Before = FDateTime::UtcNow() - FTimespan::FromMinutes(2);
	GEngine->Exec(nullptr, *FString::Printf(TEXT("CsvProfile start Filename=%s"), *Filename));
	FPlatformProcess::Sleep(static_cast<float>(Dur));
	GEngine->Exec(nullptr, TEXT("CsvProfile stop"));
	FPlatformProcess::Sleep(0.5f);

	const FString File = FindNewestMatchingFile(FPaths::ProjectDir() / TEXT("Saved/Profiling/CSV"), TEXT("*.csv"), Before);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("filename"), Filename);
	Root->SetNumberField(TEXT("duration_sec"), Dur);
	if (!File.IsEmpty())
	{
		Root->SetStringField(TEXT("csv_path"), File);
		Root->SetNumberField(TEXT("size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*File)));
	}
	else { Root->SetStringField(TEXT("csv_path"), TEXT("")); Root->SetStringField(TEXT("warning"), TEXT("CsvProfile may require PIE running"));	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleCaptureMemreport(const TSharedPtr<FJsonObject>& Params)
{
	int32 WaitMs = 2000;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = static_cast<int32>(Params->GetNumberField(TEXT("wait_ms")));
	WaitMs = FMath::Clamp(WaitMs, 500, 30000);

	if (!GEngine) return FMonolithActionResult::Error(TEXT("GEngine unavailable"));

	const FDateTime Before = FDateTime::UtcNow() - FTimespan::FromMinutes(2);
	GEngine->Exec(nullptr, TEXT("memreport -full"));
	FPlatformProcess::Sleep(WaitMs * 0.001f);

	const FString Dir = FPaths::ProjectDir() / TEXT("Saved/Profiling/MemReports");
	const FString File = FindNewestMatchingFile(Dir, TEXT("*.memreport"), Before);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	if (File.IsEmpty())
	{
		Root->SetStringField(TEXT("memreport_path"), TEXT(""));
		Root->SetStringField(TEXT("warning"), TEXT("No .memreport file; try again with larger wait_ms if PIE is running"));
		return FMonolithActionResult::Success(Root);
	}

	Root->SetStringField(TEXT("memreport_path"), File);
	Root->SetNumberField(TEXT("size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*File)));

	FString Contents;
	if (FFileHelper::LoadFileToString(Contents, *File))
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		const FString Header = Contents.Left(FMath::Min(8192, Contents.Len()));
		auto Pull = [&](const FString& Pat, const TCHAR* Key)
		{
			FRegexMatcher M(FRegexPattern(Pat), Header);
			if (M.FindNext())
			{
				FString V = M.GetCaptureGroup(1); V.ReplaceInline(TEXT(","), TEXT(""));
				double Num = FCString::Atod(*V);
				double Mb = (M.GetCaptureGroup(2) == TEXT("GB")) ? Num * 1024.0 : Num;
				Summary->SetNumberField(Key, Mb);
			}
		};
		Pull(TEXT("Physical Memory:\\s*([\\d,\\.]+)\\s*(MB|GB)"), TEXT("physical_mb"));
		Pull(TEXT("Virtual Memory:\\s*([\\d,\\.]+)\\s*(MB|GB)"), TEXT("virtual_mb"));
		Pull(TEXT("Peak Memory:\\s*([\\d,\\.]+)\\s*(MB|GB)"), TEXT("peak_mb"));
		Summary->SetNumberField(TEXT("total_file_chars"), Contents.Len());
		Root->SetObjectField(TEXT("summary"), Summary);
	}
	return FMonolithActionResult::Success(Root);
}

// ==================== P1 Diagnostic handlers (new) ====================

FMonolithActionResult FMonolithPerfActions::HandleGetFrameStats(const TSharedPtr<FJsonObject>& Params)
{
	int32 WaitMs = 500;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = static_cast<int32>(Params->GetNumberField(TEXT("wait_ms")));

	// Toggle `stat unit` on, wait, off, scrape. `stat unit` prints to overlay usually — the
	// reliable programmatic path is `stat dumphitches` or read GEngine stats directly.
	// Here we use `t.FPSChart.StartChart` + `StopChart` alternative — or simpler, read GEngine frame time.
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);

	if (GEngine)
	{
		// GEngine tracks frame counters we can read directly
		const double FrameTimeMs = FApp::GetDeltaTime() * 1000.0;
		Root->SetNumberField(TEXT("frame_ms"), FrameTimeMs);
		Root->SetNumberField(TEXT("fps"), FrameTimeMs > 0.0 ? 1000.0 / FrameTimeMs : 0.0);
		Root->SetBoolField(TEXT("pie_active"), GEditor && GEditor->PlayWorld != nullptr);
	}

	// Also scrape any `stat slow` output that's already in the log
	TArray<TSharedPtr<FJsonValue>> Lines = ExecCommandCapture(TEXT("stat slow"), FMath::Clamp(WaitMs, 0, 3000));
	Root->SetNumberField(TEXT("stat_slow_lines"), Lines.Num());
	Root->SetArrayField(TEXT("raw_lines"), Lines);
	Root->SetStringField(TEXT("note"), TEXT("frame_ms from FApp::GetDeltaTime (may be 0 in idle editor). For detailed per-thread breakdown use perf.capture_stats_session or perf.start_trace."));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleProfileGPUFrame(const TSharedPtr<FJsonObject>& Params)
{
	int32 TopN = 15;
	if (Params->HasField(TEXT("top_n"))) TopN = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("top_n"))), 1, 200);
	int32 WaitMs = 1500;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("wait_ms"))), 100, 10000);

	TArray<TSharedPtr<FJsonValue>> Lines = ExecCommandCapture(TEXT("ProfileGPU"), WaitMs);

	// Parse lines like "       X.XXms   Y.Y%   PassName" — heuristic
	const FRegexPattern PassPat(TEXT("^\\s*(\\d+\\.\\d+)ms\\s+(\\d+\\.\\d+)%\\s+(.+)$"));
	TArray<TSharedPtr<FJsonValue>> Passes;
	for (const TSharedPtr<FJsonValue>& LineVal : Lines)
	{
		if (Passes.Num() >= TopN) break;
		FString Line = LineVal->AsString();
		FRegexMatcher M(PassPat, Line);
		if (M.FindNext())
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetNumberField(TEXT("ms"), FCString::Atod(*M.GetCaptureGroup(1)));
			P->SetNumberField(TEXT("percent"), FCString::Atod(*M.GetCaptureGroup(2)));
			P->SetStringField(TEXT("pass"), M.GetCaptureGroup(3).TrimStartAndEnd());
			Passes.Add(MakeShared<FJsonValueObject>(P));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetNumberField(TEXT("top_n"), TopN);
	Root->SetNumberField(TEXT("passes_found"), Passes.Num());
	Root->SetArrayField(TEXT("passes"), Passes);
	Root->SetNumberField(TEXT("raw_lines_captured"), Lines.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleListStreamingTextures(const TSharedPtr<FJsonObject>& Params)
{
	int32 TopN = 30;
	if (Params->HasField(TEXT("top_n"))) TopN = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("top_n"))), 1, 500);
	int32 WaitMs = 1500;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("wait_ms"))), 200, 10000);

	TArray<TSharedPtr<FJsonValue>> Lines = ExecCommandCapture(TEXT("ListStreamingTextures"), WaitMs);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetNumberField(TEXT("raw_lines_captured"), Lines.Num());
	Root->SetNumberField(TEXT("top_n"), TopN);
	// Return up to TopN raw lines — caller parses
	TArray<TSharedPtr<FJsonValue>> Top;
	for (int32 i = 0; i < FMath::Min(TopN, Lines.Num()); ++i) Top.Add(Lines[i]);
	Root->SetArrayField(TEXT("raw_lines"), Top);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleDumpMemoryByClass(const TSharedPtr<FJsonObject>& Params)
{
	const FString ClassName = Params->GetStringField(TEXT("class_name"));
	if (ClassName.IsEmpty()) return FMonolithActionResult::Error(TEXT("class_name required"));

	int32 TopN = 30;
	if (Params->HasField(TEXT("top_n"))) TopN = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("top_n"))), 1, 500);
	int32 WaitMs = 1500;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("wait_ms"))), 200, 10000);

	const FString Cmd = FString::Printf(TEXT("obj list class=%s"), *ClassName);
	TArray<TSharedPtr<FJsonValue>> Lines = ExecCommandCapture(Cmd, WaitMs);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("class_name"), ClassName);
	Root->SetNumberField(TEXT("raw_lines_captured"), Lines.Num());
	TArray<TSharedPtr<FJsonValue>> Top;
	for (int32 i = 0; i < FMath::Min(TopN, Lines.Num()); ++i) Top.Add(Lines[i]);
	Root->SetArrayField(TEXT("raw_lines"), Top);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleActorCountByClass(const TSharedPtr<FJsonObject>& Params)
{
	int32 TopN = 30;
	if (Params->HasField(TEXT("top_n"))) TopN = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("top_n"))), 1, 500);
	bool bUsePIE = true;
	if (Params->HasField(TEXT("use_pie_world"))) bUsePIE = Params->GetBoolField(TEXT("use_pie_world"));

	UWorld* World = nullptr;
	if (GEditor)
	{
		World = (bUsePIE && GEditor->PlayWorld) ? ToRawPtr(GEditor->PlayWorld) : GEditor->GetEditorWorldContext().World();
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	if (!World) { Root->SetBoolField(TEXT("ok"), false); Root->SetStringField(TEXT("error"), TEXT("No valid world")); return FMonolithActionResult::Success(Root); }

	TMap<FString, int32> Counts;
	int32 Total = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!*It) continue;
		const UClass* Cls = It->GetClass();
		if (!Cls) continue;
		Counts.FindOrAdd(Cls->GetPathName())++;
		++Total;
	}

	Counts.ValueSort([](const int32& A, const int32& B) { return A > B; });

	TArray<TSharedPtr<FJsonValue>> Arr;
	int32 N = 0;
	for (const TPair<FString, int32>& P : Counts)
	{
		if (N++ >= TopN) break;
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("class"), P.Key);
		E->SetNumberField(TEXT("count"), P.Value);
		Arr.Add(MakeShared<FJsonValueObject>(E));
	}

	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("world_name"), World->GetName());
	Root->SetNumberField(TEXT("total_actors"), Total);
	Root->SetNumberField(TEXT("unique_classes"), Counts.Num());
	Root->SetArrayField(TEXT("top_by_count"), Arr);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleGetStatsSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	const FString Group = Params->GetStringField(TEXT("stat_group"));
	if (Group.IsEmpty()) return FMonolithActionResult::Error(TEXT("stat_group required"));
	int32 WaitMs = 800;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("wait_ms"))), 100, 5000);

	TArray<TSharedPtr<FJsonValue>> Lines = ExecCommandCapture(FString::Printf(TEXT("stat %s"), *Group), WaitMs);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("stat_group"), Group);
	Root->SetNumberField(TEXT("lines_captured"), Lines.Num());
	Root->SetArrayField(TEXT("raw_lines"), Lines);
	Root->SetStringField(TEXT("note"), TEXT("`stat <group>` toggles overlay; output capture depends on group. For structured data use dedicated APIs (perf.get_rendering_stats, perf.list_streaming_textures, etc)"));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleGetRenderingStats(const TSharedPtr<FJsonObject>& Params)
{
	int32 WaitMs = 800;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("wait_ms"))), 100, 5000);

	TArray<TSharedPtr<FJsonValue>> Scene = ExecCommandCapture(TEXT("stat SceneRendering"), WaitMs / 2);
	TArray<TSharedPtr<FJsonValue>> Views = ExecCommandCapture(TEXT("stat InitViews"), WaitMs / 2);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetArrayField(TEXT("scene_rendering"), Scene);
	Root->SetArrayField(TEXT("init_views"), Views);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleGetStreamingStats(const TSharedPtr<FJsonObject>& Params)
{
	int32 WaitMs = 800;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("wait_ms"))), 100, 5000);

	TArray<TSharedPtr<FJsonValue>> S = ExecCommandCapture(TEXT("stat Streaming"), WaitMs);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetArrayField(TEXT("raw_lines"), S);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleGetDDCStats(const TSharedPtr<FJsonObject>& Params)
{
	FString Endpoint = TEXT("http://[::1]:8558/stats");
	if (Params->HasField(TEXT("zen_endpoint"))) Endpoint = Params->GetStringField(TEXT("zen_endpoint"));
	int32 WaitMs = 500;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("wait_ms"))), 100, 5000);

	TArray<TSharedPtr<FJsonValue>> DdcLines = ExecCommandCapture(TEXT("stat DDC"), WaitMs / 2);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("zen_endpoint"), Endpoint);
	Root->SetArrayField(TEXT("stat_ddc_raw_lines"), DdcLines);
	Root->SetStringField(TEXT("note"), TEXT("HTTP GET to Zen endpoint deferred (needs async HTTP + callback wiring). For now use execute_console_command 'stat DDC' / 'DumpDDC' + scrape."));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleGetWorldPartitionStatus(const TSharedPtr<FJsonObject>& Params)
{
	bool bUsePIE = true;
	if (Params->HasField(TEXT("use_pie_world"))) bUsePIE = Params->GetBoolField(TEXT("use_pie_world"));

	UWorld* World = nullptr;
	if (GEditor) World = (bUsePIE && GEditor->PlayWorld) ? ToRawPtr(GEditor->PlayWorld) : GEditor->GetEditorWorldContext().World();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	if (!World) return FMonolithActionResult::Error(TEXT("No valid world"));

	// Basic streaming level stats via console
	TArray<TSharedPtr<FJsonValue>> Lines = ExecCommandCapture(TEXT("stat LevelStreaming"), 500);

	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("world_name"), World->GetName());
	Root->SetNumberField(TEXT("streaming_levels_count"), World->GetStreamingLevels().Num());
	Root->SetArrayField(TEXT("stat_level_streaming"), Lines);
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithPerfActions::HandleGetPIEPerfSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	int32 WaitMs = 1000;
	if (Params->HasField(TEXT("wait_ms"))) WaitMs = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("wait_ms"))), 200, 10000);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);

	// Frame
	const double FrameMs = FApp::GetDeltaTime() * 1000.0;
	Root->SetNumberField(TEXT("frame_ms"), FrameMs);
	Root->SetNumberField(TEXT("fps"), FrameMs > 0.0 ? 1000.0 / FrameMs : 0.0);

	// PIE state
	UWorld* World = nullptr;
	if (GEditor)
	{
		World = GEditor->PlayWorld ? ToRawPtr(GEditor->PlayWorld) : GEditor->GetEditorWorldContext().World();
		Root->SetBoolField(TEXT("pie_active"), GEditor->PlayWorld != nullptr);
	}
	if (World)
	{
		Root->SetStringField(TEXT("world_name"), World->GetName());
		Root->SetNumberField(TEXT("streaming_levels_count"), World->GetStreamingLevels().Num());

		// Quick actor count
		int32 Total = 0;
		for (TActorIterator<AActor> It(World); It; ++It) { if (*It) ++Total; }
		Root->SetNumberField(TEXT("total_actors"), Total);
	}

	// Memory snapshot from FPlatformMemory
	{
		FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
		TSharedPtr<FJsonObject> Mem = MakeShared<FJsonObject>();
		Mem->SetNumberField(TEXT("used_physical_mb"), Stats.UsedPhysical / (1024.0 * 1024.0));
		Mem->SetNumberField(TEXT("peak_used_physical_mb"), Stats.PeakUsedPhysical / (1024.0 * 1024.0));
		Mem->SetNumberField(TEXT("used_virtual_mb"), Stats.UsedVirtual / (1024.0 * 1024.0));
		Mem->SetNumberField(TEXT("available_physical_mb"), Stats.AvailablePhysical / (1024.0 * 1024.0));
		Root->SetObjectField(TEXT("memory"), Mem);
	}

	// Queue / streaming status
	TArray<TSharedPtr<FJsonValue>> StreamLines = ExecCommandCapture(TEXT("stat Streaming"), WaitMs / 2);
	Root->SetNumberField(TEXT("stat_streaming_lines"), StreamLines.Num());

	Root->SetStringField(TEXT("note"), TEXT("One-shot PIE snapshot. For deep profiling use perf.start_trace + perf.capture_stats_session."));
	return FMonolithActionResult::Success(Root);
}

// =========================================================================================
// ==================== M3-M9 ~95 new handlers — thin console wrappers ====================
// =========================================================================================
// Pattern: most handlers just run a console command and return captured log lines. Shared
// helper `ExecCommandCapture()` does the heavy lifting. PIE-gated handlers use
// `PERF_REQUIRE_PIE()` macro which returns Error if !GEditor->PlayWorld.
//
// For handlers with simple structure, a small local lambda `MakeStdResp(raw_lines)`
// builds the standard response. For handlers with custom parsing (regex extract to
// structured fields), we do it inline.

static TSharedPtr<FJsonObject> MakeStdResp(const TArray<TSharedPtr<FJsonValue>>& Lines, const TCHAR* Note = nullptr)
{
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetNumberField(TEXT("lines_captured"), Lines.Num());
	R->SetArrayField(TEXT("raw_lines"), Lines);
	if (Note) R->SetStringField(TEXT("note"), Note);
	return R;
}

static int32 GetWaitMs(const TSharedPtr<FJsonObject>& Params, int32 Default = 800)
{
	if (Params->HasField(TEXT("wait_ms")))
		return FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("wait_ms"))), 100, 30000);
	return Default;
}

static int32 GetTopN(const TSharedPtr<FJsonObject>& Params, int32 Default = 30, int32 Max = 500)
{
	if (Params->HasField(TEXT("top_n")))
		return FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("top_n"))), 1, Max);
	return Default;
}

// ---------- D2 Stat generic primitives ----------

FMonolithActionResult FMonolithPerfActions::HandleStatListGroups(const TSharedPtr<FJsonObject>& Params)
{
	auto Lines = ExecCommandCapture(TEXT("stat grouplist"), GetWaitMs(Params, 500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleStatDumpframe(const TSharedPtr<FJsonObject>& Params)
{
	PERF_REQUIRE_PIE();
	FString Root;
	if (Params->HasField(TEXT("root"))) Root = Params->GetStringField(TEXT("root"));
	const FString Cmd = Root.IsEmpty() ? TEXT("stat dumpframe") : FString::Printf(TEXT("stat dumpframe -root=%s"), *Root);
	auto Lines = ExecCommandCapture(Cmd, GetWaitMs(Params, 1500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleStatDumpave(const TSharedPtr<FJsonObject>& Params)
{
	PERF_REQUIRE_PIE();
	int32 Frames = 30;
	if (Params->HasField(TEXT("frames"))) Frames = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("frames"))), 2, 500);
	FString Root;
	if (Params->HasField(TEXT("root"))) Root = Params->GetStringField(TEXT("root"));
	FString Cmd = FString::Printf(TEXT("stat dumpave -num=%d"), Frames);
	if (!Root.IsEmpty()) Cmd += FString::Printf(TEXT(" -root=%s"), *Root);
	auto Lines = ExecCommandCapture(Cmd, GetWaitMs(Params, Frames * 50));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleStatDumphitches(const TSharedPtr<FJsonObject>& Params)
{
	auto Lines = ExecCommandCapture(TEXT("stat dumphitches"), GetWaitMs(Params, 500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleStatDumpevents(const TSharedPtr<FJsonObject>& Params)
{
	double ThresholdMs = 2.0;
	if (Params->HasField(TEXT("threshold_ms"))) ThresholdMs = Params->GetNumberField(TEXT("threshold_ms"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("stat dumpevents -ms=%.2f -all"), ThresholdMs), GetWaitMs(Params, 500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleStatSlowConfigure(const TSharedPtr<FJsonObject>& Params)
{
	double ThresholdMs = 5.0;
	if (Params->HasField(TEXT("threshold_ms"))) ThresholdMs = Params->GetNumberField(TEXT("threshold_ms"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("stat slow -ms=%.2f"), ThresholdMs), GetWaitMs(Params, 300));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleStatNamedEvents(const TSharedPtr<FJsonObject>& Params)
{
	const bool bEnable = Params->HasField(TEXT("enable")) ? Params->GetBoolField(TEXT("enable")) : true;
	auto Lines = ExecCommandCapture(bEnable ? TEXT("stat namedevents") : TEXT("stat namedevents -off"), 300);
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleStatUnitPeak(const TSharedPtr<FJsonObject>& Params)
{
	PERF_REQUIRE_PIE();
	auto Lines = ExecCommandCapture(TEXT("stat unitmax"), GetWaitMs(Params, 500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleStatNone(const TSharedPtr<FJsonObject>& Params)
{
	if (GEngine) GEngine->Exec(nullptr, TEXT("stat none"));
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	return FMonolithActionResult::Success(R);
}

// ---------- D2 per-domain wrappers ----------

#define PERF_DOMAIN_WRAPPER(FnName, Cmd, bPieRequired) \
	FMonolithActionResult FMonolithPerfActions::FnName(const TSharedPtr<FJsonObject>& Params) { \
		if (bPieRequired) { PERF_REQUIRE_PIE(); } \
		auto L = ExecCommandCapture(TEXT(Cmd), GetWaitMs(Params)); \
		return FMonolithActionResult::Success(MakeStdResp(L)); }

PERF_DOMAIN_WRAPPER(HandleGetCoreStats,        "stat unit",           true)
PERF_DOMAIN_WRAPPER(HandleGetMemoryStats,      "stat Memory",         false)
PERF_DOMAIN_WRAPPER(HandleGetShadowStats,      "stat ShadowRendering", true)
PERF_DOMAIN_WRAPPER(HandleGetPhysicsStats,     "stat Chaos",          true)
PERF_DOMAIN_WRAPPER(HandleGetAudioStats,       "stat Audio",          true)
PERF_DOMAIN_WRAPPER(HandleGetAnimationStats,   "stat Anim",           true)
PERF_DOMAIN_WRAPPER(HandleGetNetworkStats,     "stat Net",            true)
PERF_DOMAIN_WRAPPER(HandleGetTaskStats,        "stat TaskGraph",      false)

// ---------- D3 GPU extended ----------

FMonolithActionResult FMonolithPerfActions::HandleProfileGPUWithDrawCalls(const TSharedPtr<FJsonObject>& Params)
{
	PERF_REQUIRE_PIE();
	if (GEngine) GEngine->Exec(nullptr, TEXT("r.ProfileGPU.DrawCallEvents 1"));
	auto Lines = ExecCommandCapture(TEXT("ProfileGPU"), GetWaitMs(Params, 1500));
	if (GEngine) GEngine->Exec(nullptr, TEXT("r.ProfileGPU.DrawCallEvents 0"));
	return FMonolithActionResult::Success(MakeStdResp(Lines, TEXT("DrawCallEvents enabled for this capture only")));
}

FMonolithActionResult FMonolithPerfActions::HandleCaptureGPUFrames(const TSharedPtr<FJsonObject>& Params)
{
	PERF_REQUIRE_PIE();
	int32 N = 1;
	if (Params->HasField(TEXT("frames"))) N = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("frames"))), 1, 32);
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("Capture.GPU %d"), N), GetWaitMs(Params, 2000));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

PERF_DOMAIN_WRAPPER(HandleGetGPURealtime,    "stat GPU",         true)
PERF_DOMAIN_WRAPPER(HandleGetGPUMemory,      "stat GPUMemory",   true)
PERF_DOMAIN_WRAPPER(HandleGetNaniteStats,    "stat Nanite",      true)
PERF_DOMAIN_WRAPPER(HandleGetLumenStats,     "stat Lumen",       true)
PERF_DOMAIN_WRAPPER(HandleGetRayTracingStats,"stat RayTracing",  true)
PERF_DOMAIN_WRAPPER(HandleGetVSMStats,       "stat VSM",         true)
PERF_DOMAIN_WRAPPER(HandleGetSubstrateStats, "stat Substrate",   true)

FMonolithActionResult FMonolithPerfActions::HandleListDrawCalls(const TSharedPtr<FJsonObject>& Params)
{
	PERF_REQUIRE_PIE();
	if (GEngine) GEngine->Exec(nullptr, TEXT("r.ProfileGPU.DrawCallEvents 1"));
	auto Lines = ExecCommandCapture(TEXT("ProfileGPU"), GetWaitMs(Params, 2000));
	if (GEngine) GEngine->Exec(nullptr, TEXT("r.ProfileGPU.DrawCallEvents 0"));
	const int32 TopN = GetTopN(Params);
	TArray<TSharedPtr<FJsonValue>> Top;
	for (int32 i = 0; i < FMath::Min(TopN, Lines.Num()); ++i) Top.Add(Lines[i]);
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetNumberField(TEXT("top_n"), TopN);
	R->SetNumberField(TEXT("total_lines"), Lines.Num());
	R->SetArrayField(TEXT("raw_lines"), Top);
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithPerfActions::HandleProfileGPUSetSort(const TSharedPtr<FJsonObject>& Params)
{
	const FString Mode = Params->HasField(TEXT("mode")) ? Params->GetStringField(TEXT("mode")) : TEXT("1");
	if (GEngine) GEngine->Exec(nullptr, *FString::Printf(TEXT("r.ProfileGPU.Sort %s"), *Mode));
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetStringField(TEXT("mode"), Mode);
	return FMonolithActionResult::Success(R);
}

// ---------- D10+D11 Shader/PSO/DDC extended ----------

PERF_DOMAIN_WRAPPER(HandleGetShaderCompileJobs, "r.ShaderCompiler.JobTiming 1", false)
PERF_DOMAIN_WRAPPER(HandleGetPSOCacheStats,     "r.ShaderPipelineCache.DumpStats", false)
PERF_DOMAIN_WRAPPER(HandleGetPSOPrecacheStats,  "r.PSOPrecache.Stats",    false)
PERF_DOMAIN_WRAPPER(HandleGetShaderMemory,      "stat ShaderMemory",      false)
PERF_DOMAIN_WRAPPER(HandleListMaterialsByCost,  "ListMaterials",          false)
PERF_DOMAIN_WRAPPER(HandleDDCDump,              "DumpDDC",                false)

FMonolithActionResult FMonolithPerfActions::HandleGetMaterialPermutationCount(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("material_path"));
	if (Path.IsEmpty()) return FMonolithActionResult::Error(TEXT("material_path required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("DumpMaterialShaderMaps %s"), *Path), GetWaitMs(Params, 1500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleDumpMaterialShaderMap(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("material_path"));
	if (Path.IsEmpty()) return FMonolithActionResult::Error(TEXT("material_path required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("DumpMaterialShaderMaps %s"), *Path), GetWaitMs(Params, 1500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

// Zen HTTP via FHttpModule — synchronous style since MCP call is already async
FMonolithActionResult FMonolithPerfActions::HandleZenHttpStats(const TSharedPtr<FJsonObject>& Params)
{
	const FString Endpoint = Params->HasField(TEXT("endpoint"))
		? Params->GetStringField(TEXT("endpoint"))
		: TEXT("http://[::1]:8558/stats");
	auto Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Endpoint);
	Req->SetVerb(TEXT("GET"));
	Req->SetTimeout(5.0f);
	FString Body;
	bool bOk = false;
	int32 Code = -1;
	Req->OnProcessRequestComplete().BindLambda(
		[&Body, &bOk, &Code](FHttpRequestPtr, FHttpResponsePtr R, bool bConnected)
		{
			if (R.IsValid()) { Code = R->GetResponseCode(); Body = R->GetContentAsString(); bOk = bConnected && Code >= 200 && Code < 400; }
		});
	Req->ProcessRequest();
	const double T0 = FPlatformTime::Seconds();
	while (Req->GetStatus() != EHttpRequestStatus::Succeeded && Req->GetStatus() != EHttpRequestStatus::Failed
		&& FPlatformTime::Seconds() - T0 < 5.0)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.05f);
		FPlatformProcess::Sleep(0.05f);
	}
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("endpoint"), Endpoint);
	R->SetBoolField(TEXT("ok"), bOk);
	R->SetNumberField(TEXT("status_code"), Code);
	if (Body.Len() > 32 * 1024) Body = Body.Left(32 * 1024) + TEXT("...(trunc)");
	R->SetStringField(TEXT("body"), Body);
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithPerfActions::HandleZenHealth(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> NewParams = MakeShared<FJsonObject>();
	NewParams->SetStringField(TEXT("endpoint"), TEXT("http://[::1]:8558/health"));
	return HandleZenHttpStats(NewParams);
}

FMonolithActionResult FMonolithPerfActions::HandleZenListNamespaces(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> NewParams = MakeShared<FJsonObject>();
	NewParams->SetStringField(TEXT("endpoint"), TEXT("http://[::1]:8558/namespaces"));
	return HandleZenHttpStats(NewParams);
}

FMonolithActionResult FMonolithPerfActions::HandleDDCBackendGraph(const TSharedPtr<FJsonObject>& Params)
{
	auto Lines = ExecCommandCapture(TEXT("DerivedDataCache.Dump"), GetWaitMs(Params, 500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

// ---------- D13 Niagara ----------

PERF_DOMAIN_WRAPPER(HandleNiagaraDumpComponents,     "fx.Niagara.DumpComponents",     true)
PERF_DOMAIN_WRAPPER(HandleNiagaraListSystems,        "fx.Niagara.DumpSystemInstances", true)
PERF_DOMAIN_WRAPPER(HandleNiagaraEmitterOptimInfo,   "fx.Niagara.DumpEmitterOptimizationRelevancyInfo", true)
PERF_DOMAIN_WRAPPER(HandleNiagaraSimCacheStats,      "fx.Niagara.SystemSimCacheStats", true)
PERF_DOMAIN_WRAPPER(HandleNiagaraPerSystemCost,      "stat Niagara",                   true)
PERF_DOMAIN_WRAPPER(HandleNiagaraGPUCPUSplit,        "stat Niagara",                   true)

FMonolithActionResult FMonolithPerfActions::HandleNiagaraGetQualityLevel(const TSharedPtr<FJsonObject>& Params)
{
	auto Lines = ExecCommandCapture(TEXT("fx.Niagara.QualityLevel"), GetWaitMs(Params, 300));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleNiagaraDebugHUDToggle(const TSharedPtr<FJsonObject>& Params)
{
	auto Lines = ExecCommandCapture(TEXT("fx.NiagaraDebugHud"), GetWaitMs(Params, 300));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

// ---------- D6+D7 Streaming extended ----------

FMonolithActionResult FMonolithPerfActions::HandleListWaitingTextures(const TSharedPtr<FJsonObject>& Params)
{
	PERF_REQUIRE_PIE();
	auto Lines = ExecCommandCapture(TEXT("ListStreamingTextures"), GetWaitMs(Params, 1500));
	TArray<TSharedPtr<FJsonValue>> Filtered;
	for (const auto& L : Lines)
	{
		const FString S = L->AsString();
		if (S.Contains(TEXT("Waiting")) || S.Contains(TEXT("Streaming"))) Filtered.Add(L);
	}
	const int32 TopN = GetTopN(Params);
	TArray<TSharedPtr<FJsonValue>> Top;
	for (int32 i = 0; i < FMath::Min(TopN, Filtered.Num()); ++i) Top.Add(Filtered[i]);
	return FMonolithActionResult::Success(MakeStdResp(Top));
}

PERF_DOMAIN_WRAPPER(HandleGetStreamingPoolSize,   "r.Streaming.PoolSize",            false)
PERF_DOMAIN_WRAPPER(HandleGetStreamingPoolStatus, "stat StreamingOverview",          true)
PERF_DOMAIN_WRAPPER(HandleDumpTextureStreaming,   "DumpTextureStreamingStats",       true)
PERF_DOMAIN_WRAPPER(HandleForceStreamAllUsed,     "r.Streaming.FullyLoadUsedTextures 1", true)
PERF_DOMAIN_WRAPPER(HandleGetVTResidency,         "r.VT.Residency.Show 1",           true)
PERF_DOMAIN_WRAPPER(HandleDumpVTPoolUsage,        "r.VT.DumpPoolUsage",              true)

FMonolithActionResult FMonolithPerfActions::HandleListStreamingLevels(const TSharedPtr<FJsonObject>& Params)
{
	const bool bUsePIE = !Params->HasField(TEXT("use_pie_world")) || Params->GetBoolField(TEXT("use_pie_world"));
	UWorld* World = GEditor ? (bUsePIE && GEditor->PlayWorld ? ToRawPtr(GEditor->PlayWorld) : GEditor->GetEditorWorldContext().World()) : nullptr;
	if (!World) return FMonolithActionResult::Error(TEXT("no world"));
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (ULevelStreaming* S : World->GetStreamingLevels())
	{
		if (!S) continue;
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("level"), S->GetWorldAssetPackageName());
		E->SetBoolField(TEXT("loaded"), S->IsLevelLoaded());
		E->SetBoolField(TEXT("visible"), S->IsLevelVisible());
		E->SetBoolField(TEXT("should_be_loaded"), S->ShouldBeLoaded());
		Arr.Add(MakeShared<FJsonValueObject>(E));
	}
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetNumberField(TEXT("count"), Arr.Num());
	R->SetArrayField(TEXT("levels"), Arr);
	return FMonolithActionResult::Success(R);
}

PERF_DOMAIN_WRAPPER(HandleWPCellsAtLocation,       "wp.Runtime.ToggleDrawRuntimeHash2D", true)
PERF_DOMAIN_WRAPPER(HandleWPListLoadedCells,       "wp.Runtime.ShowStreamingSources", true)
PERF_DOMAIN_WRAPPER(HandleWPListStreamingSources,  "wp.Runtime.ShowStreamingSources", true)
PERF_DOMAIN_WRAPPER(HandleHLODListTransitions,     "stat HLOD",                       true)
PERF_DOMAIN_WRAPPER(HandleGetHLODStats,            "stat HLOD",                       true)

// ---------- D5 Memory extended ----------

FMonolithActionResult FMonolithPerfActions::HandleParseMemreportSection(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty() || !FPaths::FileExists(Path)) return FMonolithActionResult::Error(TEXT("path required and must exist"));
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *Path)) return FMonolithActionResult::Error(TEXT("failed to read"));
	FString SectionName = TEXT("Objects");
	if (Params->HasField(TEXT("section"))) SectionName = Params->GetStringField(TEXT("section"));
	int32 StartIdx = Content.Find(SectionName, ESearchCase::IgnoreCase);
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	if (StartIdx < 0) { R->SetBoolField(TEXT("ok"), false); R->SetStringField(TEXT("error"), TEXT("section not found")); return FMonolithActionResult::Success(R); }
	const int32 EndIdx = FMath::Min(Content.Len(), StartIdx + 65536);
	R->SetBoolField(TEXT("ok"), true);
	R->SetStringField(TEXT("section"), SectionName);
	R->SetStringField(TEXT("content"), Content.Mid(StartIdx, EndIdx - StartIdx));
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithPerfActions::HandleDiffMemreports(const TSharedPtr<FJsonObject>& Params)
{
	const FString Before = Params->GetStringField(TEXT("before"));
	const FString After = Params->GetStringField(TEXT("after"));
	if (Before.IsEmpty() || After.IsEmpty()) return FMonolithActionResult::Error(TEXT("before/after required"));
	auto Parse = [](const FString& P) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *P)) return S;
		const FString Head = Content.Left(FMath::Min(8192, Content.Len()));
		auto Pull = [&](const TCHAR* Pat, const TCHAR* Key)
		{
			FRegexMatcher M(FRegexPattern(Pat), Head);
			if (M.FindNext())
			{
				FString V = M.GetCaptureGroup(1); V.ReplaceInline(TEXT(","), TEXT(""));
				S->SetNumberField(Key, FCString::Atod(*V));
			}
		};
		Pull(TEXT("Physical Memory:\\s*([\\d,\\.]+)\\s*MB"), TEXT("physical_mb"));
		Pull(TEXT("Virtual Memory:\\s*([\\d,\\.]+)\\s*MB"), TEXT("virtual_mb"));
		return S;
	};
	TSharedPtr<FJsonObject> B = Parse(Before);
	TSharedPtr<FJsonObject> A = Parse(After);
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetObjectField(TEXT("before"), B);
	R->SetObjectField(TEXT("after"), A);
	double BPhys = 0.0, APhys = 0.0;
	B->TryGetNumberField(TEXT("physical_mb"), BPhys);
	A->TryGetNumberField(TEXT("physical_mb"), APhys);
	R->SetNumberField(TEXT("delta_physical_mb"), APhys - BPhys);
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithPerfActions::HandleGetPlatformMemory(const TSharedPtr<FJsonObject>& Params)
{
	FPlatformMemoryStats S = FPlatformMemory::GetStats();
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetNumberField(TEXT("used_physical_mb"), S.UsedPhysical / (1024.0 * 1024.0));
	R->SetNumberField(TEXT("peak_used_physical_mb"), S.PeakUsedPhysical / (1024.0 * 1024.0));
	R->SetNumberField(TEXT("used_virtual_mb"), S.UsedVirtual / (1024.0 * 1024.0));
	R->SetNumberField(TEXT("peak_used_virtual_mb"), S.PeakUsedVirtual / (1024.0 * 1024.0));
	R->SetNumberField(TEXT("available_physical_mb"), S.AvailablePhysical / (1024.0 * 1024.0));
	R->SetNumberField(TEXT("available_virtual_mb"), S.AvailableVirtual / (1024.0 * 1024.0));
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithPerfActions::HandleGetPlatformMemoryConstants(const TSharedPtr<FJsonObject>& Params)
{
	const FPlatformMemoryConstants& C = FPlatformMemory::GetConstants();
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetNumberField(TEXT("total_physical_mb"), C.TotalPhysical / (1024.0 * 1024.0));
	R->SetNumberField(TEXT("total_virtual_mb"), C.TotalVirtual / (1024.0 * 1024.0));
	R->SetNumberField(TEXT("page_size"), C.PageSize);
	R->SetNumberField(TEXT("os_allocation_granularity"), C.OsAllocationGranularity);
	return FMonolithActionResult::Success(R);
}

PERF_DOMAIN_WRAPPER(HandleLLMDumpContext,  "LLM.DumpLLMContext",  false)
PERF_DOMAIN_WRAPPER(HandleLLMListTags,     "LLM.ShowTags",        false)
PERF_DOMAIN_WRAPPER(HandleLLMReport,       "LLM.Report",          false)

FMonolithActionResult FMonolithPerfActions::HandleFindAssetRefs(const TSharedPtr<FJsonObject>& Params)
{
	const FString Asset = Params->GetStringField(TEXT("asset"));
	if (Asset.IsEmpty()) return FMonolithActionResult::Error(TEXT("asset required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("obj refs Name=%s"), *Asset), GetWaitMs(Params, 1500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleDumpClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	int32 Depth = 2;
	if (Params->HasField(TEXT("depth"))) Depth = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("depth"))), 1, 5);
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("obj list hier=%d"), Depth), GetWaitMs(Params, 1500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleGCCollect(const TSharedPtr<FJsonObject>& Params)
{
	auto Lines = ExecCommandCapture(TEXT("GC.ForceCollectGarbageEveryFrame 0"), 200);
	if (GEngine) { GEngine->ForceGarbageCollection(true); }
	return FMonolithActionResult::Success(MakeStdResp(Lines, TEXT("ForceGarbageCollection(true) invoked")));
}

PERF_DOMAIN_WRAPPER(HandleGCDumpReachable,    "GC.DumpPoolStats",      false)
PERF_DOMAIN_WRAPPER(HandleMallocLeakSnapshot, "MallocLeak.Snapshot",   false)
PERF_DOMAIN_WRAPPER(HandleMallocLeakCheck,    "MallocLeak.Check",      false)

// ---------- D8 TaskGraph/Threading / D14 Anim / D15 Physics / D16 Audio / D17 Network ----------

PERF_DOMAIN_WRAPPER(HandleGetTaskgraphStats,           "stat TaskGraph",       false)
PERF_DOMAIN_WRAPPER(HandleGetThreadStats,              "stat Threading",       false)
PERF_DOMAIN_WRAPPER(HandleGetAsyncLoadingThreadState,  "stat Async",           false)
PERF_DOMAIN_WRAPPER(HandleGetParallelForStats,         "stat ParallelTask",    false)
PERF_DOMAIN_WRAPPER(HandleABPPerActorCost,             "a.AnimNode.Stats",     true)
PERF_DOMAIN_WRAPPER(HandleABPDumpStates,               "a.DumpAnimGraphStates", true)
PERF_DOMAIN_WRAPPER(HandleListActiveMontages,          "stat AnimMontage",     true)
PERF_DOMAIN_WRAPPER(HandleGetAnimSharingStats,         "stat AnimationSharing", true)
PERF_DOMAIN_WRAPPER(HandleGetChaosStats,               "stat Chaos",           true)
PERF_DOMAIN_WRAPPER(HandleGetChaosCollisions,          "stat ChaosCollisions", true)
PERF_DOMAIN_WRAPPER(HandleGetActiveRigidbodyCount,     "stat ChaosSolvers",    true)
PERF_DOMAIN_WRAPPER(HandleChaosDumpEvolution,          "DumpChaosEvolutionStats", true)
PERF_DOMAIN_WRAPPER(HandleGetActiveVoiceCount,         "stat AudioChannels",   true)
PERF_DOMAIN_WRAPPER(HandleAudioDumpConcurrency,        "au.DumpConcurrencyStats", true)
PERF_DOMAIN_WRAPPER(HandleAudioListActiveSounds,       "au.DumpActiveSounds",  true)
PERF_DOMAIN_WRAPPER(HandleMetaSoundListActive,         "meta.Sound.DumpActiveSounds", true)
PERF_DOMAIN_WRAPPER(HandleGetNetBandwidth,             "stat NetPackets",      true)
PERF_DOMAIN_WRAPPER(HandleNetRPCByClass,               "stat NetRPC",          true)
PERF_DOMAIN_WRAPPER(HandleNetListConnections,          "stat Net",             true)
PERF_DOMAIN_WRAPPER(HandleNetDumpReplicationGraph,     "net.ReplicationGraphDumpAllRepList", true)

// ---------- CSV Profiler extended ----------

FMonolithActionResult FMonolithPerfActions::HandleCsvBreadcrumb(const TSharedPtr<FJsonObject>& Params)
{
	const FString Name = Params->GetStringField(TEXT("name"));
	if (Name.IsEmpty()) return FMonolithActionResult::Error(TEXT("name required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("CsvProfile breadcrumb %s"), *Name), 200);
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleCsvMetadata(const TSharedPtr<FJsonObject>& Params)
{
	const FString Key = Params->GetStringField(TEXT("key"));
	const FString Value = Params->GetStringField(TEXT("value"));
	if (Key.IsEmpty()) return FMonolithActionResult::Error(TEXT("key required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("CsvProfile metadata %s=%s"), *Key, *Value), 200);
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

PERF_DOMAIN_WRAPPER(HandleCsvListCategories, "CsvProfile list", false)

FMonolithActionResult FMonolithPerfActions::HandleCsvSetCategory(const TSharedPtr<FJsonObject>& Params)
{
	const FString Cat = Params->GetStringField(TEXT("category"));
	const bool bEnable = Params->HasField(TEXT("enable")) ? Params->GetBoolField(TEXT("enable")) : true;
	if (Cat.IsEmpty()) return FMonolithActionResult::Error(TEXT("category required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("CsvProfile %s %s"), bEnable ? TEXT("enable") : TEXT("disable"), *Cat), 200);
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleSummarizeCsv(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty() || !FPaths::FileExists(Path)) return FMonolithActionResult::Error(TEXT("path required"));
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *Path)) return FMonolithActionResult::Error(TEXT("failed to read"));
	TArray<FString> AllLines;
	Content.ParseIntoArrayLines(AllLines);
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetNumberField(TEXT("total_rows"), AllLines.Num());
	R->SetNumberField(TEXT("file_size"), Content.Len());
	if (AllLines.Num() > 0) R->SetStringField(TEXT("header"), AllLines[0]);
	R->SetStringField(TEXT("note"), TEXT("Full CSV statistical summary (mean/p95/max) requires external CSVStats.exe tool; basic row count returned here."));
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithPerfActions::HandleDetectCsvHitches(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty() || !FPaths::FileExists(Path)) return FMonolithActionResult::Error(TEXT("path required"));
	double Threshold = 50.0;
	if (Params->HasField(TEXT("threshold_ms"))) Threshold = Params->GetNumberField(TEXT("threshold_ms"));
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *Path)) return FMonolithActionResult::Error(TEXT("failed to read"));
	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines);
	if (Lines.Num() < 2) return FMonolithActionResult::Error(TEXT("CSV empty"));
	TArray<FString> Header;
	Lines[0].ParseIntoArray(Header, TEXT(","));
	int32 FrameTimeCol = -1;
	for (int32 i = 0; i < Header.Num(); ++i)
	{
		if (Header[i].Contains(TEXT("FrameTime")) || Header[i].Equals(TEXT("FrameTime"))) { FrameTimeCol = i; break; }
	}
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetNumberField(TEXT("threshold_ms"), Threshold);
	if (FrameTimeCol < 0) { R->SetStringField(TEXT("warning"), TEXT("FrameTime column not found")); return FMonolithActionResult::Success(R); }
	TArray<TSharedPtr<FJsonValue>> Hitches;
	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		TArray<FString> Cols;
		Lines[i].ParseIntoArray(Cols, TEXT(","));
		if (FrameTimeCol >= Cols.Num()) continue;
		double MsVal = FCString::Atod(*Cols[FrameTimeCol]);
		if (MsVal > Threshold)
		{
			TSharedPtr<FJsonObject> H = MakeShared<FJsonObject>();
			H->SetNumberField(TEXT("frame"), i);
			H->SetNumberField(TEXT("frame_ms"), MsVal);
			Hitches.Add(MakeShared<FJsonValueObject>(H));
		}
	}
	R->SetNumberField(TEXT("hitch_count"), Hitches.Num());
	R->SetArrayField(TEXT("hitches"), Hitches);
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithPerfActions::HandleCsvListMetrics(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty() || !FPaths::FileExists(Path)) return FMonolithActionResult::Error(TEXT("path required"));
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *Path)) return FMonolithActionResult::Error(TEXT("failed to read"));
	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines);
	TArray<TSharedPtr<FJsonValue>> Cols;
	if (Lines.Num() > 0)
	{
		TArray<FString> Header;
		Lines[0].ParseIntoArray(Header, TEXT(","));
		for (const auto& H : Header) Cols.Add(MakeShared<FJsonValueString>(H));
	}
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetArrayField(TEXT("metrics"), Cols);
	return FMonolithActionResult::Success(R);
}

// ---------- Trace control extended ----------

PERF_DOMAIN_WRAPPER(HandlePauseTrace,       "Trace.Pause",         false)
PERF_DOMAIN_WRAPPER(HandleResumeTrace,      "Trace.Resume",        false)
PERF_DOMAIN_WRAPPER(HandleTraceStatus,      "Trace.Status",        false)
PERF_DOMAIN_WRAPPER(HandleTraceListChannels,"Trace.ListChannels",  false)
PERF_DOMAIN_WRAPPER(HandleTraceSnapshot,    "Trace.Snapshot",      false)

FMonolithActionResult FMonolithPerfActions::HandleTraceBookmark(const TSharedPtr<FJsonObject>& Params)
{
	const FString Name = Params->GetStringField(TEXT("name"));
	if (Name.IsEmpty()) return FMonolithActionResult::Error(TEXT("name required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("Trace.Bookmark %s"), *Name), 200);
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

// ---------- D9 Asset Dependency ----------

FMonolithActionResult FMonolithPerfActions::HandleGetAssetDependencies(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) return FMonolithActionResult::Error(TEXT("path required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("obj dependencies Name=%s"), *Path), GetWaitMs(Params, 1500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleGetAssetReferencers(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) return FMonolithActionResult::Error(TEXT("path required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("obj refs Name=%s"), *Path), GetWaitMs(Params, 1500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

PERF_DOMAIN_WRAPPER(HandleGetAssetRegistryStats, "AssetManager.DumpAssetRegistryInfo", false)

// ---------- D12 Material ----------

FMonolithActionResult FMonolithPerfActions::HandleGetMaterialStats(const TSharedPtr<FJsonObject>& Params)
{
	const FString Path = Params->GetStringField(TEXT("material_path"));
	if (Path.IsEmpty()) return FMonolithActionResult::Error(TEXT("material_path required"));
	auto Lines = ExecCommandCapture(FString::Printf(TEXT("DumpMaterialShaderMaps %s"), *Path), GetWaitMs(Params, 1500));
	return FMonolithActionResult::Success(MakeStdResp(Lines));
}

FMonolithActionResult FMonolithPerfActions::HandleGetMaterialComplexity(const TSharedPtr<FJsonObject>& Params)
{
	auto Lines = ExecCommandCapture(TEXT("r.ShowMaterialStats 1"), GetWaitMs(Params, 500));
	return FMonolithActionResult::Success(MakeStdResp(Lines, TEXT("Material stats overlay enabled; query per-material via MaterialEditor GUI or r.ShowMaterialStats commands")));
}

// ---------- Workflow aggregates ----------

FMonolithActionResult FMonolithPerfActions::HandleGetFullSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	R->SetBoolField(TEXT("pie_active"), IsPerfPIEActive());
	// Memory
	FPlatformMemoryStats S = FPlatformMemory::GetStats();
	R->SetNumberField(TEXT("used_physical_mb"), S.UsedPhysical / (1024.0 * 1024.0));
	R->SetNumberField(TEXT("available_physical_mb"), S.AvailablePhysical / (1024.0 * 1024.0));
	// Frame
	R->SetNumberField(TEXT("frame_ms"), FApp::GetDeltaTime() * 1000.0);
	// Actor count (PIE only)
	if (IsPerfPIEActive() && GEditor && GEditor->PlayWorld)
	{
		int32 Total = 0;
		for (TActorIterator<AActor> It(GEditor->PlayWorld); It; ++It) { if (*It) ++Total; }
		R->SetNumberField(TEXT("total_actors_pie"), Total);
		R->SetNumberField(TEXT("streaming_levels"), GEditor->PlayWorld->GetStreamingLevels().Num());
	}
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithPerfActions::HandleStopAllOverlays(const TSharedPtr<FJsonObject>& Params)
{
	if (GEngine) GEngine->Exec(nullptr, TEXT("stat none"));
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("ok"), true);
	return FMonolithActionResult::Success(R);
}

// ==================== M10 D1: Trace Analysis (in-process via IAnalysisService) ====================
// Uses the PUBLIC IAnalysisService::Analyze() which internally creates ALL module analyzers
// (LoadTime, CPU, GPU, Diagnostics, FileActivity, Memory, Tasks, Log, etc.) then queries
// providers via the public ReadXxxProvider() functions.

#include "Trace/Analyzer.h"
#include "Trace/Analysis.h"
#include "Trace/DataStream.h"
#include "TraceServices/AnalyzerFactories.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Bookmarks.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"
#include "TraceServices/Utils.h"

namespace PerfTraceInternal
{
	// ---- Bookmark provider ----
	struct FBookmarkEntry { FString Name; double Time; };

	class FPerfBookmarkProvider : public TraceServices::IEditableBookmarkProvider
	{
	public:
		virtual void UpdateBookmarkSpec(uint64 BookmarkPoint, const TCHAR* FormatString, const TCHAR* File, int32 Line) override
		{
			Specs.FindOrAdd(BookmarkPoint).FormatString = FormatString;
		}
		virtual void AppendBookmark(uint64 BookmarkPoint, double Time, uint32 CallstackId, const uint8* FormatArgs) override
		{
			auto* S = Specs.Find(BookmarkPoint);
			Bookmarks.Add({S ? S->FormatString : TEXT("<unknown>"), Time});
		}
		virtual void AppendBookmark(uint64 BookmarkPoint, double Time, uint32 CallstackId, const TCHAR* Text) override
		{
			Bookmarks.Add({FString(Text), Time});
		}
		struct FSpec { FString FormatString; };
		TMap<uint64, FSpec> Specs;
		TArray<FBookmarkEntry> Bookmarks;
	};

	// ---- Counter provider ----
	class FPerfCounter : public TraceServices::IEditableCounter
	{
	public:
		virtual void SetName(const TCHAR* N) override { Name = N; }
		virtual void SetGroup(const TCHAR*) override {}
		virtual void SetDescription(const TCHAR*) override {}
		virtual void SetIsFloatingPoint(bool b) override { bFloat = b; }
		virtual void SetIsResetEveryFrame(bool) override {}
		virtual void SetDisplayHint(TraceServices::ECounterDisplayHint) override {}
		virtual void AddValue(double T, int64 V) override { LastInt = V; LastTime = T; }
		virtual void AddValue(double T, double V) override { LastFloat = V; LastTime = T; bFloat = true; }
		virtual void SetValue(double T, int64 V) override { LastInt = V; LastTime = T; }
		virtual void SetValue(double T, double V) override { LastFloat = V; LastTime = T; bFloat = true; }
		FString Name; bool bFloat = false; int64 LastInt = 0; double LastFloat = 0.0; double LastTime = 0.0;
	};

	class FPerfCounterProvider : public TraceServices::IEditableCounterProvider
	{
	public:
		virtual const TraceServices::ICounter* GetCounter(TraceServices::IEditableCounter*) override { return nullptr; }
		virtual TraceServices::IEditableCounter* CreateEditableCounter() override
		{
			Counters.Add(MakeUnique<FPerfCounter>());
			return Counters.Last().Get();
		}
		virtual void AddCounter(const TraceServices::ICounter*) override {}
		TArray<TUniquePtr<FPerfCounter>> Counters;
	};

	// ---- CPU profiler: scope duration collector (ported from SummarizeTraceUtils) ----
	struct FPerfScopeStats
	{
		FString Name;
		uint64 Count = 0;
		double TotalSec = 0.0;
		double MinSec = TNumericLimits<double>::Max();
		double MaxSec = 0.0;
		void Add(double Dur)
		{
			Count++;
			TotalSec += Dur;
			MinSec = FMath::Min(MinSec, Dur);
			MaxSec = FMath::Max(MaxSec, Dur);
		}
	};

	/** CPU profiler provider implementing IEditableTimingProfilerProvider + IEditableThreadProvider.
	 *  Collects per-scope inclusive timing. Ported from Engine's FSummarizeCpuProfilerProvider pattern. */
	class FPerfCpuProvider
		: public TraceServices::IEditableTimingProfilerProvider
		, public TraceServices::IEditableThreadProvider
	{
	public:
		// -- IEditableTimingProfilerProvider --
		virtual uint32 AddTimer(TraceServices::ETimingProfilerTimerType Type) override
		{
			if (Type == TraceServices::ETimingProfilerTimerType::CPU)
			{
				uint32 Id = ScopeNames.Num();
				ScopeNames.Add(TOptional<FString>());
				return Id;
			}
			return 0;
		}

		virtual uint32 AddCpuTimer(FStringView Name, const TCHAR* File, uint32 Line) override
		{
			uint32 Id = ScopeNames.Num();
			ScopeNames.Add(FString(Name));
			return Id;
		}

		virtual void SetTimerName(uint32 TimerId, FStringView Name) override
		{
			if (TimerId < (uint32)ScopeNames.Num())
			{
				ScopeNames[TimerId] = FString(Name);
			}
		}

		virtual uint32 AddMetadata(uint32 MasterTimerId, TArray<uint8>&& Metadata) override
		{
			uint32 MetaId = MetadataStore.Num();
			MetadataStore.Add({MoveTemp(Metadata), MasterTimerId});
			return ~MetaId;
		}

		virtual TArrayView<uint8> GetEditableMetadata(uint32 TimerId) override
		{
			if (int32(TimerId) >= 0) return {};
			TimerId = ~TimerId;
			if (TimerId >= (uint32)MetadataStore.Num()) return {};
			return MetadataStore[TimerId].Payload;
		}

		virtual TraceServices::IEditableTimeline<TraceServices::FTimingProfilerEvent>& GetCpuThreadEditableTimeline(uint32 ThreadId) override
		{
			auto* Found = Threads.Find(ThreadId);
			if (Found) return *(Found->Get());
			return *Threads.Add(ThreadId, MakeUnique<FThread>(ThreadId, this));
		}

		// -- IEditableThreadProvider --
		virtual void AddThread(uint32 Id, const TCHAR* Name, EThreadPriority Priority) override
		{
			if (!Threads.Find(Id))
			{
				Threads.Add(Id, MakeUnique<FThread>(Id, this));
			}
			ThreadCount++;
		}

		// ---- Results ----
		const FString* LookupScopeName(uint32 ScopeId)
		{
			if (int32(ScopeId) < 0)
			{
				ScopeId = MetadataStore[~ScopeId].TimerId;
			}
			if (ScopeId < (uint32)ScopeNames.Num() && ScopeNames[ScopeId].IsSet())
			{
				return &ScopeNames[ScopeId].GetValue();
			}
			return nullptr;
		}

		TMap<FString, FPerfScopeStats> GetAggregatedScopes() const
		{
			return AggregatedScopes;
		}

		int32 ThreadCount = 0;

	private:
		struct FMetadata { TArray<uint8> Payload; uint32 TimerId; };
		TArray<FMetadata> MetadataStore;
		TArray<TOptional<FString>> ScopeNames;
		TMap<FString, FPerfScopeStats> AggregatedScopes;

		struct FScopeEnter { uint32 ScopeId; double StartTime; };

		struct FThread : public TraceServices::IEditableTimeline<TraceServices::FTimingProfilerEvent>
		{
			FThread(uint32 InId, FPerfCpuProvider* InP) : ThreadId(InId), Provider(InP) {}
			uint32 ThreadId;
			FPerfCpuProvider* Provider;
			TArray<FScopeEnter> ScopeStack;

			virtual void AppendBeginEvent(double StartTime, const TraceServices::FTimingProfilerEvent& Event) override
			{
				ScopeStack.Add({Event.TimerIndex, StartTime});
			}
			virtual void AppendEndEvent(double EndTime) override
			{
				if (ScopeStack.IsEmpty()) return;
				FScopeEnter Top = ScopeStack.Pop();
				double Dur = EndTime - Top.StartTime;
				// Guard against mismatched/corrupt scope pairs
				if (!FMath::IsFinite(Dur) || Dur < 0.0 || Dur > 3600.0) return;
				const FString* Name = Provider->LookupScopeName(Top.ScopeId);
				if (Name && !Name->IsEmpty())
				{
					Provider->AggregatedScopes.FindOrAdd(*Name).Name = *Name;
					Provider->AggregatedScopes[*Name].Add(Dur);
				}
			}
		};

		TMap<uint32, TUniquePtr<FThread>> Threads;
	};

	// ---- Custom IAnalyzer #1: Diagnostics (session metadata via Session2 event) ----
	class FPerfDiagnosticsAnalyzer : public UE::Trace::IAnalyzer
	{
	public:
		FString Platform, AppName, ProjectName, CommandLine, Branch, BuildVersion;
		uint32 Changelist = 0; bool bHasData = false;
		virtual void OnAnalysisBegin(const FOnAnalysisContext& Context) override
		{ Context.InterfaceBuilder.RouteEvent(0, "Diagnostics", "Session2"); }
		virtual bool OnEvent(uint16 RouteId, EStyle, const FOnEventContext& Context) override
		{
			const auto& D = Context.EventData;
			D.GetString("Platform", Platform); D.GetString("AppName", AppName);
			D.GetString("ProjectName", ProjectName); D.GetString("CommandLine", CommandLine);
			D.GetString("Branch", Branch); D.GetString("BuildVersion", BuildVersion);
			Changelist = D.GetValue<uint32>("Changelist", 0); bHasData = true;
			return true;
		}
	};

	// ---- Custom IAnalyzer #2: Frame counter (count BeginFrame events, not BeginGameFrame) ----
	class FPerfFrameAnalyzer : public UE::Trace::IAnalyzer
	{
	public:
		int32 GameFrames = 0, RenderFrames = 0;
		virtual void OnAnalysisBegin(const FOnAnalysisContext& Context) override
		{
			// Both specific (Game/Render) and generic frame events
			Context.InterfaceBuilder.RouteEvent(0, "Misc", "BeginGameFrame");
			Context.InterfaceBuilder.RouteEvent(1, "Misc", "BeginRenderFrame");
			Context.InterfaceBuilder.RouteEvent(2, "Misc", "BeginFrame"); // fallback generic
		}
		virtual bool OnEvent(uint16 RouteId, EStyle, const FOnEventContext&) override
		{
			if (RouteId == 0) GameFrames++;
			else if (RouteId == 1) RenderFrames++;
			else if (RouteId == 2) { if (GameFrames == 0) GameFrames++; } // generic fallback
			return true;
		}
	};

	// ---- Custom IAnalyzer #3: Log — two-phase: collect specs (with verbosity), then count messages ----
	class FPerfLogAnalyzer : public UE::Trace::IAnalyzer
	{
	public:
		int32 TotalMessages = 0, Errors = 0, Warnings = 0;
		TMap<uint64, uint8> SpecVerbosity; // LogPoint → Verbosity
		virtual void OnAnalysisBegin(const FOnAnalysisContext& Context) override
		{
			Context.InterfaceBuilder.RouteEvent(0, "Logging", "LogMessageSpec");
			Context.InterfaceBuilder.RouteEvent(1, "Logging", "LogMessage");
		}
		virtual bool OnEvent(uint16 RouteId, EStyle, const FOnEventContext& Context) override
		{
			const auto& D = Context.EventData;
			if (RouteId == 0) // LogMessageSpec — carries verbosity
			{
				uint64 LogPoint = D.GetValue<uint64>("LogPoint", 0);
				uint8 Verbosity = D.GetValue<uint8>("Verbosity", 5); // 5 = Log
				SpecVerbosity.Add(LogPoint, Verbosity);
			}
			else if (RouteId == 1) // LogMessage — reference spec by LogPoint
			{
				TotalMessages++;
				uint64 LogPoint = D.GetValue<uint64>("LogPoint", 0);
				uint8* V = SpecVerbosity.Find(LogPoint);
				uint8 Verbosity = V ? *V : 5;
				if (Verbosity == 0 || Verbosity == 1) Errors++; // Fatal=0, Error=1
				else if (Verbosity == 2) Warnings++; // Warning=2
			}
			return true;
		}
	};

	// ---- Custom IAnalyzer #4: LoadTime — track per-package load duration ----
	struct FPackageLoadInfo { FString Name; double StartSec; double EndSec; };
	class FPerfLoadTimeAnalyzer : public UE::Trace::IAnalyzer
	{
	public:
		// Two maps: Ptr→Name for name resolution, Ptr→StartTime for duration
		TMap<uint64, FString> PtrToName;
		TMap<uint64, double> PtrToStart;
		TArray<FPackageLoadInfo> Completed;
		int32 AsyncCount = 0;
		virtual void OnAnalysisBegin(const FOnAnalysisContext& Context) override
		{
			Context.InterfaceBuilder.RouteEvent(0, "LoadTime", "NewAsyncPackage");
			Context.InterfaceBuilder.RouteEvent(1, "LoadTime", "DestroyAsyncPackage");
			Context.InterfaceBuilder.RouteEvent(2, "LoadTime", "PackageSummary");
			Context.InterfaceBuilder.RouteEvent(3, "LoadTime", "BeginRequest");
			Context.InterfaceBuilder.RouteEvent(4, "LoadTime", "EndRequest");
		}
		virtual bool OnEvent(uint16 RouteId, EStyle, const FOnEventContext& Context) override
		{
			const auto& D = Context.EventData;
			double T = Context.EventTime.AsSeconds();
			if (RouteId == 0) // NewAsyncPackage
			{
				uint64 Ptr = D.GetValue<uint64>("AsyncPackage", 0);
				PtrToStart.Add(Ptr, T);
				AsyncCount++;
				FString Name = TraceServices::FTraceAnalyzerUtils::LegacyAttachmentString<TCHAR>("Name", Context);
				if (!Name.IsEmpty()) PtrToName.Add(Ptr, MoveTemp(Name));
			}
			else if (RouteId == 1) // DestroyAsyncPackage
			{
				uint64 Ptr = D.GetValue<uint64>("AsyncPackage", 0);
				FString* N = PtrToName.Find(Ptr);
				double* S = PtrToStart.Find(Ptr);
				if (N && S && !N->IsEmpty()) Completed.Add({*N, *S, T});
				PtrToStart.Remove(Ptr);
			}
			else if (RouteId == 2) // PackageSummary
			{
				uint64 Ptr = D.GetValue<uint64>("AsyncPackage", 0);
				FString Name = TraceServices::FTraceAnalyzerUtils::LegacyAttachmentString<TCHAR>("Name", Context);
				if (!Name.IsEmpty()) PtrToName.Add(Ptr, MoveTemp(Name));
			}
			else if (RouteId == 3) // BeginRequest — additional start marker
			{
				uint64 Id = D.GetValue<uint64>("RequestId", 0);
				if (Id != 0) PtrToStart.Add(Id, T);
			}
			else if (RouteId == 4) // EndRequest
			{
				uint64 Id = D.GetValue<uint64>("RequestId", 0);
				double* S = PtrToStart.Find(Id);
				if (S)
				{
					FString Name;
					D.GetString("Name", Name);
					if (!Name.IsEmpty()) Completed.Add({MoveTemp(Name), *S, T});
					PtrToStart.Remove(Id);
				}
			}
			return true;
		}
	};

	// ---- Custom IAnalyzer #5: File I/O counter ----
	class FPerfFileIOAnalyzer : public UE::Trace::IAnalyzer
	{
	public:
		int32 Reads = 0, Writes = 0, Opens = 0; uint64 BytesRead = 0, BytesWritten = 0;
		virtual void OnAnalysisBegin(const FOnAnalysisContext& Context) override
		{
			Context.InterfaceBuilder.RouteEvent(0, "PlatformFile", "EndRead");
			Context.InterfaceBuilder.RouteEvent(1, "PlatformFile", "EndWrite");
			Context.InterfaceBuilder.RouteEvent(2, "PlatformFile", "EndOpen");
		}
		virtual bool OnEvent(uint16 RouteId, EStyle, const FOnEventContext& Context) override
		{
			if (RouteId == 0) { Reads++; BytesRead += Context.EventData.GetValue<uint64>("SizeRead", 0); }
			else if (RouteId == 1) { Writes++; BytesWritten += Context.EventData.GetValue<uint64>("SizeWritten", 0); }
			else { Opens++; }
			return true;
		}
	};

	// ---- Custom IAnalyzer #6: LLM Memory tags (per-tag values from trace) ----
	struct FLLMTagEntry { FString Name; int64 ParentId; };
	struct FLLMTagSnapshot { int64 TagId; int64 Value; double Time; };
	class FPerfLLMAnalyzer : public UE::Trace::IAnalyzer
	{
	public:
		TMap<int64, FLLMTagEntry> Tags;
		TMap<int64, int64> LatestValues; // TagId → latest value (bytes)
		virtual void OnAnalysisBegin(const FOnAnalysisContext& Context) override
		{
			Context.InterfaceBuilder.RouteEvent(0, "LLM", "TagsSpec");
			Context.InterfaceBuilder.RouteEvent(1, "LLM", "TagValue");
		}
		virtual bool OnEvent(uint16 RouteId, EStyle, const FOnEventContext& Context) override
		{
			const auto& D = Context.EventData;
			if (RouteId == 0) // TagsSpec
			{
				int64 TagId = D.GetValue<int64>("TagId", 0);
				int64 ParentId = D.GetValue<int64>("ParentId", 0);
				FString Name = TraceServices::FTraceAnalyzerUtils::LegacyAttachmentString<TCHAR>("Name", Context);
				Tags.Add(TagId, {MoveTemp(Name), ParentId});
			}
			else if (RouteId == 1) // TagValue — array of tag IDs + values
			{
				const auto& TagArr = D.GetArray<int64>("Tags");
				const auto& ValArr = D.GetArray<int64>("Values");
				int32 Count = FMath::Min(TagArr.Num(), ValArr.Num());
				const int64* TagData = TagArr.GetData();
				const int64* ValData = ValArr.GetData();
				if (TagData && ValData)
				{
					for (int32 i = 0; i < Count; ++i)
					{
						LatestValues.Add(TagData[i], ValData[i]);
					}
				}
			}
			return true;
		}
	};

	// ---- Custom IAnalyzer #7: Memory Allocation tracker (alloc/free counting + total size) ----
	class FPerfAllocAnalyzer : public UE::Trace::IAnalyzer
	{
	public:
		int64 AllocCount = 0, FreeCount = 0;
		int64 TotalAllocBytes = 0, TotalFreeBytes = 0;
		int64 PeakLiveBytes = 0, CurrentLiveBytes = 0;
		TMap<uint64, uint64> LiveAllocs; // Address → Size (for tracking peak)

		virtual void OnAnalysisBegin(const FOnAnalysisContext& Context) override
		{
			Context.InterfaceBuilder.RouteEvent(0, "Memory", "Alloc");
			Context.InterfaceBuilder.RouteEvent(1, "Memory", "AllocSystem");
			Context.InterfaceBuilder.RouteEvent(2, "Memory", "AllocVideo");
			Context.InterfaceBuilder.RouteEvent(3, "Memory", "Free");
			Context.InterfaceBuilder.RouteEvent(4, "Memory", "FreeSystem");
			Context.InterfaceBuilder.RouteEvent(5, "Memory", "FreeVideo");
		}
		virtual bool OnEvent(uint16 RouteId, EStyle, const FOnEventContext& Context) override
		{
			const auto& D = Context.EventData;
			if (RouteId <= 2) // Alloc
			{
				uint64 Address = D.GetValue<uint64>("Address", 0);
				uint64 SizeUpper = D.GetValue<uint32>("Size", 0);
				uint8 AlignSizeLower = D.GetValue<uint8>("AlignmentPow2_SizeLower", 0);
				uint64 Size = (SizeUpper << 4) | (uint64)(AlignSizeLower & 0xF); // SizeShift=4
				AllocCount++;
				TotalAllocBytes += Size;
				CurrentLiveBytes += Size;
				if (CurrentLiveBytes > PeakLiveBytes) PeakLiveBytes = CurrentLiveBytes;
				LiveAllocs.Add(Address, Size);
			}
			else // Free
			{
				uint64 Address = D.GetValue<uint64>("Address", 0);
				FreeCount++;
				if (uint64* Size = LiveAllocs.Find(Address))
				{
					TotalFreeBytes += *Size;
					CurrentLiveBytes -= *Size;
					LiveAllocs.Remove(Address);
				}
			}
			return true;
		}
	};

} // namespace PerfTraceInternal

TSharedPtr<FJsonObject> FMonolithPerfActions::RunTraceAnalysis(const FString& TracePath, int32 TopN)
{
	using namespace PerfTraceInternal;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!FPaths::FileExists(TracePath))
	{
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("File not found: %s"), *TracePath));
		return Result;
	}

	int64 FileSize = IFileManager::Get().FileSize(*TracePath);

	// Open trace file
	UE::Trace::FFileDataStream* DataStream = new UE::Trace::FFileDataStream();
	if (!DataStream->Open(*TracePath))
	{
		delete DataStream;
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("error"), TEXT("Failed to open .utrace file"));
		return Result;
	}

	// Create independent session (no singleton conflict)
	TSharedPtr<TraceServices::IAnalysisSession> Session = TraceServices::CreateAnalysisSession(
		0, nullptr, TUniquePtr<UE::Trace::IInDataStream>(DataStream));

	UE::Trace::FAnalysisContext AnalysisContext;

	// 1. Bookmarks provider
	FPerfBookmarkProvider BookmarkProvider;
	TSharedPtr<UE::Trace::IAnalyzer> BookmarkAnalyzer = TraceServices::CreateBookmarksAnalyzer(*Session, BookmarkProvider);
	AnalysisContext.AddAnalyzer(*BookmarkAnalyzer);

	// 2. Counters provider (captures ALL stat counters: LLM, Memory, GPU, RDG, Shader, DDC, etc.)
	FPerfCounterProvider CounterProvider;
	TSharedPtr<UE::Trace::IAnalyzer> CounterAnalyzer = TraceServices::CreateCountersAnalyzer(*Session, CounterProvider);
	AnalysisContext.AddAnalyzer(*CounterAnalyzer);

	// 3. CPU profiler (timing + threads)
	FPerfCpuProvider CpuProvider;
	TSharedPtr<UE::Trace::IAnalyzer> CpuAnalyzer = TraceServices::CreateCpuProfilerAnalyzer(*Session, CpuProvider, CpuProvider);
	AnalysisContext.AddAnalyzer(*CpuAnalyzer);

	// 4-8. Custom analyzers for data not covered by public factories
	FPerfDiagnosticsAnalyzer DiagAnalyzer;
	AnalysisContext.AddAnalyzer(DiagAnalyzer);
	FPerfFrameAnalyzer FrameAnalyzer;
	AnalysisContext.AddAnalyzer(FrameAnalyzer);
	FPerfLogAnalyzer LogAnalyzer;
	AnalysisContext.AddAnalyzer(LogAnalyzer);
	FPerfLoadTimeAnalyzer LoadTimeAnalyzer;
	AnalysisContext.AddAnalyzer(LoadTimeAnalyzer);
	FPerfFileIOAnalyzer FileIOAnalyzer;
	AnalysisContext.AddAnalyzer(FileIOAnalyzer);
	FPerfLLMAnalyzer LLMAnalyzer;
	AnalysisContext.AddAnalyzer(LLMAnalyzer);
	FPerfAllocAnalyzer AllocAnalyzer;
	AnalysisContext.AddAnalyzer(AllocAnalyzer);

	// Process (blocking)
	double T0 = FPlatformTime::Seconds();
	UE::Trace::FAnalysisProcessor Processor = AnalysisContext.Process(*DataStream);
	Processor.Wait();
	double AnalysisDuration = FPlatformTime::Seconds() - T0;

	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("trace_path"), TracePath);
	Result->SetNumberField(TEXT("file_size_mb"), FileSize / (1024.0 * 1024.0));
	Result->SetNumberField(TEXT("analysis_duration_sec"), AnalysisDuration);
	Result->SetNumberField(TEXT("thread_count"), CpuProvider.ThreadCount);

	// ======== CPU Timing: top timers by total time ========
	TMap<FString, FPerfScopeStats> Scopes = CpuProvider.GetAggregatedScopes();
	TArray<FPerfScopeStats> ScopeArr;
	for (auto& P : Scopes) { if (P.Value.Count > 0) ScopeArr.Add(P.Value); }

	ScopeArr.Sort([](const FPerfScopeStats& A, const FPerfScopeStats& B) { return A.TotalSec > B.TotalSec; });
	TArray<TSharedPtr<FJsonValue>> TimerArr;
	for (int32 i = 0, N = FMath::Min(TopN, ScopeArr.Num()); i < N; ++i)
	{
		const auto& S = ScopeArr[i];
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), S.Name);
		Obj->SetNumberField(TEXT("count"), (double)S.Count);
		Obj->SetNumberField(TEXT("total_sec"), S.TotalSec);
		Obj->SetNumberField(TEXT("max_ms"), S.MaxSec * 1000.0);
		Obj->SetNumberField(TEXT("avg_ms"), (S.TotalSec / S.Count) * 1000.0);
		TimerArr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Result->SetArrayField(TEXT("top_timers_by_total"), TimerArr);
	Result->SetNumberField(TEXT("total_scope_count"), ScopeArr.Num());

	// ======== CPU Timing: top hitches by max single-call ========
	ScopeArr.Sort([](const FPerfScopeStats& A, const FPerfScopeStats& B) { return A.MaxSec > B.MaxSec; });
	TArray<TSharedPtr<FJsonValue>> HitchArr;
	for (int32 i = 0, N = FMath::Min(TopN, ScopeArr.Num()); i < N; ++i)
	{
		const auto& S = ScopeArr[i];
		if (S.MaxSec < 0.001) continue;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), S.Name);
		Obj->SetNumberField(TEXT("max_ms"), S.MaxSec * 1000.0);
		Obj->SetNumberField(TEXT("count"), (double)S.Count);
		Obj->SetNumberField(TEXT("total_sec"), S.TotalSec);
		HitchArr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Result->SetArrayField(TEXT("top_hitches_by_max"), HitchArr);

	// ======== Bookmarks with text + time ========
	TArray<TSharedPtr<FJsonValue>> BmArr;
	int32 BmLimit = TopN * 2;
	for (int32 i = 0, N = FMath::Min(BmLimit, BookmarkProvider.Bookmarks.Num()); i < N; ++i)
	{
		const auto& B = BookmarkProvider.Bookmarks[i];
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), B.Name);
		Obj->SetNumberField(TEXT("time_sec"), B.Time);
		BmArr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Result->SetArrayField(TEXT("bookmarks"), BmArr);
	Result->SetNumberField(TEXT("bookmark_count"), BookmarkProvider.Bookmarks.Num());

	// ======== Counters with actual values ========
	struct FCounterVal { FString Name; double Value; bool bIsMemory; };
	TArray<FCounterVal> AllCounters;
	for (const auto& Ctr : CounterProvider.Counters)
	{
		if (Ctr->Name.IsEmpty()) continue;
		double Val = Ctr->bFloat ? Ctr->LastFloat : (double)Ctr->LastInt;
		AllCounters.Add({Ctr->Name, Val, false});
	}
	AllCounters.Sort([](const FCounterVal& A, const FCounterVal& B) { return FMath::Abs(A.Value) > FMath::Abs(B.Value); });

	TArray<TSharedPtr<FJsonValue>> CtrArr;
	for (int32 i = 0, N = FMath::Min(TopN * 5, AllCounters.Num()); i < N; ++i)
	{
		const auto& C = AllCounters[i];
		if (C.Value == 0.0) continue;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), C.Name);
		Obj->SetNumberField(TEXT("value"), C.Value);
		CtrArr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Result->SetArrayField(TEXT("counters"), CtrArr);
	Result->SetNumberField(TEXT("counter_count"), CounterProvider.Counters.Num());

	// ======== Diagnostics ========
	if (DiagAnalyzer.bHasData)
	{
		TSharedPtr<FJsonObject> Diag = MakeShared<FJsonObject>();
		Diag->SetStringField(TEXT("platform"), DiagAnalyzer.Platform);
		Diag->SetStringField(TEXT("app_name"), DiagAnalyzer.AppName);
		Diag->SetStringField(TEXT("project_name"), DiagAnalyzer.ProjectName);
		Diag->SetStringField(TEXT("branch"), DiagAnalyzer.Branch);
		Diag->SetStringField(TEXT("build_version"), DiagAnalyzer.BuildVersion);
		Diag->SetNumberField(TEXT("changelist"), DiagAnalyzer.Changelist);
		Diag->SetStringField(TEXT("command_line"), DiagAnalyzer.CommandLine);
		Result->SetObjectField(TEXT("diagnostics"), Diag);
	}

	// ======== Frames (prefer CPU timer "Frame" scope count, fallback to event counter) ========
	{
		int32 FrameScopeCount = 0;
		auto* FrameScope = Scopes.Find(TEXT("Frame"));
		if (FrameScope) FrameScopeCount = (int32)FrameScope->Count;
		Result->SetNumberField(TEXT("game_frame_count"), FrameScopeCount > 0 ? FrameScopeCount : FrameAnalyzer.GameFrames);
		Result->SetNumberField(TEXT("render_frame_count"), FrameAnalyzer.RenderFrames);
	}

	// ======== Log ========
	TSharedPtr<FJsonObject> LogSummary = MakeShared<FJsonObject>();
	LogSummary->SetNumberField(TEXT("total"), LogAnalyzer.TotalMessages);
	LogSummary->SetNumberField(TEXT("errors"), LogAnalyzer.Errors);
	LogSummary->SetNumberField(TEXT("warnings"), LogAnalyzer.Warnings);
	Result->SetObjectField(TEXT("log"), LogSummary);

	// ======== LoadTime packages (top by duration) ========
	if (LoadTimeAnalyzer.Completed.Num() > 0)
	{
		LoadTimeAnalyzer.Completed.Sort([](const FPackageLoadInfo& A, const FPackageLoadInfo& B)
		{
			return (A.EndSec - A.StartSec) > (B.EndSec - B.StartSec);
		});
		TArray<TSharedPtr<FJsonValue>> PkgArr;
		for (int32 i = 0, N = FMath::Min(TopN, LoadTimeAnalyzer.Completed.Num()); i < N; ++i)
		{
			const auto& P = LoadTimeAnalyzer.Completed[i];
			double DurMs = (P.EndSec - P.StartSec) * 1000.0;
			if (DurMs < 0.0) continue;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("package"), P.Name);
			Obj->SetNumberField(TEXT("duration_ms"), DurMs);
			PkgArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Result->SetArrayField(TEXT("top_packages_by_load_time"), PkgArr);
	}
	Result->SetNumberField(TEXT("async_package_count"), LoadTimeAnalyzer.AsyncCount);

	// ======== File I/O ========
	TSharedPtr<FJsonObject> FileIO = MakeShared<FJsonObject>();
	FileIO->SetNumberField(TEXT("reads"), FileIOAnalyzer.Reads);
	FileIO->SetNumberField(TEXT("writes"), FileIOAnalyzer.Writes);
	FileIO->SetNumberField(TEXT("opens"), FileIOAnalyzer.Opens);
	FileIO->SetNumberField(TEXT("bytes_read_mb"), FileIOAnalyzer.BytesRead / (1024.0 * 1024.0));
	FileIO->SetNumberField(TEXT("bytes_written_mb"), FileIOAnalyzer.BytesWritten / (1024.0 * 1024.0));
	Result->SetObjectField(TEXT("file_io"), FileIO);

	// ======== LLM Memory Tags (from trace) ========
	if (LLMAnalyzer.Tags.Num() > 0)
	{
		// Merge tag names with latest values, sort by abs value descending
		struct FLLMEntry { FString Name; int64 Bytes; };
		TArray<FLLMEntry> LLMEntries;
		for (const auto& Pair : LLMAnalyzer.LatestValues)
		{
			FLLMTagEntry* Tag = LLMAnalyzer.Tags.Find(Pair.Key);
			FString TagName = Tag ? Tag->Name : FString::Printf(TEXT("Tag_%lld"), Pair.Key);
			LLMEntries.Add({MoveTemp(TagName), Pair.Value});
		}
		LLMEntries.Sort([](const FLLMEntry& A, const FLLMEntry& B) { return FMath::Abs(A.Bytes) > FMath::Abs(B.Bytes); });

		TArray<TSharedPtr<FJsonValue>> LLMArr;
		for (int32 i = 0, N = FMath::Min(TopN * 3, LLMEntries.Num()); i < N; ++i)
		{
			const auto& E = LLMEntries[i];
			if (E.Bytes == 0) continue;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("tag"), E.Name);
			Obj->SetNumberField(TEXT("bytes"), (double)E.Bytes);
			Obj->SetNumberField(TEXT("mb"), E.Bytes / (1024.0 * 1024.0));
			LLMArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Result->SetArrayField(TEXT("llm_tags"), LLMArr);
		Result->SetNumberField(TEXT("llm_tag_count"), LLMAnalyzer.Tags.Num());
	}

	// ======== Memory Allocations (from trace) ========
	if (AllocAnalyzer.AllocCount > 0)
	{
		TSharedPtr<FJsonObject> MemObj = MakeShared<FJsonObject>();
		MemObj->SetNumberField(TEXT("alloc_count"), (double)AllocAnalyzer.AllocCount);
		MemObj->SetNumberField(TEXT("free_count"), (double)AllocAnalyzer.FreeCount);
		MemObj->SetNumberField(TEXT("total_alloc_mb"), AllocAnalyzer.TotalAllocBytes / (1024.0 * 1024.0));
		MemObj->SetNumberField(TEXT("total_free_mb"), AllocAnalyzer.TotalFreeBytes / (1024.0 * 1024.0));
		MemObj->SetNumberField(TEXT("peak_live_mb"), AllocAnalyzer.PeakLiveBytes / (1024.0 * 1024.0));
		MemObj->SetNumberField(TEXT("current_live_mb"), AllocAnalyzer.CurrentLiveBytes / (1024.0 * 1024.0));
		MemObj->SetNumberField(TEXT("leak_count"), (double)AllocAnalyzer.LiveAllocs.Num());
		Result->SetObjectField(TEXT("memory_allocations"), MemObj);
	}

	return Result;
}

FMonolithActionResult FMonolithPerfActions::HandleAnalyzeTrace(const TSharedPtr<FJsonObject>& Params)
{
	FString TracePath = Params->GetStringField(TEXT("trace_path"));
	if (TracePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("trace_path is required"));
	}

	// Resolve relative paths
	if (!TracePath.Contains(TEXT(":")) && !TracePath.StartsWith(TEXT("/")))
	{
		TracePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Profiling"), TracePath);
	}

	int32 TopN = 30;
	if (Params->HasField(TEXT("top_n")))
	{
		TopN = FMath::Clamp((int32)Params->GetNumberField(TEXT("top_n")), 5, 200);
	}

	TSharedPtr<FJsonObject> Result = RunTraceAnalysis(TracePath, TopN);
	if (!Result.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Trace analysis returned null"));
	}

	return FMonolithActionResult::Success(Result);
}

// ==================== M10 D5: LLM Programmatic Tag Values ====================

#include "HAL/LowLevelMemTracker.h"

FMonolithActionResult FMonolithPerfActions::HandleLLMTagValues(const TSharedPtr<FJsonObject>& Params)
{
#if ENABLE_LOW_LEVEL_MEM_TRACKER
	FString TrackerStr = TEXT("Default");
	if (Params->HasField(TEXT("tracker")))
	{
		TrackerStr = Params->GetStringField(TEXT("tracker"));
	}

	ELLMTracker Tracker = TrackerStr.Equals(TEXT("Platform"), ESearchCase::IgnoreCase)
		? ELLMTracker::Platform : ELLMTracker::Default;

	TMap<FName, uint64> TagsWithAmounts;
	FLowLevelMemTracker::Get().GetTrackedTagsNamesWithAmount(TagsWithAmounts, Tracker, ELLMTagSet::None);

	// Sort by size descending
	TagsWithAmounts.ValueSort([](const uint64& A, const uint64& B) { return A > B; });

	int32 TopN = 100;
	if (Params->HasField(TEXT("top_n")))
	{
		TopN = FMath::Clamp((int32)Params->GetNumberField(TEXT("top_n")), 5, 500);
	}

	TArray<TSharedPtr<FJsonValue>> TagArr;
	int64 TotalBytes = 0;
	int32 Count = 0;
	for (const auto& Pair : TagsWithAmounts)
	{
		if (Count >= TopN) break;
		if (Pair.Value == 0) continue;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("tag"), Pair.Key.ToString());
		Obj->SetNumberField(TEXT("bytes"), (double)Pair.Value);
		Obj->SetNumberField(TEXT("mb"), Pair.Value / (1024.0 * 1024.0));
		TagArr.Add(MakeShared<FJsonValueObject>(Obj));
		TotalBytes += Pair.Value;
		Count++;
	}

	uint64 TotalTracked = FLowLevelMemTracker::Get().GetTotalTrackedMemory(Tracker);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("tracker"), TrackerStr);
	Result->SetNumberField(TEXT("total_tracked_mb"), TotalTracked / (1024.0 * 1024.0));
	Result->SetNumberField(TEXT("tag_count"), TagsWithAmounts.Num());
	Result->SetArrayField(TEXT("tags"), TagArr);
	return FMonolithActionResult::Success(Result);
#else
	return FMonolithActionResult::Error(TEXT("LLM tracker not enabled in this build (ENABLE_LOW_LEVEL_MEM_TRACKER=0)"));
#endif
}

#undef PERF_DOMAIN_WRAPPER
#undef PERF_REQUIRE_PIE
