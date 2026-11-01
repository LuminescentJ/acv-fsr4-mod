EXTERN ResolveD3D12Export:PROC

.code

FORWARDER MACRO procName:req, exportIndex:req
    PUBLIC procName
procName PROC FRAME
    sub rsp, 0D8h
    .allocstack 0D8h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    mov [rsp+40h], rax
    mov [rsp+48h], r10
    mov [rsp+50h], r11
    movdqu [rsp+60h], xmm0
    movdqu [rsp+70h], xmm1
    movdqu [rsp+80h], xmm2
    movdqu [rsp+90h], xmm3
    movdqu [rsp+0A0h], xmm4
    movdqu [rsp+0B0h], xmm5
    mov ecx, exportIndex
    call ResolveD3D12Export
    test rax, rax
    jz missingExport
    mov [rsp+0C0h], rax
    movdqu xmm0, [rsp+60h]
    movdqu xmm1, [rsp+70h]
    movdqu xmm2, [rsp+80h]
    movdqu xmm3, [rsp+90h]
    movdqu xmm4, [rsp+0A0h]
    movdqu xmm5, [rsp+0B0h]
    mov rcx, [rsp+20h]
    mov rdx, [rsp+28h]
    mov r8, [rsp+30h]
    mov r9, [rsp+38h]
    mov r10, [rsp+48h]
    mov r11, [rsp+0C0h]
    mov rax, [rsp+40h]
    add rsp, 0D8h
    jmp r11
missingExport:
    add rsp, 0D8h
    mov eax, 80004001h
    ret
procName ENDP
ENDM

FORWARDER Forward_Ordinal99, 0
FORWARDER Forward_SetAppCompatStringPointer, 1
FORWARDER Forward_D3D12GetDebugInterface, 3
FORWARDER Forward_D3D12CoreCreateLayeredDevice, 4
FORWARDER Forward_D3D12CoreGetLayeredDeviceSize, 5
FORWARDER Forward_D3D12CoreRegisterLayers, 6
FORWARDER Forward_D3D12CreateRootSignatureDeserializer, 7
FORWARDER Forward_D3D12CreateVersionedRootSignatureDeserializer, 8
FORWARDER Forward_D3D12DeviceRemovedExtendedData, 9
FORWARDER Forward_D3D12EnableExperimentalFeatures, 10
FORWARDER Forward_D3D12GetInterface, 11
FORWARDER Forward_D3D12PIXEventsReplaceBlock, 12
FORWARDER Forward_D3D12PIXGetThreadInfo, 13
FORWARDER Forward_D3D12PIXNotifyWakeFromFenceSignal, 14
FORWARDER Forward_D3D12PIXReportCounter, 15
FORWARDER Forward_D3D12SerializeRootSignature, 16
FORWARDER Forward_D3D12SerializeVersionedRootSignature, 17
FORWARDER Forward_GetBehaviorValue, 18
END
