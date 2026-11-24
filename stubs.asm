; Pass-through stubs for the real System32 dxgi.dll exports.
;
; g_real[i] is resolved lazily. The Windows app-compat engine calls dxgi's compat
; exports from inside the loader notification, where loading the real dxgi can fail;
; in that case StubSlowPath records the call (replayed once the real dxgi is loaded)
; and the stub returns 0 instead of jumping to a null pointer.
EXTERN g_real:QWORD
EXTERN StubSlowPath:PROC

STUB MACRO name, idx
name PROC
  mov rax, qword ptr [g_real + idx*8]
  test rax, rax
  jz @F
  jmp rax
@@:
  push rcx
  push rdx
  push r8
  push r9
  sub rsp, 28h
  mov ecx, idx
  lea rdx, [rsp + 28h]          ; -> saved r9, r8, rdx, rcx
  call StubSlowPath
  add rsp, 28h
  pop r9
  pop r8
  pop rdx
  pop rcx
  test eax, eax
  jz name&_deferred
  jmp qword ptr [g_real + idx*8]
name&_deferred:
  xor eax, eax
  ret
name ENDP
ENDM

.code
STUB stub_ApplyCompatResolutionQuirking, 0
STUB stub_CompatString, 1
STUB stub_CompatValue, 2
STUB stub_DXGIDumpJournal, 3
STUB stub_PIXBeginCapture, 4
STUB stub_PIXEndCapture, 5
STUB stub_PIXGetCaptureState, 6
STUB stub_SetAppCompatStringPointer, 7
STUB stub_UpdateHMDEmulationStatus, 8
STUB stub_DXGID3D10CreateDevice, 12
STUB stub_DXGID3D10CreateLayeredDevice, 13
STUB stub_DXGID3D10GetLayeredDeviceSize, 14
STUB stub_DXGID3D10RegisterLayers, 15
STUB stub_DXGIDeclareAdapterRemovalSupport, 16
STUB stub_DXGIDisableVBlankVirtualization, 17
STUB stub_DXGIGetDebugInterface1, 18
STUB stub_DXGIReportAdapterConfiguration, 19
END
