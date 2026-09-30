; CallRecorder installer (Inno Setup 6.2+). Built by tools\release.ps1, which
; passes the version and, for signed builds, the "certum" sign tool:
;   ISCC /DMyAppVersion=1.0.0 [/DSignBuild=1 /Scertum=...] installer\CallRecorder.iss
;
; Per-user install (no admin prompt) into %LOCALAPPDATA%\Programs, matching the
; app's per-user Start at login entry. Recordings and settings are left in
; place on uninstall.

#ifndef MyAppVersion
#define MyAppVersion "0.0.0"
#endif
#define MyAppName "CallRecorder"
#define MyAppPublisher "Fareedoon Ahmed"
#define MyAppURL "https://github.com/Feridoun/CallRecorder"
#define MyAppExeName "CallRecorder.exe"

[Setup]
AppId={{1A252DF0-BF8F-4B2B-AE25-0A195056029E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Setup
VersionInfoProductName={#MyAppName}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
OutputDir=..\build\installer
OutputBaseFilename=CallRecorder-Setup-{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile=..\LICENSE
SetupIconFile=..\res\CallRecorder.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
; Closing the app is handled in [Code] below, gracefully, so a recording in
; progress is saved rather than killed.
CloseApplications=no
#ifdef SignBuild
SignTool=certum
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Start CallRecorder when I sign in to Windows"; GroupDescription: "Options:"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; The same value the app's own Start at login setting manages.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "CallRecorder"; \
    ValueData: """{app}\{#MyAppExeName}"""; Tasks: startup

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[Code]
const
  WM_CLOSE = $0010;
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  ApprovedKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run';

// Asks a running CallRecorder to exit the way its Exit menu item does (a
// recording in progress is stopped and saved), then waits up to 15 s.
function CloseRunningApp(): Boolean;
var
  Window: HWND;
  Waited: Integer;
begin
  Result := True;
  Window := FindWindowByClassName('CallRecorderWindow');
  if Window = 0 then Exit;
  PostMessage(Window, WM_CLOSE, 0, 0);
  Waited := 0;
  while (FindWindowByClassName('CallRecorderWindow') <> 0) and (Waited < 15000) do begin
    Sleep(250);
    Waited := Waited + 250;
  end;
  Result := FindWindowByClassName('CallRecorderWindow') = 0;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if CloseRunningApp() then
    Result := ''
  else
    Result := 'CallRecorder is still running. Exit it from its tray icon, then run Setup again.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := CloseRunningApp();
  if not Result then
    MsgBox('CallRecorder is still running. Exit it from its tray icon, then uninstall again.', mbError, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then begin
    // Also covers the entry made by the app's own Start at login setting.
    RegDeleteValue(HKEY_CURRENT_USER, RunKey, 'CallRecorder');
    RegDeleteValue(HKEY_CURRENT_USER, ApprovedKey, 'CallRecorder');
  end;
end;
