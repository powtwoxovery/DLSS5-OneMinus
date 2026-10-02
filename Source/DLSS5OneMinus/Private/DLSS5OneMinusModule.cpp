#include "DLSS5OneMinusModule.h"

#include "DLSS5OneMinusInternal.h"
#include "DLSS5OneMinusSettingsManager.h"
#include "DLSS5OneMinusViewExtension.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY_STATIC(LogDLSS5OneMinus, Log, All);

namespace
{
    FCriticalSection GStatusMutex;
    FDLSS5OneMinusStatusSnapshot GStatus;
    TAtomic<uint64> GTargetViewId { 0 };
    TAtomic<uint64> GTargetRenderTargetId { 0 };
    TSharedPtr<FDLSS5OneMinusViewExtension, ESPMode::ThreadSafe> GViewExtension;
    FDelegateHandle GPostEngineInitHandle;
    IConsoleObject* GOwnershipProbeCommand = nullptr;

    void CreateViewExtension()
    {
        if (GEngine != nullptr && !GViewExtension.IsValid())
        {
            GViewExtension = FSceneViewExtensions::NewExtension<FDLSS5OneMinusViewExtension>();
        }
    }
}

namespace DLSS5OneMinus
{
    FDLSS5OneMinusStatusSnapshot GetStatus()
    {
        FScopeLock Lock(&GStatusMutex);
        return GStatus;
    }

    void SetStatus(const FDLSS5OneMinusStatusSnapshot& Status)
    {
        FScopeLock Lock(&GStatusMutex);
        GStatus = Status;
    }

    void SetTargetViewId(const uint64 ViewId)
    {
        const uint64 PreviousViewId = GTargetViewId.Exchange(ViewId);
        if (PreviousViewId != ViewId)
        {
            InvalidateOutputStatus(TEXT("Rendering target changed; waiting for fresh NR output."));
        }
    }

    uint64 GetTargetViewId()
    {
        return GTargetViewId.Load();
    }

    void SetTargetRenderTargetId(const uint64 RenderTargetId)
    {
        GTargetRenderTargetId.Store(RenderTargetId);
    }

    uint64 GetTargetRenderTargetId()
    {
        return GTargetRenderTargetId.Load();
    }

    void InvalidateOutputStatus(const TCHAR* Reason)
    {
        FScopeLock Lock(&GStatusMutex);
        GStatus.bOutputSubmitted = false;
        GStatus.bOutputCompleted = false;
        GStatus.SubmittedFrameNumber = 0;
        GStatus.SubmittedViewId = 0;
        GStatus.SubmittedConfigGeneration = 0;
        GStatus.LastSubmittedFence = 0;
        GStatus.EffectiveRoute = TEXT("None");
        if (Reason != nullptr)
        {
            GStatus.Detail = Reason;
        }
    }

    const TCHAR* LexToString(const EDLSS5OneMinusRuntimeState State)
    {
        switch (State)
        {
        case EDLSS5OneMinusRuntimeState::Disabled: return TEXT("Disabled");
        case EDLSS5OneMinusRuntimeState::MissingVendor: return TEXT("Missing Vendor");
        case EDLSS5OneMinusRuntimeState::MissingUnofficialRuntime: return TEXT("Missing Unofficial Runtime");
        case EDLSS5OneMinusRuntimeState::DependenciesReady: return TEXT("Dependencies Ready");
        case EDLSS5OneMinusRuntimeState::Probing: return TEXT("Probing");
        case EDLSS5OneMinusRuntimeState::Initializing: return TEXT("Initializing");
        case EDLSS5OneMinusRuntimeState::WaitingForCreateFence: return TEXT("Waiting For Create Fence");
        case EDLSS5OneMinusRuntimeState::Ready: return TEXT("Ready");
        case EDLSS5OneMinusRuntimeState::Retiring: return TEXT("Retiring");
        case EDLSS5OneMinusRuntimeState::FaultedQuarantined: return TEXT("Faulted / Quarantined");
        default: return TEXT("Unknown");
        }
    }
}

void FDLSS5OneMinusModule::StartupModule()
{
    const TSharedPtr<IPlugin> ThisPlugin = IPluginManager::Get().FindPlugin(TEXT("DLSS5OneMinus"));
    if (!ThisPlugin.IsValid())
    {
        UE_LOG(LogDLSS5OneMinus, Error, TEXT("DLSS5-OneMinus could not resolve its plugin descriptor."));
        return;
    }

    AddShaderSourceDirectoryMapping(
        TEXT("/Plugin/DLSS5OneMinus"),
        FPaths::Combine(ThisPlugin->GetBaseDir(), TEXT("Shaders")));

    const TSharedPtr<IPlugin> DLSSPlugin = IPluginManager::Get().FindPlugin(TEXT("DLSS"));
    const FString UnofficialRuntimeDir = FPaths::Combine(
        FPaths::ProjectPluginsDir(),
        TEXT("DLSS5/DLSSNRRuntime/Win64"));
    const bool bHasRuntime = FPaths::FileExists(FPaths::Combine(UnofficialRuntimeDir, TEXT("nvngx_dlssnr.dll")));
    const bool bHasAdapter = FPaths::FileExists(FPaths::Combine(UnofficialRuntimeDir, TEXT("nvngx.dll_research_adapter.dll")));

    FDLSS5OneMinusStatusSnapshot Status;
    Status.bVendorPluginAvailable = DLSSPlugin.IsValid();
    Status.bUnofficialRuntimeAvailable = bHasRuntime && bHasAdapter;

    if (!Status.bVendorPluginAvailable)
    {
        Status.State = EDLSS5OneMinusRuntimeState::MissingVendor;
        Status.Detail = TEXT("Install the authorized clean NVIDIA DLSS plugin under Plugins/NVIDIA.");
    }
    else if (!Status.bUnofficialRuntimeAvailable)
    {
        Status.State = EDLSS5OneMinusRuntimeState::MissingUnofficialRuntime;
        Status.Detail = TEXT("Optional unofficial runtime is absent from Plugins/DLSS5/DLSSNRRuntime/Win64.");
    }
    else
    {
        Status.State = EDLSS5OneMinusRuntimeState::DependenciesReady;
        Status.Detail = TEXT("Dependencies found; NGX ownership is not yet proven and NR remains disabled.");
    }

    DLSS5OneMinus::SetStatus(Status);
    GOwnershipProbeCommand = IConsoleManager::Get().RegisterConsoleCommand(
        TEXT("DLSSNR.ProbeOwnership"),
        TEXT("Run the bounded DLSS5-OneMinus D3D12 NR snippet bootstrap. Reuses the loaded stock core, initializes/populates only the snippet, and does not create or evaluate a feature."),
        FConsoleCommandDelegate::CreateStatic(&DLSS5OneMinus::RunNGXSnippetBootstrap),
        ECVF_Default);
    if (GEngine != nullptr)
    {
        CreateViewExtension();
    }
    else
    {
        GPostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&CreateViewExtension);
    }

    UE_LOG(LogDLSS5OneMinus, Display, TEXT("DLSS5-OneMinus loaded: state=%s; %s"),
        DLSS5OneMinus::LexToString(Status.State), *Status.Detail);
}

void FDLSS5OneMinusModule::ShutdownModule()
{
    DLSS5OneMinus::ResetAllSettingsState();
    if (GOwnershipProbeCommand != nullptr)
    {
        IConsoleManager::Get().UnregisterConsoleObject(GOwnershipProbeCommand);
        GOwnershipProbeCommand = nullptr;
    }
    if (GPostEngineInitHandle.IsValid())
    {
        FCoreDelegates::OnPostEngineInit.Remove(GPostEngineInitHandle);
        GPostEngineInitHandle.Reset();
    }
    GViewExtension.Reset();
}

IMPLEMENT_MODULE(FDLSS5OneMinusModule, DLSS5OneMinus)
