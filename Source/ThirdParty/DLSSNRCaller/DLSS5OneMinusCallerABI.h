#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>

using FOneMinusNrInit = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t*,
    ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using FOneMinusNrPopulate = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter*);
using FOneMinusNrCreate = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList*,
    NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using FOneMinusNrEvaluate = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList*,
    const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using FOneMinusNrRelease = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle*);

static_assert(sizeof(void*) == 8, "DLSS5-OneMinus's caller adapter is Windows x64 only.");
static_assert(sizeof(NVSDK_NGX_Result) == 4 && sizeof(NVSDK_NGX_Version) == 4,
    "Unexpected NGX ABI enum width.");
