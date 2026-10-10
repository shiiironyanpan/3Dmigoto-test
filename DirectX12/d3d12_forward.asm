; d3d12_forward.asm - jump stubs forwarding every export we do not hook to the real system d3d12.dll
; (generated list; must match g_ForwardTable order in Proxy12.cpp)

OPTION CASEMAP:NONE

EXTERN g_ForwardTable:QWORD

.code

Fwd_D3D12GetDebugInterface PROC
    jmp qword ptr [g_ForwardTable + 0]
Fwd_D3D12GetDebugInterface ENDP

Fwd_D3D12SerializeRootSignature PROC
    jmp qword ptr [g_ForwardTable + 8]
Fwd_D3D12SerializeRootSignature ENDP

Fwd_D3D12SerializeVersionedRootSignature PROC
    jmp qword ptr [g_ForwardTable + 16]
Fwd_D3D12SerializeVersionedRootSignature ENDP

Fwd_D3D12CreateRootSignatureDeserializer PROC
    jmp qword ptr [g_ForwardTable + 24]
Fwd_D3D12CreateRootSignatureDeserializer ENDP

Fwd_D3D12CreateVersionedRootSignatureDeserializer PROC
    jmp qword ptr [g_ForwardTable + 32]
Fwd_D3D12CreateVersionedRootSignatureDeserializer ENDP

Fwd_D3D12EnableExperimentalFeatures PROC
    jmp qword ptr [g_ForwardTable + 40]
Fwd_D3D12EnableExperimentalFeatures ENDP

Fwd_D3D12GetInterface PROC
    jmp qword ptr [g_ForwardTable + 48]
Fwd_D3D12GetInterface ENDP

Fwd_D3D12CoreCreateLayeredDevice PROC
    jmp qword ptr [g_ForwardTable + 56]
Fwd_D3D12CoreCreateLayeredDevice ENDP

Fwd_D3D12CoreGetLayeredDeviceSize PROC
    jmp qword ptr [g_ForwardTable + 64]
Fwd_D3D12CoreGetLayeredDeviceSize ENDP

Fwd_D3D12CoreRegisterLayeredDevice PROC
    jmp qword ptr [g_ForwardTable + 72]
Fwd_D3D12CoreRegisterLayeredDevice ENDP

Fwd_D3D12PIXEventsReplaceBlock PROC
    jmp qword ptr [g_ForwardTable + 80]
Fwd_D3D12PIXEventsReplaceBlock ENDP

Fwd_D3D12PIXGetThreadInfo PROC
    jmp qword ptr [g_ForwardTable + 88]
Fwd_D3D12PIXGetThreadInfo ENDP

Fwd_D3D12PIXNotifyWakeFromFenceSignal PROC
    jmp qword ptr [g_ForwardTable + 96]
Fwd_D3D12PIXNotifyWakeFromFenceSignal ENDP

Fwd_D3D12PIXReportCounter PROC
    jmp qword ptr [g_ForwardTable + 104]
Fwd_D3D12PIXReportCounter ENDP

Fwd_GetBehaviorValue PROC
    jmp qword ptr [g_ForwardTable + 112]
Fwd_GetBehaviorValue ENDP

Fwd_SetAppCompatStringPointer PROC
    jmp qword ptr [g_ForwardTable + 120]
Fwd_SetAppCompatStringPointer ENDP

END
