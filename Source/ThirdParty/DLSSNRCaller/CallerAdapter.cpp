#include "DLSS5OneMinusCallerABI.h"

// The unofficial snippet validates the physical caller module. These wrappers make
// that boundary explicit and keep every undocumented call out of the UE module.
extern "C" __declspec(dllexport) __declspec(noinline)
NVSDK_NGX_Result __cdecl ProbeNrInit(FOneMinusNrInit Function, unsigned long long ApplicationId,
    const wchar_t* DataPath, ID3D12Device* Device, NVSDK_NGX_Version Version,
    const NVSDK_NGX_Parameter* Parameters)
{
    volatile NVSDK_NGX_Result Result = Function(
        ApplicationId, DataPath, Device, Version, Parameters);
    return Result;
}
extern "C" __declspec(dllexport) __declspec(noinline)
NVSDK_NGX_Result __cdecl ProbeNrPopulate(FOneMinusNrPopulate Function,
    NVSDK_NGX_Parameter* Parameters)
{
    volatile NVSDK_NGX_Result Result = Function(Parameters);
    return Result;
}

extern "C" __declspec(dllexport) __declspec(noinline)
NVSDK_NGX_Result __cdecl ProbeNrCreate(FOneMinusNrCreate Function,
    ID3D12GraphicsCommandList* CommandList, NVSDK_NGX_Feature Feature,
    const NVSDK_NGX_Parameter* Parameters, NVSDK_NGX_Handle** Handle)
{
    volatile NVSDK_NGX_Result Result = Function(CommandList, Feature, Parameters, Handle);
    return Result;
}

extern "C" __declspec(dllexport) __declspec(noinline)
NVSDK_NGX_Result __cdecl ProbeNrEvaluate(FOneMinusNrEvaluate Function,
    ID3D12GraphicsCommandList* CommandList, const NVSDK_NGX_Handle* Handle,
    const NVSDK_NGX_Parameter* Parameters)
{
    volatile NVSDK_NGX_Result Result = Function(CommandList, Handle, Parameters, nullptr);
    return Result;
}

extern "C" __declspec(dllexport) __declspec(noinline)
NVSDK_NGX_Result __cdecl ProbeNrRelease(FOneMinusNrRelease Function, NVSDK_NGX_Handle* Handle)
{
    volatile NVSDK_NGX_Result Result = Function(Handle);
    return Result;
}
