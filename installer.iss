; Inno Setup script for the UDP Stream Output OBS plugin.
;
; Installs into OBS Studio's per-machine additional-plugins directory
; (%ProgramData%\obs-studio\plugins\<name>\), which OBS Studio (v28+) scans
; automatically on startup. This means the installer does NOT need to
; locate the user's actual OBS Studio install directory at all.
;
; Place this file at the root of the plugin project (same level as
; build_x64\), or adjust SourceRundir below to match wherever you put it.
;
; Build with Inno Setup (https://jrsoftware.org/isinfo.php):
;   1. Open this file in the Inno Setup Compiler (or run ISCC.exe installer.iss)
;   2. Output .exe appears in installer_output\

; PluginName/PluginVersion/RundirConfig are all passed in by CI via ISCC's
; /D command line switch (Package-Windows.ps1) -- but a plain, unconditional
; #define here would silently win over that anyway, since Inno Setup
; Preprocessor treats a /D switch as if it were the first line of the
; script, and a later #define for the same name is a plain (silent, no
; warning) redefinition. That was a real, undiscovered bug: every CI build
; had its /DPluginVersion=/DPluginName= command-line overrides quietly
; discarded in favor of whatever was hardcoded below, and only the CMake
; side (buildspec.json's own version, consumed independently) ever actually
; followed a release tag. Guarding each with #ifndef makes the hardcoded
; value a fallback for a manual/local ISCC run with no /D switches, while
; letting CI's actual command-line values take effect as intended.
#ifndef PluginName
  #define PluginName "Colour"     ; must exactly match buildspec.json "name"
#endif
#define PluginDisplayName "Colour"
#ifndef PluginVersion
  #define PluginVersion "1.0.8"              ; must match buildspec.json "version"
#endif
#ifndef RundirConfig
  ; Matches the CMake preset's build configuration for a normal (non-tag)
  ; CI build. A version-tag-triggered release build instead compiles in
  ; "Release" config (see build-project.yaml's check-event job), which
  ; Package-Windows.ps1 now passes through as /DRundirConfig=Release --
  ; this default only applies when that override isn't supplied (e.g. a
  ; manual local build). Without this override, a real tagged-release build
  ; always failed here: ISCC would still look for the DLL under
  ; build_x64\rundir\RelWithDebInfo\ even though the actual build placed it
  ; under the Release config folder instead.
  #define RundirConfig "RelWithDebInfo"
#endif
; NOTE: this build's post-build step copies the DLL/PDB flat into
; rundir\<config>\, and only the data/ folder contents into a
; rundir\<config>\<name>\ subfolder. Confirmed directly from the verbose
; MSBuild log rather than assumed from the template's usual layout.
#define SourceRundir "build_x64\rundir\" + RundirConfig

[Setup]
; Generate your own unique GUID for a real release (in Inno Setup's Tools
; menu: "Generate GUID"), then keep it stable across versions so updates
; upgrade in-place instead of creating duplicate Add/Remove Programs entries.
AppId={{B6C3F2B0-7B9E-4B6E-9A2E-1F3C8D4E5A6B}
AppName={#PluginDisplayName}
AppVersion={#PluginVersion}
AppPublisher=Your Name Here
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
Compression=lzma2
SolidCompression=yes
OutputDir=installer_output
OutputBaseFilename={#PluginName}-{#PluginVersion}-installer
UninstallDisplayName={#PluginDisplayName}
WizardStyle=modern

[Files]
Source: "{#SourceRundir}\{#PluginName}.dll"; \
    DestDir: "{commonappdata}\obs-studio\plugins\{#PluginName}\bin\64bit"; \
    Flags: ignoreversion

Source: "{#SourceRundir}\{#PluginName}.pdb"; \
    DestDir: "{commonappdata}\obs-studio\plugins\{#PluginName}\bin\64bit"; \
    Flags: ignoreversion skipifsourcedoesntexist

Source: "{#SourceRundir}\{#PluginName}\*"; \
    DestDir: "{commonappdata}\obs-studio\plugins\{#PluginName}\data"; \
    Flags: ignoreversion recursesubdirs createallsubdirs; \
    Check: DirExists(ExpandConstant('{#SourceRundir}\{#PluginName}'))

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\{#PluginName}"

[Code]
function InitializeSetup(): Boolean;
begin
  // Best-effort reminder only — I can't confirm OBS's internal mutex name,
  // so this dialog always shows rather than risk silently skipping a real
  // warning. If you confirm the actual mutex name, swap in
  // CheckForMutexes('<real name>') to make it conditional.
  if MsgBox('If OBS Studio is currently running, please close it before continuing — ' +
            'otherwise the plugin files may not update correctly.' + #13#10#13#10 +
            'Continue?', mbConfirmation, MB_YESNO) = IDNO then
  begin
    Result := False;
    Exit;
  end;
  Result := True;
end;
