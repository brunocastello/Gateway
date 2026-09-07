; Gateway installer.
;
; Built by NSIS from Linux in CI, so it is produced the same way as everything
; else here rather than assembled by hand on a Windows box.
;
;   makensis -DVERSION=0.3.1 -DSRC=build-win32 installer/gateway.nsi
;
; The output is Setup.exe. It is named for what it does rather than for the
; program it carries, which is what the era's installers were called and what
; a floppy labelled "Gateway" wants on it.
;
; The default directory is C:\Gateway rather than Program Files. Windows 95
; does have Program Files -- it was introduced there -- but its name is not
; 8.3 clean, so on a FAT partition without long filename support it appears as
; PROGRA~1; and Gateway is one small executable and a settings file rather than
; a suite. A short path at the root is what this era's utilities did, and it is
; what the documentation can print without qualification.

; ANSI, not Unicode: the Unicode stub will not load on 95, 98 or Me. This has
; to come before anything that writes to the header or compresses data --
; including the MUI include below -- or NSIS refuses to change charset.
Unicode false

!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SRC
  !define SRC "build-win32"
!endif

Name "Gateway ${VERSION}"
OutFile "Setup.exe"
InstallDir "C:\Gateway"
InstallDirRegKey HKLM "Software\Gateway" "InstallDir"

; Windows 95 has no notion of elevation, and on 2000 and XP a directory the
; user chose themselves does not need it.
RequestExecutionLevel user

SetCompressor /SOLID lzma
BrandingText "Gateway ${VERSION}"

; No MUI_ICON or MUI_UNICON: the installer keeps NSIS's own install and
; uninstall icons. Setup is not Gateway, and giving it Gateway's icon makes
; two different things look like the same one in a folder or a Start Menu.

!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Gateway" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"

  File "${SRC}\Gateway.exe"
  File "/oname=Settings.md" "..\docs\prefs.md"
  File "/oname=LICENSE.txt" "..\LICENSE"

  ; Never overwrite an existing configuration. It holds the mail password and
  ; the OAuth refresh token, and replacing it on an upgrade would silently log
  ; the user out of their own mail.
  IfFileExists "$INSTDIR\Gateway.ini" +3 0
    File "/oname=Gateway.ini" "..\docs\prefs-example.txt"
    Goto +2
  DetailPrint "Keeping the existing Gateway.ini"

  CreateDirectory "$SMPROGRAMS\Gateway"
  CreateShortCut "$SMPROGRAMS\Gateway\Gateway.lnk" "$INSTDIR\Gateway.exe"
  CreateShortCut "$SMPROGRAMS\Gateway\Settings.lnk" "$INSTDIR\Gateway.ini"
  CreateShortCut "$SMPROGRAMS\Gateway\Uninstall Gateway.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "Software\Gateway" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Gateway" "Version" "${VERSION}"

  ; Add/Remove Programs.
  !define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Gateway"
  WriteRegStr HKLM "${UNINST}" "DisplayName"     "Gateway ${VERSION}"
  WriteRegStr HKLM "${UNINST}" "DisplayVersion"  "${VERSION}"
  WriteRegStr HKLM "${UNINST}" "Publisher"       "Bruno Castello"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon"     "$INSTDIR\Gateway.exe"
  WriteRegStr HKLM "${UNINST}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1

  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  ; Gateway may be sitting in the tray. Ask it to quit before removing it,
  ; or the file is in use and the uninstall leaves it behind.
  FindWindow $0 "GatewayWndClass" ""
  IntCmp $0 0 +3
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 1000

  Delete "$INSTDIR\Gateway.exe"
  Delete "$INSTDIR\Settings.md"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\Gateway.log"
  Delete "$INSTDIR\Uninstall.exe"

  ; The configuration is the user's, not ours: it holds their password and
  ; refresh token, so it is left unless they say otherwise.
  IfFileExists "$INSTDIR\Gateway.ini" 0 +4
    MessageBox MB_YESNO|MB_ICONQUESTION \
      "Remove your settings file as well?$\n$\nIt holds your mail password and sign-in token." \
      IDNO +2
    Delete "$INSTDIR\Gateway.ini"

  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\Gateway\Gateway.lnk"
  Delete "$SMPROGRAMS\Gateway\Settings.lnk"
  Delete "$SMPROGRAMS\Gateway\Uninstall Gateway.lnk"
  RMDir "$SMPROGRAMS\Gateway"

  ; The tray menu's "Start with Windows" writes this; leaving it behind would
  ; make Windows complain at every login about a program that is not there.
  ; Both hives, because that is where it may have gone: Windows 95 without
  ; user profiles has no per-user Run key, so the entry lands machine-wide.
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Gateway"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "Gateway"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Gateway"
  DeleteRegKey HKLM "Software\Gateway"
SectionEnd
