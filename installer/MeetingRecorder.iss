; MeetingRecorder installer (Inno Setup 6.2+). Built by tools\release.ps1, which
; passes the version and, for signed builds, the "certum" sign tool:
;   ISCC /DMyAppVersion=1.0.0 [/DSignBuild=1 /Scertum=...] installer\MeetingRecorder.iss
;
; Per-user install (no admin prompt) into %LOCALAPPDATA%\Programs, matching the
; app's per-user Start at login entry. Recordings and settings are left in
; place on uninstall.

#ifndef MyAppVersion
#define MyAppVersion "0.0.0"
#endif
#define MyAppName "MeetingRecorder"
#define MyAppPublisher "Fareedoon Ahmed"
#define MyAppURL "https://github.com/Feridoun/MeetingRecorder"
#define MyAppExeName "MeetingRecorder.exe"

[Setup]
; Unchanged from when the app was called CallRecorder, so this upgrades it.
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
; Don't reuse a CallRecorder folder; CurStepChanged removes it instead.
UsePreviousAppDir=no
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
OutputDir=..\build\installer
OutputBaseFilename=MeetingRecorder-Setup-{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile=..\LICENSE
SetupIconFile=..\res\MeetingRecorder.ico
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
Name: "startup"; Description: "Start MeetingRecorder when I sign in to Windows"; GroupDescription: "Options:"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; Shortcuts from when the app was called CallRecorder.
Type: files; Name: "{autoprograms}\CallRecorder.lnk"
Type: files; Name: "{autodesktop}\CallRecorder.lnk"

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; The same value the app's own Start at login setting manages.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "MeetingRecorder"; \
    ValueData: """{app}\{#MyAppExeName}"""; Tasks: startup

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[Code]
const
  WM_CLOSE = $0010;
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  ApprovedKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run';
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{1A252DF0-BF8F-4B2B-AE25-0A195056029E}_is1';

var
  OldAppDir: String;

// Asks a running copy to exit the way its Exit menu item does (a recording in
// progress is stopped and saved), then waits up to 15 s.
function CloseWindowClass(ClassName: String): Boolean;
var
  Window: HWND;
  Waited: Integer;
begin
  Result := True;
  Window := FindWindowByClassName(ClassName);
  if Window = 0 then Exit;
  PostMessage(Window, WM_CLOSE, 0, 0);
  Waited := 0;
  while (FindWindowByClassName(ClassName) <> 0) and (Waited < 15000) do begin
    Sleep(250);
    Waited := Waited + 250;
  end;
  Result := FindWindowByClassName(ClassName) = 0;
end;

// MeetingRecorder, or CallRecorder as it was called up to 1.2.
function CloseRunningApp(): Boolean;
begin
  Result := CloseWindowClass('MeetingRecorderWindow') and CloseWindowClass('CallRecorderWindow');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // Where the previous version is, read before this install records its own.
  if not RegQueryStringValue(HKEY_CURRENT_USER, UninstallKey, 'InstallLocation', OldAppDir) then
    OldAppDir := '';
  if CloseRunningApp() then
    Result := ''
  else
    Result := 'MeetingRecorder is still running. Exit it from its tray icon, then run Setup again.';
end;

// An upgrade from CallRecorder installs to a new folder, so remove the old
// program files, and the folder if nothing else is in it. Settings and
// recordings live elsewhere; the app moves those itself.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Old: String;
begin
  if CurStep <> ssPostInstall then Exit;
  Old := RemoveBackslashUnlessRoot(OldAppDir);
  if (Old = '') or (CompareText(Old, RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0) then Exit;
  if not FileExists(Old + '\CallRecorder.exe') then Exit;
  DeleteFile(Old + '\CallRecorder.exe');
  DeleteFile(Old + '\LICENSE.txt');
  DeleteFile(Old + '\THIRD_PARTY_NOTICES.txt');
  DeleteFile(Old + '\unins000.exe');
  DeleteFile(Old + '\unins000.dat');
  RemoveDir(Old);
end;

function InitializeUninstall(): Boolean;
begin
  Result := CloseRunningApp();
  if not Result then
    MsgBox('MeetingRecorder is still running. Exit it from its tray icon, then uninstall again.', mbError, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then begin
    // Also covers the entry made by the app's own Start at login setting.
    RegDeleteValue(HKEY_CURRENT_USER, RunKey, 'MeetingRecorder');
    RegDeleteValue(HKEY_CURRENT_USER, ApprovedKey, 'MeetingRecorder');
    // Left by CallRecorder if the app was never started after the upgrade.
    RegDeleteValue(HKEY_CURRENT_USER, RunKey, 'CallRecorder');
    RegDeleteValue(HKEY_CURRENT_USER, ApprovedKey, 'CallRecorder');
  end;
end;
