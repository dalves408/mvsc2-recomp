; Standalone, extraction-only Setup for the hi-res texture pack (the same flow as NHL Hitz Pro's addon).
; Built by scripts/build_installer.py. It finds the installed game, downloads the pack's .bin parts
; (or uses the ones beside it), checks them against texture_payload.iss and extracts them into texture_pack.
#include "texture_payload.iss"
#ifndef BuildDir
#define BuildDir "..\build_release\Release"
#endif
#ifndef GameGuid
#define GameGuid "B7E3A1D4-5F26-4C89-A0D7-2E9C61F84B3A"
#endif

[Setup]
AppId={{{#PackGuid}}
AppName=Marvel vs. Capcom 2 Hi-Res Textures
AppVersion={#TextureVersion}
AppPublisher=dalves408/mvsc2-recomp
AppPublisherURL=https://github.com/dalves408/mvsc2-recomp
DefaultDirName={code:GameDir}
UsePreviousAppDir=no
AppendDefaultDirName=no
DisableDirPage=no
DisableProgramGroupPage=yes
DirExistsWarning=no
Uninstallable=no
CreateUninstallRegKey=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=yes
CloseApplicationsFilter=mvc2_recomp.exe,config.exe
SetupIconFile=..\resources\mvc2.ico
WizardSmallImageFile=wizard_small_55.bmp,wizard_small_64.bmp,wizard_small_80.bmp,wizard_small_96.bmp,wizard_small_128.bmp
OutputBaseFilename=MvC2-HiRes-Textures-{#TextureVersion}-Setup
ArchiveExtraction=enhanced/nopassword
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Messages]
WelcomeLabel2=Add the hi-res textures to Marvel vs. Capcom 2.%n%nSetup uses the texture-*.bin file beside this EXE if it is there; otherwise it downloads it (about {#TextureDownloadBytes / 1000000} MB). An interrupted download resumes when you run Setup again.%n%nRequires game version 1.0.0 or newer.
SelectDirLabel3=Choose your installed Marvel vs. Capcom 2 game folder.
ButtonInstall=&Extract
ReadyLabel1=Ready to extract the hi-res texture pack.
ReadyLabel2a=Click Extract to continue, or Back to choose another game folder.
StatusExtractFiles=Extracting texture pack...
FinishedHeadingLabel=Texture pack extracted
FinishedLabel=Hi-res textures are on unless you switched them off. To change it, open the Marvel vs. Capcom 2 Settings program (Textures tab), or press F1 in the game and set Hi-res textures.

[Files]
Source: "{#BuildDir}\mvc2_texture_download.exe"; Flags: dontcopy
Source: "{#BuildDir}\config.exe"; DestDir: "{app}"; Flags: replacesameversion

[Code]
#include "texture_download_code.iss"
const
  GameKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#GameGuid}}_is1';

function GameExists(Dir: String): Boolean;
begin
  Result := FileExists(Dir + '\mvc2_recomp.exe');
end;

function FindGameDir: String;
var Dir: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, GameKey, 'Inno Setup: App Path', Dir) then
    if GameExists(RemoveBackslashUnlessRoot(Dir)) then
      Result := RemoveBackslashUnlessRoot(Dir);
  if Result = '' then
    if RegQueryStringValue(HKLM, GameKey, 'Inno Setup: App Path', Dir) then
      if GameExists(RemoveBackslashUnlessRoot(Dir)) then
        Result := RemoveBackslashUnlessRoot(Dir);
end;

function GameDir(Param: String): String;
begin
  Result := FindGameDir;
  if Result = '' then Result := 'C:\Games\Marvel vs. Capcom 2';
end;

function SupportedGame(Exe: String): Boolean;
var MS, LS: Cardinal;
begin
  Result := False;
  if GetVersionNumbers(Exe, MS, LS) then
    Result := MS >= $00010000;
end;

function CheckGame(Dir: String): String;
begin
  Result := '';
  if not GameExists(Dir) then
    Result := 'Choose the installed Marvel vs. Capcom 2 game folder (the one with mvc2_recomp.exe).'
  else if not SupportedGame(Dir + '\mvc2_recomp.exe') then
    Result := 'This texture pack requires Marvel vs. Capcom 2 version 1.0.0 or newer. Update the game first.';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var Error: String;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not WizardSilent then begin
    Error := CheckGame(RemoveBackslashUnlessRoot(WizardDirValue));
    if Error <> '' then begin
      MsgBox(Error, mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  { Silent installs must fail cleanly instead of waiting at a directory-page dialog. }
  Result := CheckGame(RemoveBackslashUnlessRoot(ExpandConstant('{app}')));
  if Result = '' then Result := PrepareTexturePayload(1048576);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpReady then begin
    if TexturePackNeedsInstall then begin
      WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
      WizardForm.PageNameLabel.Caption := SetupMessage(msgWizardReady);
      WizardForm.PageDescriptionLabel.Caption := SetupMessage(msgReadyLabel1);
      WizardForm.ReadyLabel.Caption := SetupMessage(msgReadyLabel2a);
    end else begin
      WizardForm.NextButton.Caption := '&Continue';
      WizardForm.PageNameLabel.Caption := 'Texture pack already installed';
      WizardForm.PageDescriptionLabel.Caption := 'The current release is already present in this game folder.';
      WizardForm.ReadyLabel.Caption := 'Click Continue to finish. No texture download or extraction is needed.';
    end;
  end;
  if (CurPageID = wpFinished) and TexturePackAlreadyCurrent then begin
    WizardForm.FinishedHeadingLabel.Caption := 'Texture pack already installed';
    WizardForm.FinishedLabel.Caption := 'The current hi-res texture pack is already installed. No download or extraction was needed.';
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo;
  if not TexturePackNeedsInstall then
    Result := Result + NewLine + NewLine + 'The current hi-res texture pack is already installed. No download or extraction is needed.'
  else if TextureAllPartsLocal then
    Result := Result + NewLine + NewLine + 'Setup will check and use the texture archive beside it.'
  else
    Result := Result + NewLine + NewLine + 'Setup will download the texture archive (about {#TextureDownloadBytes / 1000000} MB).';
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  TextureCancelButtonClick(Cancel, Confirm);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    InstallTexturePack;
  if CurStep = ssDone then TextureFinishCacheChoice;
end;

procedure DeinitializeSetup;
begin
  TextureCleanupOnExit;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if not TexturePackInstalled then Result := 1;
end;
