Unicode true
ManifestSupportedOS all
RequestExecutionLevel user

!ifndef STAGE_DIR
    !error "STAGE_DIR is required"
!endif
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
    !define PRODUCT_VERSION "0.1.0.0"
!endif

Name "Witness Game"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\SaidaEngine\Witness Game"
BrandingText "Saida Engine"

SetCompressor /SOLID zlib
SetDatablockOptimize on
SetDateSave off
CRCCheck force
AutoCloseWindow true
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "CompanyName" "Saida"
VIAddVersionKey /LANG=1033 "FileDescription" "Witness Game Installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright Saida"
VIAddVersionKey /LANG=1033 "ProductName" "Witness Game"
VIAddVersionKey /LANG=1033 "ProductVersion" "${PRODUCT_VERSION}"

Page directory
Page instfiles
UninstPage instfiles

Section "Witness Game" SecMain
    !include "${PAYLOAD_INCLUDE}"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
    !include "${UNINSTALL_INCLUDE}"
    ; Runtime-regenerable files are intentionally absent from the immutable
    ; payload inventory but may be created beside the executable after a run.
    Delete "$INSTDIR\asset_registry.local.json"
    Delete "$INSTDIR\pipeline_cache.bin"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
