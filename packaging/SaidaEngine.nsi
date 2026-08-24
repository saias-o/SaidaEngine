Unicode true
ManifestSupportedOS all
RequestExecutionLevel user

!include "MUI2.nsh"

!ifndef OUTPUT_FILE
    !error "OUTPUT_FILE is required"
!endif
!ifndef PAYLOAD_INCLUDE
    !error "PAYLOAD_INCLUDE is required"
!endif
!ifndef UNINSTALL_INCLUDE
    !error "UNINSTALL_INCLUDE is required"
!endif
!ifndef PRODUCT_VERSION
    !define PRODUCT_VERSION "1.0.0.0"
!endif
!ifndef PRODUCT_DISPLAY_VERSION
    !define PRODUCT_DISPLAY_VERSION "1.0.0-beta.4"
!endif

Name "SaidaEngine ${PRODUCT_DISPLAY_VERSION}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\SaidaEngine"
InstallDirRegKey HKCU "Software\SaidaEngine" "InstallDir"
BrandingText "SaidaEngine"

SetCompressor /SOLID lzma
SetDatablockOptimize on
SetDateSave off
CRCCheck force
AutoCloseWindow true
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "CompanyName" "Saida"
VIAddVersionKey /LANG=1033 "FileDescription" "SaidaEngine Installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright SaidaEngine Contributors"
VIAddVersionKey /LANG=1033 "ProductName" "SaidaEngine"
VIAddVersionKey /LANG=1033 "ProductVersion" "${PRODUCT_DISPLAY_VERSION}"

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "SaidaEngine" SecMain
    !include "${PAYLOAD_INCLUDE}"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "Software\SaidaEngine" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaidaEngine" \
        "DisplayName" "SaidaEngine ${PRODUCT_DISPLAY_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaidaEngine" \
        "DisplayVersion" "${PRODUCT_DISPLAY_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaidaEngine" \
        "Publisher" "Saida"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaidaEngine" \
        "UninstallString" '"$INSTDIR\Uninstall.exe"'
    CreateDirectory "$SMPROGRAMS\SaidaEngine"
    CreateShortcut "$SMPROGRAMS\SaidaEngine\SaidaEngine Hub.lnk" \
        "$INSTDIR\SaidaEngineHub.exe" "" "$INSTDIR\SaidaEngineHub.exe" 0
    CreateShortcut "$SMPROGRAMS\SaidaEngine\SaidaEngine Editor.lnk" \
        "$INSTDIR\SaidaEngine.exe" "" "$INSTDIR\SaidaEngine.exe" 0
SectionEnd

Section "Uninstall"
    Delete "$SMPROGRAMS\SaidaEngine\SaidaEngine Hub.lnk"
    Delete "$SMPROGRAMS\SaidaEngine\SaidaEngine Editor.lnk"
    RMDir "$SMPROGRAMS\SaidaEngine"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaidaEngine"
    DeleteRegKey HKCU "Software\SaidaEngine"
    !include "${UNINSTALL_INCLUDE}"
    Delete "$INSTDIR\asset_registry.local.json"
    Delete "$INSTDIR\pipeline_cache.bin"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
