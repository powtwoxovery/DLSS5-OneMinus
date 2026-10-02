#include "DLSS5OneMinusInternal.h"
#include "DLSS5OneMinusRuntimeD3D12.h"

#include "HAL/PlatformFileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "ID3D12DynamicRHI.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Paths.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_params.h"

#include <bcrypt.h>

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDLSS5OneMinusNGXBootstrap, Log, All);

namespace
{
    constexpr TCHAR ExpectedSnippetSHA256[] = TEXT("6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927");
    constexpr TCHAR ExpectedAdapterSHA256[] = TEXT("adc01160ddad23183b9390efc38829e0412ca0a798ee390a1245bc80e18db400");

#if DLSS5ONEMINUS_WITH_D3D12
    using FCoreGetCapabilityParameters = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter**);
    using FSnippetInit = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    using FSnippetPopulate = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter*);
    using FSnippetCreate = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using FSnippetEvaluate = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
    using FSnippetRelease = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle*);
    using FSnippetGetApplicationId = unsigned long long (NVSDK_CONV *)();
    using FSnippetGetAPIVersion = NVSDK_NGX_Version (NVSDK_CONV *)();
    using FAdapterInit = NVSDK_NGX_Result (__cdecl *)(FSnippetInit, unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    using FAdapterPopulate = NVSDK_NGX_Result (__cdecl *)(FSnippetPopulate, NVSDK_NGX_Parameter*);
    using FAdapterCreate = NVSDK_NGX_Result (__cdecl *)(FSnippetCreate, ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using FAdapterEvaluate = NVSDK_NGX_Result (__cdecl *)(FSnippetEvaluate, ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*);
    using FAdapterRelease = NVSDK_NGX_Result (__cdecl *)(FSnippetRelease, NVSDK_NGX_Handle*);

    enum class EFeatureState : uint8
    {
        Uninitialized,
        WaitingForCreateFence,
        Ready,
        FaultedQuarantined
    };

    struct FPendingEvaluation
    {
        uint64 FenceValue = 0;
        uint64 FrameNumber = 0;
        uint64 ViewId = 0;
        uint64 ConfigGeneration = 0;
        int32 Route = 1;
    };

    struct FBootstrapState
    {
        void* SnippetHandle = nullptr;
        void* AdapterHandle = nullptr;
        NVSDK_NGX_Parameter* CoreCapabilityParameters = nullptr;
        bool bAttempted = false;
        bool bBootstrapQueued = false;
        bool bSnippetCallEntered = false;
        bool bSucceeded = false;
        FSnippetCreate SnippetCreate = nullptr;
        FSnippetEvaluate SnippetEvaluate = nullptr;
        FSnippetRelease SnippetRelease = nullptr;
        FAdapterCreate AdapterCreate = nullptr;
        FAdapterEvaluate AdapterEvaluate = nullptr;
        FAdapterRelease AdapterRelease = nullptr;
        NVSDK_NGX_Handle* Feature = nullptr;
        TRefCountPtr<ID3D12Fence> CompletionFence;
        FIntPoint FeatureExtent = FIntPoint::ZeroValue;
        uint64 NextFenceValue = 1;
        uint64 CreateFenceValue = 0;
        uint64 LastUseFenceValue = 0;
        uint64 ConfigGeneration = 0;
        uint64 LastViewId = 0;
        // Track the route of the previous evaluation.
        // Pre DLSS / Post DLAA feed HDR-encoded input and Post Tone feeds display-encoded input; history from one domain is invalid in the other.
        int32 LastRoute = -1;
        TArray<FPendingEvaluation> PendingEvaluations;
        bool bResetPending = true;
        EFeatureState FeatureState = EFeatureState::Uninitialized;
    };

    FBootstrapState GBootstrapState;
    FCriticalSection GBootstrapMutex;

    template <typename T>
    bool ResolveExport(void* ModuleHandle, const TCHAR* Name, T& OutFunction)
    {
        OutFunction = reinterpret_cast<T>(FPlatformProcess::GetDllExport(ModuleHandle, Name));
        return OutFunction != nullptr;
    }

    bool HashFileSHA256(const FString& Path, FString& OutHash)
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> File(PlatformFile.OpenRead(*Path));
        if (!File)
        {
            return false;
        }

        BCRYPT_ALG_HANDLE Algorithm = nullptr;
        BCRYPT_HASH_HANDLE HashHandle = nullptr;
        ULONG ObjectBytes = 0;
        ULONG HashBytes = 0;
        ULONG ResultBytes = 0;

        auto Cleanup = [&]()
        {
            if (HashHandle != nullptr)
            {
                BCryptDestroyHash(HashHandle);
            }
            if (Algorithm != nullptr)
            {
                BCryptCloseAlgorithmProvider(Algorithm, 0);
            }
        };

        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
            || !BCRYPT_SUCCESS(BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&ObjectBytes), sizeof(ObjectBytes), &ResultBytes, 0))
            || !BCRYPT_SUCCESS(BCryptGetProperty(Algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&HashBytes), sizeof(HashBytes), &ResultBytes, 0)))
        {
            Cleanup();
            return false;
        }

        TArray<uint8> Object;
        TArray<uint8> Digest;
        TArray<uint8> Buffer;
        Object.SetNumUninitialized(ObjectBytes);
        Digest.SetNumUninitialized(HashBytes);
        Buffer.SetNumUninitialized(1024 * 1024);

        if (!BCRYPT_SUCCESS(BCryptCreateHash(
            Algorithm, &HashHandle, Object.GetData(), Object.Num(), nullptr, 0, 0)))
        {
            Cleanup();
            return false;
        }

        int64 Remaining = File->Size();
        while (Remaining > 0)
        {
            const int64 ReadBytes = FMath::Min<int64>(Remaining, Buffer.Num());
            if (!File->Read(Buffer.GetData(), ReadBytes)
                || !BCRYPT_SUCCESS(BCryptHashData(
                    HashHandle, Buffer.GetData(), static_cast<ULONG>(ReadBytes), 0)))
            {
                Cleanup();
                return false;
            }
            Remaining -= ReadBytes;
        }

        const bool bFinished = BCRYPT_SUCCESS(BCryptFinishHash(
            HashHandle, Digest.GetData(), Digest.Num(), 0));
        if (bFinished)
        {
            OutHash = BytesToHex(Digest.GetData(), Digest.Num()).ToLower();
        }
        Cleanup();
        return bFinished;
    }

    FString GetLoadedModulePath(HMODULE Module)
    {
        TCHAR PathBuffer[32768] = {};
        const DWORD PathLength = ::GetModuleFileNameW(Module, PathBuffer, UE_ARRAY_COUNT(PathBuffer));
        return PathLength > 0 && PathLength < UE_ARRAY_COUNT(PathBuffer)
            ? FString(static_cast<int32>(PathLength), PathBuffer)
            : FString(TEXT("<path unavailable>"));
    }

    void ReleasePreInitHandles()
    {
        if (GBootstrapState.AdapterHandle != nullptr)
        {
            FPlatformProcess::FreeDllHandle(GBootstrapState.AdapterHandle);
            GBootstrapState.AdapterHandle = nullptr;
        }
        if (GBootstrapState.SnippetHandle != nullptr)
        {
            FPlatformProcess::FreeDllHandle(GBootstrapState.SnippetHandle);
            GBootstrapState.SnippetHandle = nullptr;
        }
    }

    void SetFeatureFault(const TCHAR* Stage, NVSDK_NGX_Result Result)
    {
        GBootstrapState.FeatureState = EFeatureState::FaultedQuarantined;
        FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        Status.State = EDLSS5OneMinusRuntimeState::FaultedQuarantined;
        Status.bOutputSubmitted = false;
        Status.bOutputCompleted = false;
        Status.LastEvaluateResult = static_cast<int32>(Result);
        Status.Detail = FString::Printf(
            TEXT("NR entered the unofficial %s call and failed with 0x%08X. The feature, borrowed parameters, and unofficial modules are quarantined until editor restart."),
            Stage, static_cast<uint32>(Result));
        DLSS5OneMinus::SetStatus(Status);
        UE_LOG(LogDLSS5OneMinusNGXBootstrap, Error, TEXT("%s"), *Status.Detail);
    }

    ID3D12Resource* GetResidentResource(
        ID3D12DynamicRHI* D3D12RHI,
        FRHICommandList& RHICmdList,
        FRHITexture* Texture,
        bool bInput)
    {
        if (Texture == nullptr)
        {
            return nullptr;
        }

        // UE 5.6+ path - make the texture resident explicitly; RDG owns the state transitions (inputs SRV, output UAVCompute).
        // In 5.6 ID3D12DynamicRHI::RHITransitionResource is deprecated and implemented as a no-op that logs an error, so the 5.5 transition+residency side effect silently disappears.
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
        D3D12RHI->RHIUpdateResourceResidency(RHICmdList, D3D12RHI->RHIGetResourceDeviceIndex(Texture), Texture);
#else
        D3D12RHI->RHITransitionResource(
            RHICmdList,
            Texture,
            bInput ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
#endif
        return D3D12RHI->RHIGetResource(Texture);
    }

    void SetSubrect(NVSDK_NGX_Parameter* Parameters, const ANSICHAR* Prefix, const FIntRect& Rect)
    {
        const FString Base = FString::Printf(TEXT("DLSSNR.%sSubrect"), ANSI_TO_TCHAR(Prefix));
        Parameters->Set(TCHAR_TO_ANSI(*(Base + TEXT("BaseX"))), static_cast<uint32>(Rect.Min.X));
        Parameters->Set(TCHAR_TO_ANSI(*(Base + TEXT("BaseY"))), static_cast<uint32>(Rect.Min.Y));
        Parameters->Set(TCHAR_TO_ANSI(*(Base + TEXT("Width"))), static_cast<uint32>(Rect.Width()));
        Parameters->Set(TCHAR_TO_ANSI(*(Base + TEXT("Height"))), static_cast<uint32>(Rect.Height()));
    }

    float ReadFloatCVar(const TCHAR* Name, float DefaultValue)
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable != nullptr ? Variable->GetFloat() : DefaultValue;
    }

    int32 ReadIntCVar(const TCHAR* Name, int32 DefaultValue)
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable != nullptr ? Variable->GetInt() : DefaultValue;
    }

    BEGIN_SHADER_PARAMETER_STRUCT(FModelPassParameters, )
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputColor)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputDepth)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputMotionVectors)
        RDG_TEXTURE_ACCESS(OutputColor, ERHIAccess::UAVCompute)
    END_SHADER_PARAMETER_STRUCT()
#endif

    void SetBootstrapFault(FDLSS5OneMinusStatusSnapshot& Status, const FString& Detail)
    {
        Status.State = EDLSS5OneMinusRuntimeState::FaultedQuarantined;
        Status.bNGXOwnershipProven = false;
        Status.Detail = Detail;
        DLSS5OneMinus::SetStatus(Status);
        UE_LOG(LogDLSS5OneMinusNGXBootstrap, Error, TEXT("%s"), *Detail);
    }
}

void DLSS5OneMinus::RunNGXSnippetBootstrap()
{
    FDLSS5OneMinusStatusSnapshot Status = GetStatus();
    if (!Status.bVendorPluginAvailable || !Status.bUnofficialRuntimeAvailable)
    {
        UE_LOG(LogDLSS5OneMinusNGXBootstrap, Warning,
            TEXT("NGX snippet bootstrap refused because required dependencies are unavailable: %s"),
            *Status.Detail);
        return;
    }

#if DLSS5ONEMINUS_WITH_D3D12 && PLATFORM_WINDOWS
    if ((IsRunningRHIInSeparateThread() && !IsInRHIThread())
        || (!IsRunningRHIInSeparateThread() && !IsInRenderingThread()))
    {
        {
            FScopeLock BootstrapLock(&GBootstrapMutex);
            if (GBootstrapState.bAttempted || GBootstrapState.bBootstrapQueued)
            {
                return;
            }
            GBootstrapState.bBootstrapQueued = true;
        }

        Status.State = EDLSS5OneMinusRuntimeState::Probing;
        Status.Detail = TEXT("NR bootstrap queued in the ordered RHI execution stream.");
        SetStatus(Status);
        ENQUEUE_RENDER_COMMAND(DLSS5OneMinusBootstrap)(
            [](FRHICommandListImmediate& RHICmdList)
            {
                if (IsRunningRHIInSeparateThread())
                {
                    RHICmdList.EnqueueLambda(
                        [](FRHICommandListImmediate&)
                        {
                            DLSS5OneMinus::RunNGXSnippetBootstrap();
                        });
                }
                else
                {
                    DLSS5OneMinus::RunNGXSnippetBootstrap();
                }
            });
        return;
    }

    FScopeLock BootstrapLock(&GBootstrapMutex);
    if (GBootstrapState.bAttempted)
    {
        UE_LOG(LogDLSS5OneMinusNGXBootstrap, Display,
            TEXT("NGX snippet bootstrap was already attempted in this process (success=%s); no call was made."),
            GBootstrapState.bSucceeded ? TEXT("true") : TEXT("false"));
        return;
    }
    GBootstrapState.bBootstrapQueued = false;
    GBootstrapState.bAttempted = true;

    if (!IsRHID3D12())
    {
        SetBootstrapFault(Status, TEXT("NGX snippet bootstrap requires the active D3D12 RHI. NR is quarantined for this editor session."));
        return;
    }

    Status.State = EDLSS5OneMinusRuntimeState::Probing;
    Status.Detail = TEXT("Validating unofficial runtime and locating the stock NGX core; no feature is being created or evaluated.");
    SetStatus(Status);

    const FString RuntimeDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectPluginsDir(), TEXT("DLSS5/DLSSNRRuntime/Win64")));
    const FString SnippetPath = FPaths::Combine(RuntimeDir, TEXT("nvngx_dlssnr.dll"));
    const FString AdapterPath = FPaths::Combine(RuntimeDir, TEXT("nvngx.dll_research_adapter.dll"));

    FString SnippetHash;
    FString AdapterHash;
    if (!HashFileSHA256(SnippetPath, SnippetHash)
        || !SnippetHash.Equals(ExpectedSnippetSHA256, ESearchCase::IgnoreCase)
        || !HashFileSHA256(AdapterPath, AdapterHash)
        || !AdapterHash.Equals(ExpectedAdapterSHA256, ESearchCase::IgnoreCase))
    {
        SetBootstrapFault(Status, FString::Printf(
            TEXT("Unofficial runtime hash validation failed. Expected snippet %s and adapter %s; no DLL was loaded."),
            ExpectedSnippetSHA256, ExpectedAdapterSHA256));
        return;
    }

    HMODULE CoreModule = ::GetModuleHandleW(L"_nvngx.dll");
    if (CoreModule == nullptr)
    {
        SetBootstrapFault(Status,
            TEXT("The stock NGX core module _nvngx.dll is not loaded. Initialize stock DLSS/DLAA first, then restart the editor before retrying NR."));
        return;
    }
    const FString CorePath = GetLoadedModulePath(CoreModule);

    FCoreGetCapabilityParameters CoreGetCapabilityParameters = nullptr;
    if (!ResolveExport(reinterpret_cast<void*>(CoreModule), TEXT("NVSDK_NGX_D3D12_GetCapabilityParameters"), CoreGetCapabilityParameters))
    {
        SetBootstrapFault(Status, FString::Printf(
            TEXT("Loaded stock NGX core has no D3D12 capability-parameter export: %s"), *CorePath));
        return;
    }

    GBootstrapState.SnippetHandle = FPlatformProcess::GetDllHandle(*SnippetPath);
    GBootstrapState.AdapterHandle = FPlatformProcess::GetDllHandle(*AdapterPath);
    if (GBootstrapState.SnippetHandle == nullptr || GBootstrapState.AdapterHandle == nullptr)
    {
        ReleasePreInitHandles();
        SetBootstrapFault(Status, TEXT("Validated unofficial runtime DLLs could not be loaded; no snippet call was made."));
        return;
    }

    FSnippetInit SnippetInit = nullptr;
    FSnippetPopulate SnippetPopulate = nullptr;
    FSnippetGetApplicationId SnippetGetApplicationId = nullptr;
    FSnippetGetAPIVersion SnippetGetAPIVersion = nullptr;
    FAdapterInit AdapterInit = nullptr;
    FAdapterPopulate AdapterPopulate = nullptr;
    const bool bExportsResolved =
        ResolveExport(GBootstrapState.SnippetHandle, TEXT("NVSDK_NGX_D3D12_Init_Ext"), SnippetInit)
        && ResolveExport(GBootstrapState.SnippetHandle, TEXT("NVSDK_NGX_D3D12_PopulateParameters_Impl"), SnippetPopulate)
        && ResolveExport(GBootstrapState.SnippetHandle, TEXT("NVSDK_NGX_D3D12_CreateFeature"), GBootstrapState.SnippetCreate)
        && ResolveExport(GBootstrapState.SnippetHandle, TEXT("NVSDK_NGX_D3D12_EvaluateFeature"), GBootstrapState.SnippetEvaluate)
        && ResolveExport(GBootstrapState.SnippetHandle, TEXT("NVSDK_NGX_D3D12_ReleaseFeature"), GBootstrapState.SnippetRelease)
        && ResolveExport(GBootstrapState.SnippetHandle, TEXT("NVSDK_NGX_GetApplicationId"), SnippetGetApplicationId)
        && ResolveExport(GBootstrapState.SnippetHandle, TEXT("NVSDK_NGX_GetAPIVersion"), SnippetGetAPIVersion)
        && ResolveExport(GBootstrapState.AdapterHandle, TEXT("ProbeNrInit"), AdapterInit)
        && ResolveExport(GBootstrapState.AdapterHandle, TEXT("ProbeNrPopulate"), AdapterPopulate)
        && ResolveExport(GBootstrapState.AdapterHandle, TEXT("ProbeNrCreate"), GBootstrapState.AdapterCreate)
        && ResolveExport(GBootstrapState.AdapterHandle, TEXT("ProbeNrEvaluate"), GBootstrapState.AdapterEvaluate)
        && ResolveExport(GBootstrapState.AdapterHandle, TEXT("ProbeNrRelease"), GBootstrapState.AdapterRelease);
    if (!bExportsResolved)
    {
        ReleasePreInitHandles();
        SetBootstrapFault(Status, TEXT("Unofficial runtime export validation failed; no snippet call was made."));
        return;
    }

    const NVSDK_NGX_Result CapabilityResult = CoreGetCapabilityParameters(&GBootstrapState.CoreCapabilityParameters);
    if (NVSDK_NGX_FAILED(CapabilityResult) || GBootstrapState.CoreCapabilityParameters == nullptr)
    {
        ReleasePreInitHandles();
        SetBootstrapFault(Status, FString::Printf(
            TEXT("Loaded stock NGX core capability acquisition failed (result 0x%08X); no snippet call was made."),
            static_cast<uint32>(CapabilityResult)));
        return;
    }

    ID3D12DynamicRHI* D3D12RHI = GetID3D12DynamicRHI();
    ID3D12Device* Device = D3D12RHI != nullptr ? D3D12RHI->RHIGetDevice(0) : nullptr;
    if (Device == nullptr)
    {
        ReleasePreInitHandles();
        SetBootstrapFault(Status,
            TEXT("UE D3D12 device lookup failed after acquiring the borrowed core parameter block. The block is intentionally retained; NR is quarantined."));
        return;
    }

    const unsigned long long ApplicationId = SnippetGetApplicationId();
    const NVSDK_NGX_Version APIVersion = SnippetGetAPIVersion();
    const FString DataPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("DLSS5OneMinus/NGX")));
    IFileManager::Get().MakeDirectory(*DataPath, true);

    Status.State = EDLSS5OneMinusRuntimeState::Initializing;
    Status.Detail = FString::Printf(
        TEXT("Initializing the NR snippet through the isolated caller adapter (application 0x%llX, API 0x%X). No feature will be created."),
        ApplicationId, static_cast<uint32>(APIVersion));
    SetStatus(Status);

    GBootstrapState.bSnippetCallEntered = true;
    const NVSDK_NGX_Result InitResult = AdapterInit(
        SnippetInit, ApplicationId, *DataPath, Device, APIVersion, GBootstrapState.CoreCapabilityParameters);
    if (NVSDK_NGX_FAILED(InitResult))
    {
        SetBootstrapFault(Status, FString::Printf(
            TEXT("NR snippet Init_Ext failed (result 0x%08X, application 0x%llX, API 0x%X). Runtime modules and the borrowed core block are retained for process safety."),
            static_cast<uint32>(InitResult), ApplicationId, static_cast<uint32>(APIVersion)));
        return;
    }

    const NVSDK_NGX_Result PopulateResult = AdapterPopulate(
        SnippetPopulate, GBootstrapState.CoreCapabilityParameters);
    if (NVSDK_NGX_FAILED(PopulateResult))
    {
        SetBootstrapFault(Status, FString::Printf(
            TEXT("NR snippet parameter population failed (result 0x%08X). Runtime modules and the borrowed core block are retained for process safety."),
            static_cast<uint32>(PopulateResult)));
        return;
    }

    if (FAILED(Device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(GBootstrapState.CompletionFence.GetInitReference()))))
    {
        SetBootstrapFault(Status,
            TEXT("NR snippet initialized and populated, but creation of the unofficial completion fence failed. Runtime modules and the borrowed core block are retained for process safety."));
        return;
    }

    GBootstrapState.bSucceeded = true;
    Status.State = EDLSS5OneMinusRuntimeState::DependenciesReady;
    Status.bNGXOwnershipProven = true;
    Status.Detail = FString::Printf(
        TEXT("NR snippet bootstrap passed (application 0x%llX, API 0x%X). Borrowed core parameters remain owned by stock NGX; no feature was created or evaluated."),
        ApplicationId, static_cast<uint32>(APIVersion));
    SetStatus(Status);

    UE_LOG(LogDLSS5OneMinusNGXBootstrap, Display,
        TEXT("DLSS5-OneMinus no-feature bootstrap passed: core=%s; snippet=%s; adapter=%s; app=0x%llX; api=0x%X; capability=0x%08X; init=0x%08X; populate=0x%08X. No core Init/Shutdown, parameter destroy, feature create, or evaluation occurred."),
        *CorePath,
        *SnippetPath,
        *AdapterPath,
        ApplicationId,
        static_cast<uint32>(APIVersion),
        static_cast<uint32>(CapabilityResult),
        static_cast<uint32>(InitResult),
        static_cast<uint32>(PopulateResult));
#else
    SetBootstrapFault(Status, TEXT("NGX snippet bootstrap is only implemented for Win64 D3D12. NR remains disabled."));
#endif
}

#if DLSS5ONEMINUS_WITH_D3D12 && PLATFORM_WINDOWS
namespace
{
    struct FModelExecutionArguments
    {
        FRHITexture* Color = nullptr;
        FRHITexture* Depth = nullptr;
        FRHITexture* MotionVectors = nullptr;
        FRHITexture* Output = nullptr;
        FIntRect ColorRect;
        FIntRect DepthRect;
        FIntRect MotionVectorRect;
        uint64 ViewId = 0;
        int32 Route = 1;
        bool bReset = false;
        FDLSS5OneMinusRenderSettings Settings;
    };

    const TCHAR* RouteName(const int32 Route)
    {
        switch (Route)
        {
        case 0: return TEXT("Pre DLSS");
        case 2: return TEXT("Post DLAA");
        default: return TEXT("Post Tone");
        }
    }

    void PromoteCompletedEvaluations()
    {
        if (!GBootstrapState.CompletionFence.IsValid())
        {
            return;
        }

        const uint64 CompletedFenceValue = GBootstrapState.CompletionFence->GetCompletedValue();
        int32 CompletedCount = 0;
        FPendingEvaluation LatestCompleted;
        for (const FPendingEvaluation& Pending : GBootstrapState.PendingEvaluations)
        {
            if (Pending.FenceValue > CompletedFenceValue)
            {
                break;
            }
            LatestCompleted = Pending;
            ++CompletedCount;
        }

        FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        Status.LastCompletedFence = CompletedFenceValue;
        if (CompletedCount > 0)
        {
            Status.FrameNumber = LatestCompleted.FrameNumber;
            Status.ViewId = LatestCompleted.ViewId;
            Status.ConfigGeneration = LatestCompleted.ConfigGeneration;
            Status.EffectiveRoute = RouteName(LatestCompleted.Route);
            Status.bOutputCompleted = true;
            Status.Detail = FString::Printf(
                TEXT("NR GPU output completed for %s (frame %llu, generation %llu, fence %llu)."),
                RouteName(LatestCompleted.Route),
                LatestCompleted.FrameNumber,
                LatestCompleted.ConfigGeneration,
                LatestCompleted.FenceValue);
            GBootstrapState.PendingEvaluations.RemoveAt(0, CompletedCount, EAllowShrinking::No);
        }
        DLSS5OneMinus::SetStatus(Status);
    }

    bool ExecuteModel(FRHICommandList& RHICmdList, const FModelExecutionArguments& Arguments)
    {
        check(!IsRunningRHIInSeparateThread() || IsInRHIThread());
        FScopeLock BootstrapLock(&GBootstrapMutex);
        PromoteCompletedEvaluations();

        if (!GBootstrapState.bSucceeded
            || GBootstrapState.FeatureState == EFeatureState::FaultedQuarantined)
        {
            return false;
        }
        if (GNumExplicitGPUsForRendering != 1)
        {
            SetFeatureFault(TEXT("multi_gpu_rejected"), NVSDK_NGX_Result_FAIL_UnsupportedInputFormat);
            return false;
        }
        if (GRHIIsDebugLayerEnabled)
        {
            SetFeatureFault(TEXT("ue55_debug_layer_rejected"), NVSDK_NGX_Result_FAIL_UnsupportedInputFormat);
            return false;
        }

        const FIntPoint RequestedExtent = Arguments.ColorRect.Size();
        if (GBootstrapState.Feature != nullptr && GBootstrapState.FeatureExtent != RequestedExtent)
        {
            if (GBootstrapState.CompletionFence->GetCompletedValue() < GBootstrapState.LastUseFenceValue)
            {
                return false;
            }

            const NVSDK_NGX_Result ReleaseResult = GBootstrapState.AdapterRelease(
                GBootstrapState.SnippetRelease, GBootstrapState.Feature);
            if (NVSDK_NGX_FAILED(ReleaseResult))
            {
                SetFeatureFault(TEXT("release_for_resize"), ReleaseResult);
                return false;
            }
            GBootstrapState.Feature = nullptr;
            GBootstrapState.FeatureExtent = FIntPoint::ZeroValue;
            GBootstrapState.FeatureState = EFeatureState::Uninitialized;
            GBootstrapState.bResetPending = true;
            GBootstrapState.PendingEvaluations.Reset();
            ++GBootstrapState.ConfigGeneration;
            DLSS5OneMinus::InvalidateOutputStatus(TEXT("NR resources resized; waiting for fresh GPU output."));
        }

        if (GBootstrapState.FeatureState == EFeatureState::WaitingForCreateFence)
        {
            const uint64 CompletedFenceValue = GBootstrapState.CompletionFence->GetCompletedValue();
            FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
            Status.LastCompletedFence = CompletedFenceValue;
            Status.Detail = FString::Printf(
                TEXT("NR feature 18 created for %dx%d; waiting for create fence %llu (completed %llu). Stock scene color remains active."),
                RequestedExtent.X,
                RequestedExtent.Y,
                GBootstrapState.CreateFenceValue,
                CompletedFenceValue);
            DLSS5OneMinus::SetStatus(Status);
            if (CompletedFenceValue < GBootstrapState.CreateFenceValue)
            {
                return false;
            }
            GBootstrapState.FeatureState = EFeatureState::Ready;
            GBootstrapState.bResetPending = true;
        }

        ID3D12DynamicRHI* D3D12RHI = GetID3D12DynamicRHI();
        const uint32 DeviceIndex = D3D12RHI->RHIGetResourceDeviceIndex(Arguments.Color);
        ID3D12GraphicsCommandList* CommandList = D3D12RHI->RHIGetGraphicsCommandList(
            RHICmdList, DeviceIndex);
        if (CommandList == nullptr)
        {
            SetFeatureFault(TEXT("command_list"), NVSDK_NGX_Result_FAIL_InvalidParameter);
            return false;
        }

        NVSDK_NGX_Parameter* Parameters = GBootstrapState.CoreCapabilityParameters;
        const uint32 Width = static_cast<uint32>(RequestedExtent.X);
        const uint32 Height = static_cast<uint32>(RequestedExtent.Y);
        NVSDK_NGX_Parameter_SetUI(Parameters, "DLSSNR.Enabled", 1);
        NVSDK_NGX_Parameter_SetUI(Parameters, "DLSSNR.Width", Width);
        NVSDK_NGX_Parameter_SetUI(Parameters, "DLSSNR.Height", Height);
        NVSDK_NGX_Parameter_SetUI(Parameters, "DLSSNR.Hint.Render.Preset", 0);
        NVSDK_NGX_Parameter_SetUI(Parameters, "DLSSNR.UICorrection", 0);
        NVSDK_NGX_Parameter_SetUI(Parameters, "DLSSNR.Style",
            static_cast<uint32>(FMath::Clamp(Arguments.Settings.Style, 0, 2)));
        NVSDK_NGX_Parameter_SetUI(Parameters, "DLSSNR.UseAutoMask",
            Arguments.Settings.bAutoMask ? 1u : 0u);
        NVSDK_NGX_Parameter_SetF(Parameters, "DLSSNR.Intensity",
            FMath::Clamp(Arguments.Settings.Intensity, 0.0f, 2.0f));
        NVSDK_NGX_Parameter_SetF(Parameters, "DLSSNR.LocalToneStrength",
            FMath::Clamp(Arguments.Settings.LocalToneStrength, 0.0f, 2.0f));
        NVSDK_NGX_Parameter_SetF(Parameters, "DLSSNR.LocalStructureStrength",
            FMath::Clamp(Arguments.Settings.LocalStructureStrength, 0.0f, 2.0f));
        NVSDK_NGX_Parameter_SetF(Parameters, "DLSSNR.SkinStructureStrength",
            FMath::Clamp(Arguments.Settings.SkinStructureStrength, -1.0f, 2.0f));

        if (GBootstrapState.Feature == nullptr)
        {
            NVSDK_NGX_Parameter_SetUI(Parameters, "CreationNodeMask", 1u << DeviceIndex);
            NVSDK_NGX_Parameter_SetUI(Parameters, "VisibilityNodeMask", RHICmdList.GetGPUMask().GetNative());

            const NVSDK_NGX_Result CreateResult = GBootstrapState.AdapterCreate(
                GBootstrapState.SnippetCreate,
                CommandList,
                static_cast<NVSDK_NGX_Feature>(18),
                Parameters,
                &GBootstrapState.Feature);
            if (NVSDK_NGX_FAILED(CreateResult) || GBootstrapState.Feature == nullptr)
            {
                SetFeatureFault(TEXT("create_feature"), CreateResult);
                return false;
            }

            GBootstrapState.FeatureExtent = RequestedExtent;
            GBootstrapState.LastViewId = Arguments.ViewId;
            ++GBootstrapState.ConfigGeneration;
            D3D12RHI->RHIFinishExternalComputeWork(
                RHICmdList, DeviceIndex, CommandList);
            GBootstrapState.CreateFenceValue = GBootstrapState.NextFenceValue++;
            GBootstrapState.LastUseFenceValue = GBootstrapState.CreateFenceValue;
            D3D12RHI->RHISignalManualFence(
                RHICmdList, GBootstrapState.CompletionFence, GBootstrapState.CreateFenceValue);
            // At the bottom of the pipe (notably MRQ), this callback is
            // already recording directly into the submission stream. A
            // subsequent frame on the same D3D12 direct queue is ordered
            // after creation without requiring a CPU-side fence poll. The
            // manual fence is retained for release/teardown accounting.
            const bool bQueueOrderedReady = RHICmdList.IsBottomOfPipe();
            GBootstrapState.FeatureState = bQueueOrderedReady
                ? EFeatureState::Ready
                : EFeatureState::WaitingForCreateFence;
            GBootstrapState.bResetPending = bQueueOrderedReady;

            FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
            Status.State = bQueueOrderedReady
                ? EDLSS5OneMinusRuntimeState::Ready
                : EDLSS5OneMinusRuntimeState::WaitingForCreateFence;
            Status.ConfigGeneration = GBootstrapState.ConfigGeneration;
            Status.ViewId = Arguments.ViewId;
            Status.LastEvaluateResult = static_cast<int32>(CreateResult);
            Status.LastCompletedFence = GBootstrapState.CompletionFence->GetCompletedValue();
            Status.bOutputSubmitted = false;
            Status.bOutputCompleted = false;
            Status.Detail = bQueueOrderedReady
                ? FString::Printf(
                    TEXT("NR feature 18 created for %ux%u; queue-ordered evaluation begins on the next frame (fence %llu)."),
                    Width, Height, GBootstrapState.CreateFenceValue)
                : FString::Printf(
                    TEXT("NR feature 18 created for %ux%u; stock scene color remains active until create fence %llu completes."),
                    Width, Height, GBootstrapState.CreateFenceValue);
            DLSS5OneMinus::SetStatus(Status);
            UE_LOG(LogDLSS5OneMinusNGXBootstrap, Display, TEXT("%s"), *Status.Detail);
            return false;
        }

        if (GBootstrapState.FeatureState != EFeatureState::Ready)
        {
            return false;
        }

        ID3D12Resource* InputColor = GetResidentResource(D3D12RHI, RHICmdList, Arguments.Color, true);
        ID3D12Resource* InputDepth = GetResidentResource(D3D12RHI, RHICmdList, Arguments.Depth, true);
        ID3D12Resource* InputMotion = GetResidentResource(D3D12RHI, RHICmdList, Arguments.MotionVectors, true);
        ID3D12Resource* OutputColor = GetResidentResource(D3D12RHI, RHICmdList, Arguments.Output, false);
        if (InputColor == nullptr || InputDepth == nullptr || InputMotion == nullptr || OutputColor == nullptr)
        {
            SetFeatureFault(TEXT("native_resource_binding"), NVSDK_NGX_Result_FAIL_InvalidParameter);
            return false;
        }
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
        // emit any pending RHI barriers into the native list before NGX records into it.
        for (const uint32 GPUIndex : RHICmdList.GetGPUMask())
        {
            D3D12RHI->RHIFlushResourceBarriers(RHICmdList, GPUIndex);
        }
#endif

        NVSDK_NGX_Parameter_SetD3d12Resource(Parameters, "DLSSNR.Color", InputColor);
        NVSDK_NGX_Parameter_SetD3d12Resource(Parameters, "DLSSNR.Output", OutputColor);
        NVSDK_NGX_Parameter_SetD3d12Resource(Parameters, "DLSSNR.Depth", InputDepth);
        NVSDK_NGX_Parameter_SetD3d12Resource(Parameters, "DLSSNR.MVec", InputMotion);
        NVSDK_NGX_Parameter_SetUI(Parameters, "DLSSNR.DepthInverted", 1);
        NVSDK_NGX_Parameter_SetUI(Parameters, "DLSSNR.Reset",
            Arguments.bReset || GBootstrapState.bResetPending || GBootstrapState.LastViewId != Arguments.ViewId
                || GBootstrapState.LastRoute != Arguments.Route ? 1u : 0u); // route change resets history
        SetSubrect(Parameters, "Color", Arguments.ColorRect);
        SetSubrect(Parameters, "Output", Arguments.ColorRect);
        SetSubrect(Parameters, "Depth", Arguments.DepthRect);
        SetSubrect(Parameters, "MVec", Arguments.MotionVectorRect);
        NVSDK_NGX_Parameter_SetF(Parameters, "DLSSNR.MVecScaleX", 1.0f);
        NVSDK_NGX_Parameter_SetF(Parameters, "DLSSNR.MVecScaleY", 1.0f);

        const NVSDK_NGX_Result EvaluateResult = GBootstrapState.AdapterEvaluate(
            GBootstrapState.SnippetEvaluate,
            CommandList,
            GBootstrapState.Feature,
            Parameters);
        if (NVSDK_NGX_FAILED(EvaluateResult))
        {
            SetFeatureFault(TEXT("evaluate_feature"), EvaluateResult);
            return false;
        }

        D3D12RHI->RHIFinishExternalComputeWork(
            RHICmdList, DeviceIndex, CommandList);
        GBootstrapState.LastUseFenceValue = GBootstrapState.NextFenceValue++;
        D3D12RHI->RHISignalManualFence(
            RHICmdList, GBootstrapState.CompletionFence, GBootstrapState.LastUseFenceValue);
        GBootstrapState.bResetPending = false;
        GBootstrapState.LastViewId = Arguments.ViewId;
        GBootstrapState.LastRoute = Arguments.Route;

        FPendingEvaluation& Pending = GBootstrapState.PendingEvaluations.AddDefaulted_GetRef();
        Pending.FenceValue = GBootstrapState.LastUseFenceValue;
        Pending.FrameNumber = GFrameCounter;
        Pending.ViewId = Arguments.ViewId;
        Pending.ConfigGeneration = GBootstrapState.ConfigGeneration;
        Pending.Route = Arguments.Route;

        FDLSS5OneMinusStatusSnapshot Status = DLSS5OneMinus::GetStatus();
        Status.State = EDLSS5OneMinusRuntimeState::Ready;
        Status.SubmittedFrameNumber = Pending.FrameNumber;
        Status.SubmittedViewId = Pending.ViewId;
        Status.SubmittedConfigGeneration = Pending.ConfigGeneration;
        Status.LastSubmittedFence = Pending.FenceValue;
        Status.LastEvaluateResult = static_cast<int32>(EvaluateResult);
        Status.LastCompletedFence = GBootstrapState.CompletionFence->GetCompletedValue();
        const bool bFirstSubmittedOutput = !Status.bOutputSubmitted;
        Status.bOutputSubmitted = true;
        Status.Detail = FString::Printf(
            TEXT("NR submitted feature 18 for %s at %ux%u (result 0x%08X, fence %llu; completed %llu)."),
            RouteName(Arguments.Route),
            Width,
            Height,
            static_cast<uint32>(EvaluateResult),
            GBootstrapState.LastUseFenceValue,
            Status.LastCompletedFence);
        DLSS5OneMinus::SetStatus(Status);
        if (bFirstSubmittedOutput)
        {
            UE_LOG(LogDLSS5OneMinusNGXBootstrap, Display, TEXT("%s"), *Status.Detail);
        }
        return true;
    }

    bool NeedsLifecycleSubmissionHint(const FIntPoint RequestedExtent)
    {
        FScopeLock BootstrapLock(&GBootstrapMutex);
        return GBootstrapState.bSucceeded
            && GBootstrapState.FeatureState != EFeatureState::FaultedQuarantined
            && (GBootstrapState.Feature == nullptr
                || GBootstrapState.FeatureExtent != RequestedExtent);
    }
}
#endif

bool DLSS5OneMinus::IsNGXSnippetBootstrapReady()
{
#if DLSS5ONEMINUS_WITH_D3D12 && PLATFORM_WINDOWS
    FScopeLock BootstrapLock(&GBootstrapMutex);
    return GBootstrapState.bSucceeded
        && GBootstrapState.FeatureState != EFeatureState::FaultedQuarantined;
#else
    return false;
#endif
}

FRDGTextureRef DLSS5OneMinus::AddModelPass(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FDLSS5OneMinusModelPassInputs& Inputs)
{
    check(Inputs.Color != nullptr && Inputs.Depth != nullptr && Inputs.MotionVectors != nullptr);
    check(!Inputs.ColorRect.IsEmpty() && !Inputs.DepthRect.IsEmpty() && !Inputs.MotionVectorRect.IsEmpty());

    FRDGTextureDesc OutputDesc = Inputs.Color->Desc;
    OutputDesc.Flags |= TexCreate_ShaderResource | TexCreate_UAV;
    FRDGTextureRef Output = GraphBuilder.CreateTexture(OutputDesc, TEXT("DLSS5OneMinus.ModelOutput"));
    AddCopyTexturePass(GraphBuilder, Inputs.Color, Output);

#if DLSS5ONEMINUS_WITH_D3D12 && PLATFORM_WINDOWS
    if (!IsNGXSnippetBootstrapReady())
    {
        return Output;
    }

    const bool bNeedsLifecycleSubmissionHint = NeedsLifecycleSubmissionHint(Inputs.ColorRect.Size());
    FModelPassParameters* Parameters = GraphBuilder.AllocParameters<FModelPassParameters>();
    Parameters->InputColor = Inputs.Color;
    Parameters->InputDepth = Inputs.Depth;
    Parameters->InputMotionVectors = Inputs.MotionVectors;
    Parameters->OutputColor = Output;

    GraphBuilder.AddPass(
        RDG_EVENT_NAME("DLSS5-OneMinus NR %dx%d", Inputs.ColorRect.Width(), Inputs.ColorRect.Height()),
        Parameters,
        ERDGPassFlags::Compute | ERDGPassFlags::Raster | ERDGPassFlags::Copy | ERDGPassFlags::SkipRenderPass,
        [Parameters, Inputs, bNeedsLifecycleSubmissionHint](FRHICommandListImmediate& RHICmdList)
        {
            Parameters->InputColor->MarkResourceAsUsed();
            Parameters->InputDepth->MarkResourceAsUsed();
            Parameters->InputMotionVectors->MarkResourceAsUsed();
            Parameters->OutputColor->MarkResourceAsUsed();

            FModelExecutionArguments Arguments;
            Arguments.Color = Parameters->InputColor->GetRHI();
            Arguments.Depth = Parameters->InputDepth->GetRHI();
            Arguments.MotionVectors = Parameters->InputMotionVectors->GetRHI();
            Arguments.Output = Parameters->OutputColor->GetRHI();
            Arguments.ColorRect = Inputs.ColorRect;
            Arguments.DepthRect = Inputs.DepthRect;
            Arguments.MotionVectorRect = Inputs.MotionVectorRect;
            Arguments.ViewId = Inputs.ViewId;
            Arguments.Route = Inputs.Route;
            Arguments.bReset = Inputs.bReset;
            Arguments.Settings = Inputs.Settings;

            // MRQ can execute this RDG pass at the bottom of the RHI pipe.
            // Enqueuing another lambda from there defers the work until after
            // the short render has finished, so the create fence never gets a
            // subsequent frame in which it can be observed. Execute directly
            // when already on the RHI thread; otherwise retain the normal
            // render-thread-to-RHI-thread handoff used by editor viewports.
            if (RHICmdList.IsBottomOfPipe())
            {
                ExecuteModel(RHICmdList, Arguments);
            }
            else
            {
                RHICmdList.EnqueueLambda(
                    [Arguments](FRHICommandListImmediate& CommandList) mutable
                    {
                        ExecuteModel(CommandList, Arguments);
                    });
            }

            // Feature creation/recreation closes the native D3D12 command
            // list and appends a manual-fence signal to UE's submission
            // payload. Dispatch that lifecycle payload promptly; otherwise a
            // quiet editor viewport can leave the create fence pending while
            // later frames only poll it.
            if (bNeedsLifecycleSubmissionHint && !RHICmdList.IsBottomOfPipe())
            {
                RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
            }
        });
#endif

    return Output;
}
