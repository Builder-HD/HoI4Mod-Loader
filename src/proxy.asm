; Export stubs for the version.dll proxy. Each stub tail-jumps through a pointer
; that main.cpp fills from System32\version.dll, so arguments pass through untouched
; regardless of signature (this also covers the undocumented GetFileVersionInfoByHandle).

FORWARD MACRO name
    EXTERN g_real_&name:QWORD
    Proxy_&name PROC
        jmp qword ptr [g_real_&name]
    Proxy_&name ENDP
ENDM

.code

FORWARD GetFileVersionInfoA
FORWARD GetFileVersionInfoByHandle
FORWARD GetFileVersionInfoExA
FORWARD GetFileVersionInfoExW
FORWARD GetFileVersionInfoSizeA
FORWARD GetFileVersionInfoSizeExA
FORWARD GetFileVersionInfoSizeExW
FORWARD GetFileVersionInfoSizeW
FORWARD GetFileVersionInfoW
FORWARD VerFindFileA
FORWARD VerFindFileW
FORWARD VerInstallFileA
FORWARD VerInstallFileW
FORWARD VerLanguageNameA
FORWARD VerLanguageNameW
FORWARD VerQueryValueA
FORWARD VerQueryValueW

END
