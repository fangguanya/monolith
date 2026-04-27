#include "Embedders/ClipSemanticEmbeddingProvider.h"
#include "Embedders/ClipPieSuspender.h"
#include "Embedders/ClipVramBudgetGuard.h"
#include "AssetVisualShardedRetriever.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Hash/Blake3.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#include "MonolithIndexLog.h"

/*
 * Clip semantic provider 实现。
 *
 * 编译期通过 WITH_MONOLITH_NNE 分流：
 *  - WITH_MONOLITH_NNE=1：完整调用 UE 5.7 NNE / NNERuntimeORT 跑 GPU (DirectML) 推理；
 *  - WITH_MONOLITH_NNE=0：所有方法存在但 IsAvailable=false；search 自动只用 geometric cohort。
 *
 * 运行期 IsAvailable=false 的几种情况（等价于"semantic cohort stale-only"）：
 *  - PIE 中（GPU 让给 game world）
 *  - ONNX 模型文件不存在
 *  - NNE 运行时不可用 / 模型加载失败
 *  - VRAM 预算耗尽
 *
 * 这些"不可用"路径都符合 spec 的 cohort_stale 一等支持语义；调用方按需把 mesh 重投 OfflineOnly 队列。
 */
#if WITH_MONOLITH_NNE
// UE 5.7 NNE 公共 API。模型对象、运行时句柄、推理 instance、张量描述都在 UE::NNE 命名空间下。
#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntime.h"
#include "NNERuntimeGPU.h"
#include "NNERuntimeRunSync.h"
#include "NNETypes.h"
#include "NNEStatus.h"
#endif

namespace ClipSemanticInternal
{
	/** CLIP-ViT-B/32 输入尺寸固定 224×224 BGR float。 */
	static constexpr int32 ClipInputSize = 224;
	/** 输出维度。 */
	static constexpr int32 ClipEmbeddingDim = 512;
	/** Provider 标识符常量。 */
	static const FName ClipProviderId = FName(TEXT("clip_vit_b32_v1"));
	/** UE 5.7 NNE GPU runtime 注册名（DirectML 后端，Windows + 4070 默认）。 */
	static const FString NneGpuRuntimeName = TEXT("NNERuntimeORTDml");

	/** ONNX 模型路径 = `Plugins/Monolith/Resources/Models/clip_vit_b32_image_encoder_fp16.onnx`。 */
	static FString ResolveOnnxPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		const FString PluginRoot = Plugin.IsValid()
			? Plugin->GetBaseDir()
			: FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"));
		return FPaths::Combine(PluginRoot, TEXT("Resources"), TEXT("Models"), TEXT("clip_vit_b32_image_encoder_fp16.onnx"));
	}

	/** CLIP-ViT-B/32 官方 image preprocessing 参数（RGB 通道顺序，0-255 像素值）。
	 *  来源：OpenAI CLIP repo `clip/clip.py::_transform`，与 OpenCLIP 一致。
	 *  原始值（RGB 0-1 域）：mean=[0.48145466, 0.4578275, 0.40821073]，
	 *                        std =[0.26862954, 0.26130258, 0.27577711]
	 *  这里按 ×255 缩放到 0-255 域，避免推理前再做一次除法。 */
	static constexpr float ClipMeanRGB[3] = { 122.7706f, 116.7460f, 104.0937f };
	static constexpr float ClipStdRGB[3]  = {  68.5005f,  66.6322f,  70.3232f };

	/** 把 64 位 hash 截到 32 位作为 ProviderVersion；同模型必产同版本。 */
	static uint32 HashOnnxToProviderVersion(const FString& OnnxPath)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *OnnxPath))
		{
			return 1; // 文件不存在时返回固定版本号；IsAvailable 会兜底拦截
		}
		FBlake3 Hasher;
		Hasher.Update(Bytes.GetData(), Bytes.Num());
		const FBlake3Hash Hash = Hasher.Finalize();
		const uint8* HashBytes = static_cast<const uint8*>(Hash.GetBytes());
		const uint32 V =
			(static_cast<uint32>(HashBytes[0]) << 24)
			| (static_cast<uint32>(HashBytes[1]) << 16)
			| (static_cast<uint32>(HashBytes[2]) << 8)
			| static_cast<uint32>(HashBytes[3]);
		// 0 是"未加载"语义保留值，避免和未加载状态撞值。
		return V == 0 ? 1u : V;
	}
}

#if WITH_MONOLITH_NNE
/** 把 NNE 相关句柄打包到 PIMPL 里，避免 NNE 头文件污染 .h 文件。
 *  整个 NNE 集成只在这一处发生；NNE API 演进只影响这一个文件。
 *  注意：INNERuntimeGPU 是全局 UInterface 类，不在 UE::NNE 命名空间；
 *        IModelGPU / IModelInstanceGPU 才在 UE::NNE 下。 */
struct FClipSemanticEmbeddingProvider::FNneState
{
	/** UE NNE GPU 运行时弱引用（DirectML）。 */
	TWeakInterfacePtr<INNERuntimeGPU> Runtime;
	/** 已经加载的 ONNX 模型对象（TStrongObjectPtr 防止 GC）。 */
	TStrongObjectPtr<UNNEModelData> ModelData;
	/** 已构造的 GPU 模型。 */
	TSharedPtr<UE::NNE::IModelGPU> Model;
	/** 推理 instance；线程安全前提下可在 BackgroundCpuPool 上跑。 */
	TSharedPtr<UE::NNE::IModelInstanceGPU> Instance;
};
#endif

FClipSemanticEmbeddingProvider::FClipSemanticEmbeddingProvider()
{
	OnnxModelPath = ClipSemanticInternal::ResolveOnnxPath();
	Info.ProviderId = ClipSemanticInternal::ClipProviderId;
	Info.ProviderVersion = 1;
	Info.EmbeddingDim = ClipSemanticInternal::ClipEmbeddingDim;
	Info.bL2Normalized = true;

	BudgetGuard = MakeUnique<FClipVramBudgetGuard>();
	PieSuspender = MakeUnique<FClipPieSuspender>();
	PieSuspender->Register();
}

FClipSemanticEmbeddingProvider::~FClipSemanticEmbeddingProvider()
{
	if (PieSuspender.IsValid())
	{
		PieSuspender->Unregister();
	}
}

bool FClipSemanticEmbeddingProvider::IsAvailable() const
{
	// PIE 期间整个 provider 不可用，避免和 game world 抢 GPU。
	if (PieSuspender.IsValid() && PieSuspender->IsPaused())
	{
		return false;
	}

	// 主动触发一次 LoadModelIfNeeded：原本它只在 Encode 第一次调时才 lazy load，
	// 但 indexer 流程是 IsAvailable 为 true 才进 Encode → 形成 chicken-and-egg：
	// 永远不 Encode → 永远不 load → IsAvailable 永远 false → 整 cohort 静默 stale。
	// 这里 const_cast 是因为接口 const 但 LoadModelIfNeeded 走 mutex 内部修改私有状态。
	const_cast<FClipSemanticEmbeddingProvider*>(this)->LoadModelIfNeeded();

	FScopeLock Lock(&LoadMutex);
	return bLoadAttempted && bLoadSucceeded;
}

void FClipSemanticEmbeddingProvider::LoadModelIfNeeded()
{
	FScopeLock Lock(&LoadMutex);
	if (bLoadAttempted)
	{
		return;
	}
	bLoadAttempted = true;
	bLoadSucceeded = false;

	// 模型文件不存在 → 直接放弃（geometric provider 仍可用）。
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*OnnxModelPath))
	{
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("ClipSemanticEmbeddingProvider: ONNX 模型文件不存在 (%s)，semantic cohort 将永久 stale-only"),
			*OnnxModelPath);
		return;
	}

	// 把 ProviderVersion 锁到模型 Blake3 哈希（任意字节变化 → cohort 全量 stale）。
	Info.ProviderVersion = ClipSemanticInternal::HashOnnxToProviderVersion(OnnxModelPath);

#if WITH_MONOLITH_NNE
	using namespace UE::NNE;

	// 1) 读 ONNX 字节（UNNEModelData::Init 接收 TConstArrayView64，直接 view 即可，无需 MoveTemp）
	TArray<uint8> ModelBytes;
	if (!FFileHelper::LoadFileToArray(ModelBytes, *OnnxModelPath))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("ClipSemanticEmbeddingProvider: 读 ONNX 文件失败 (%s)"), *OnnxModelPath);
		return;
	}

	// 2) 取 GPU 运行时（DirectML 后端）
	NneState = MakeUnique<FNneState>();
	NneState->Runtime = GetRuntime<INNERuntimeGPU>(ClipSemanticInternal::NneGpuRuntimeName);
	if (!NneState->Runtime.IsValid())
	{
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("ClipSemanticEmbeddingProvider: NNE GPU runtime '%s' 不可用，semantic cohort 永久 stale-only"),
			*ClipSemanticInternal::NneGpuRuntimeName);
		NneState.Reset();
		return;
	}

	// 3) 创建 UNNEModelData transient 对象承载 ONNX 字节
	UNNEModelData* ModelDataObj = NewObject<UNNEModelData>(GetTransientPackage());
	if (!ModelDataObj)
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("ClipSemanticEmbeddingProvider: 创建 UNNEModelData 失败"));
		NneState.Reset();
		return;
	}
	ModelDataObj->Init(TEXT("onnx"), TConstArrayView64<uint8>(ModelBytes.GetData(), ModelBytes.Num()));
	NneState->ModelData = TStrongObjectPtr<UNNEModelData>(ModelDataObj);

	// 4) 让 runtime 编译模型为 GPU IModelGPU
	NneState->Model = NneState->Runtime->CreateModelGPU(NneState->ModelData.Get());
	if (!NneState->Model.IsValid())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("ClipSemanticEmbeddingProvider: NNE CreateModelGPU 失败"));
		NneState.Reset();
		return;
	}

	// 5) 创建可重用 instance（每次 Encode 共用同一份 GPU 资源）
	NneState->Instance = NneState->Model->CreateModelInstanceGPU();
	if (!NneState->Instance.IsValid())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("ClipSemanticEmbeddingProvider: CreateModelInstanceGPU 失败"));
		NneState.Reset();
		return;
	}

	// 6) 锁定输入张量形状 1×3×224×224。CLIP-ViT-B/32 是固定输入尺寸，不需要每次 SetInputTensorShapes。
	// 状态枚举走 IModelInstanceRunSync 父类的 EResultStatus，不是 IModelInstanceGPU 自己的。
	const uint32 ShapeData[4] = {
		1u, 3u,
		static_cast<uint32>(ClipSemanticInternal::ClipInputSize),
		static_cast<uint32>(ClipSemanticInternal::ClipInputSize)
	};
	const TArray<FTensorShape> InputShapes = {
		FTensorShape::Make(TConstArrayView<uint32>(ShapeData, UE_ARRAY_COUNT(ShapeData)))
	};
	if (NneState->Instance->SetInputTensorShapes(InputShapes) != EResultStatus::Ok)
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("ClipSemanticEmbeddingProvider: SetInputTensorShapes 失败"));
		NneState.Reset();
		return;
	}

	bLoadSucceeded = true;
	UE_LOG(LogMonolithIndex, Log,
		TEXT("ClipSemanticEmbeddingProvider: ONNX 模型加载成功 (provider_version=%u, path=%s)"),
		Info.ProviderVersion, *OnnxModelPath);
#else
	UE_LOG(LogMonolithIndex, Log,
		TEXT("ClipSemanticEmbeddingProvider: 当前编译未启用 WITH_MONOLITH_NNE，semantic cohort 永久 stale-only"));
	bLoadSucceeded = false;
#endif
}

bool FClipSemanticEmbeddingProvider::BuildClipInputTensor(const FImage& ColorImage, TArray<float>& OutNCHW) const
{
	if (ColorImage.SizeX <= 0 || ColorImage.SizeY <= 0)
	{
		return false;
	}

	using namespace ClipSemanticInternal;
	const int32 K = ClipInputSize;
	OutNCHW.SetNumZeroed(3 * K * K);

	// FColor 在 UE 里按 BGRA 字节序存（小端 uint32 也是 BGRA→ARGB），
	// 但字段名 R/G/B/A 已经语义化，可以直接按颜色取值。
	const FColor* Src = reinterpret_cast<const FColor*>(ColorImage.RawData.GetData());
	const int32 SrcW = ColorImage.SizeX;
	const int32 SrcH = ColorImage.SizeY;

	// NCHW RGB float32：CLIP-ViT-B/32 image encoder 训练时用 RGB 输入 + CLIP mean/std。
	// 通道索引 0=R, 1=G, 2=B（与 ONNX 模型 pixel_values 一致）。
	// 简单 nearest 缩放；CLIP 训练时用 bicubic，对 256→224 输出 nearest 导致的质量下降可忽略。
	for (int32 Y = 0; Y < K; ++Y)
	{
		const int32 SrcY = (Y * SrcH) / K;
		for (int32 X = 0; X < K; ++X)
		{
			const int32 SrcX = (X * SrcW) / K;
			const FColor& C = Src[SrcY * SrcW + SrcX];
			OutNCHW[0 * K * K + Y * K + X] = (static_cast<float>(C.R) - ClipMeanRGB[0]) / ClipStdRGB[0];
			OutNCHW[1 * K * K + Y * K + X] = (static_cast<float>(C.G) - ClipMeanRGB[1]) / ClipStdRGB[1];
			OutNCHW[2 * K * K + Y * K + X] = (static_cast<float>(C.B) - ClipMeanRGB[2]) / ClipStdRGB[2];
		}
	}
	return true;
}

bool FClipSemanticEmbeddingProvider::Encode(
	const FImage& ColorImage,
	const FImage& SilhouetteImage,
	TArray<float>& OutEmbedding)
{
	(void)SilhouetteImage; // CLIP 自带视角不变，不需要 silhouette。

	LoadModelIfNeeded();
	if (!IsAvailable())
	{
		return false;
	}

	int32 GrantedBatch = 0;
	if (!BudgetGuard.IsValid() || !BudgetGuard->TryAcquire(1, GrantedBatch))
	{
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("ClipSemanticEmbeddingProvider: VRAM 预算已满，无法预留 batch=1"));
		return false;
	}

	bool bOk = false;
	{
		// RAII 释放预算
		ON_SCOPE_EXIT
		{
			if (BudgetGuard.IsValid())
			{
				BudgetGuard->Release(GrantedBatch);
			}
		};

		TArray<float> InputTensor;
		if (!BuildClipInputTensor(ColorImage, InputTensor))
		{
			return false;
		}

#if WITH_MONOLITH_NNE
		using namespace UE::NNE;

		if (!NneState.IsValid() || !NneState->Instance.IsValid())
		{
			return false;
		}

		OutEmbedding.SetNumZeroed(ClipSemanticInternal::ClipEmbeddingDim);

		// 输入 / 输出张量绑定：
		// - input  : NCHW float32, 1×3×224×224
		// - output : 1×512 float32 embedding
		// UE 5.7 把 GPU 推理接口收敛到 FTensorBindingCPU（GPU runtime 内部管理上传/下载）
		FTensorBindingCPU InputBinding;
		InputBinding.Data = InputTensor.GetData();
		InputBinding.SizeInBytes = InputTensor.Num() * sizeof(float);

		FTensorBindingCPU OutputBinding;
		OutputBinding.Data = OutEmbedding.GetData();
		OutputBinding.SizeInBytes = OutEmbedding.Num() * sizeof(float);

		const TConstArrayView<FTensorBindingCPU> InputBindings = MakeArrayView(&InputBinding, 1);
		const TConstArrayView<FTensorBindingCPU> OutputBindings = MakeArrayView(&OutputBinding, 1);
		// RunSync 状态枚举走 IModelInstanceRunSync 父类的 EResultStatus。
		const EResultStatus RunStatus = NneState->Instance->RunSync(InputBindings, OutputBindings);
		if (RunStatus != EResultStatus::Ok)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("ClipSemanticEmbeddingProvider: RunSync 失败 (status=%d)"),
				static_cast<int32>(RunStatus));
			return false;
		}
		bOk = true;
#else
		(void)InputTensor;
		bOk = false;
#endif
	}

	if (bOk)
	{
		L2NormalizeInPlace(OutEmbedding);
	}
	return bOk;
}
