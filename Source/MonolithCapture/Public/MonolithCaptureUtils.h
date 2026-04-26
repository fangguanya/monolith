#pragma once

#include "CoreMinimal.h"
#include "ImageCore.h"

class UObject;

/*
 * 这份头文件定义的是 Monolith 内部唯一的"资产 canonical 预览渲染"接口。
 *
 * 历史背景：
 * - 旧实现把整套 SCC2D + RenderTarget + 相机 framing 直接写在 capture action 里；
 * - 后来 AssetVisual 索引器也需要一模一样的渲染，差点就出现"两套预览渲染"。
 *
 * v2 扩展：
 * - 接口接受 `UObject* Asset`，内部按运行时类型分发到 4 条具体渲染管线：
 *     UStaticMesh         → 静态网格 mesh component + bounds-fit 相机
 *     USkeletalMesh       → 骨骼网格 mesh component + ref pose bounds
 *     UMaterialInterface  → 把 material 贴到固定球体上 + 固定相机
 *     UWidgetBlueprint    → SWidget → Texture，rasterize 后降采样到目标尺寸
 * - 所有 4 类输出统一为 256×256 BGRA8 sRGB（geometric 默认）或 224×224（semantic 默认），
 *   同一接口同一渲染 recipe，下游 AssetVisual cohort 看不到差异。
 *
 * 接口职责：
 * - 给一份 UObject 资产，返回若干张稳定的预览图；
 * - 所有可影响图像内容的参数（视图、相机、背景、光照、seed）都被压缩进
 *   `RenderRecipeVersion`，方便 AssetVisual cohort 用它判定 stale。
 *
 * 调用方约束：
 * - 必须在游戏线程调用；接口实现内部会 Tick 编辑器世界、注册临时组件；
 * - 渲染单张图大致 200ms 量级，调用方需要自己安排在 BackgroundCpuPool 之外的合适时机。
 */
namespace MonolithCapture
{
	/** canonical 视图集合：5 视图覆盖前后左右 + 等距，足以重构 silhouette。
	 *  对 Material / WidgetBlueprint 这类没有 3D bounds 概念的资产，
	 *  渲染器只接受 Iso 这一种视图（其他视图会被忽略）。 */
	enum class ECanonicalView : uint8
	{
		/** 正前方水平视角。 */
		Front,
		/** 右侧水平视角。 */
		Side,
		/** 正后方水平视角。 */
		Back,
		/** 自顶向下俯视。 */
		Top,
		/** 等距 3/4 视角，给 semantic provider 单图使用，所有资产类都支持。 */
		Iso,
	};

	/** 单张 canonical 渲染的输出，已统一成 BGRA8 sRGB 内存图像。 */
	struct FCanonicalRenderResult
	{
		/** 这张图对应的视角。 */
		ECanonicalView View = ECanonicalView::Iso;
		/** 颜色图像，BGRA8 sRGB；分辨率与请求的 resolution 一致。 */
		FImage ColorImage;
		/** silhouette mask，单通道 8bit；非 0 像素表示前景；和 ColorImage 同分辨率。 */
		FImage SilhouetteImage;
	};

	/** 渲染请求参数；这些字段共同定义 `render_recipe_version`。 */
	struct FCanonicalRenderRequest
	{
		/** 要渲染的资产；必须有效，类型支持 `UStaticMesh / USkeletalMesh / UMaterialInterface / UWidgetBlueprint`。
		 *  其他类型会被渲染器拒绝并返回 false。 */
		UObject* Asset = nullptr;
		/** 输出分辨率；正方形分辨率，默认 256，给 AssetVisual cohort 用。 */
		int32 Resolution = 256;
		/** 想要的视图集合；空表示只渲染 Iso 一张。
		 *  注：Material / WidgetBlueprint 资产只支持 Iso 一张视图，其他视图会被 silently 忽略。 */
		TArray<ECanonicalView> Views;
		/** 是否同时计算 silhouette（geometric provider 必需，semantic 不需要）。 */
		bool bComputeSilhouette = false;
	};

	/*
	 * IAssetCanonicalRenderer 是"资产 canonical 预览渲染"在整个 Monolith 内的
	 * 唯一抽象入口。
	 *
	 * MonolithCapture 提供默认实现 `FAssetCanonicalRenderer`；
	 * MonolithMesh 的 capture action 与 MonolithIndex 的 AssetVisual indexer
	 * 都通过这个接口拿渲染结果，禁止再各自 new 一份 SCC2D 渲染逻辑。
	 */
	class MONOLITHCAPTURE_API IAssetCanonicalRenderer
	{
	public:
		virtual ~IAssetCanonicalRenderer() = default;

		/** 渲染一组 canonical 视图，返回内存图像。
		 * 失败时返回 false，OutResults 内容不定。
		 * 必须在游戏线程调用。
		 * 内部按 Asset 运行时类型分发到 mesh / skel / material / widget 四条渲染管线。 */
		virtual bool RenderCanonical(
			const FCanonicalRenderRequest& Request,
			TArray<FCanonicalRenderResult>& OutResults) = 0;

		/** 把一张已渲染的图保存为 PNG 到指定路径。
		 * 这个 helper 只是为了让 capture action 不必再各自实现 PNG 序列化，
		 * 不属于核心渲染语义。 */
		virtual bool SaveImageAsPng(const FImage& Image, const FString& OutputPath) = 0;

		/** 当前 helper 实现使用的 render recipe 版本号。
		 * 任何会影响图像内容的代码改动（FOV / 背景 / 光照 / seed / 视图集合 / 分辨率 /
		 * 任一资产类型的渲染管线细节）必须 bump 这个版本号；
		 * AssetVisual cohort 把它写进 artifact Meta，版本不匹配立刻整库 stale。 */
		virtual uint32 GetRenderRecipeVersion() const = 0;
	};

	/** 取用进程内的全局 helper 实例（线程安全）。 */
	MONOLITHCAPTURE_API IAssetCanonicalRenderer& GetAssetCanonicalRenderer();
}
