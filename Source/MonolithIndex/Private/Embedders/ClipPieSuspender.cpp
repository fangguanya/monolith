#include "Embedders/ClipPieSuspender.h"

#include "Editor.h"

void FClipPieSuspender::Register()
{
	if (BeginPieHandle.IsValid())
	{
		return;
	}
	// 同时监听 BeginPIE / EndPIE；BeginPIE/EndPIE 都会带 bIsSimulating 参数表明 PIE 模式。
	BeginPieHandle = FEditorDelegates::BeginPIE.AddRaw(this, &FClipPieSuspender::HandleBeginPie);
	EndPieHandle = FEditorDelegates::EndPIE.AddRaw(this, &FClipPieSuspender::HandleEndPie);
}

void FClipPieSuspender::Unregister()
{
	if (BeginPieHandle.IsValid())
	{
		FEditorDelegates::BeginPIE.Remove(BeginPieHandle);
		BeginPieHandle.Reset();
	}
	if (EndPieHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(EndPieHandle);
		EndPieHandle.Reset();
	}
}

bool FClipPieSuspender::IsPaused() const
{
	FScopeLock Lock(&Mutex);
	return bIsPaused;
}

void FClipPieSuspender::HandleBeginPie(const bool bInIsSimulating)
{
	(void)bInIsSimulating;
	FScopeLock Lock(&Mutex);
	bIsPaused = true;
}

void FClipPieSuspender::HandleEndPie(const bool bInIsSimulating)
{
	(void)bInIsSimulating;
	FScopeLock Lock(&Mutex);
	bIsPaused = false;
}
