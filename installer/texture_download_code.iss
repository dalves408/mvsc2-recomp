{ Shared by the game Setup and the extraction-only standalone texture addon (the same flow as NHL Hitz Pro's).
  The caller includes texture_payload.iss first, for the pinned Texture* constants.

  For each part: use the copy beside Setup if it is the right one, else the cached download, else download it
  (resumable) into %LOCALAPPDATA%\MvC2Recomp\Downloads\<part SHA-256>. Every part must match its pinned size and
  SHA-256 before anything is extracted. The parts are extracted into a staging folder, the result is checked against
  the pinned manifest, and only then swapped into texture_pack. /REPAIRTEXTURES=1 reinstalls a current pack,
  /KEEPTEXTUREBIN=0 or 1 answers the keep-the-download question, /HIRES=yes selects the game Setup's texture task. }
var
  TextureProgress: TOutputProgressWizardPage;
  TextureTransferActive, TextureTransferCancelled, TextureExtractActive: Boolean;
  TexturePackInstalled: Boolean;
  TexturePackAlreadyCurrent: Boolean;
  TextureUsedCache, TextureCacheChoiceAsked, TextureDeleteCacheOnExit: Boolean;
  TexturePartPaths, TexturePartCaches: array[1..4] of String;
  TexturePart: Integer;
  TextureLastUiTick: LongWord;
  TexturePayloadPath, TextureCacheDir, TextureCancelFile, TextureError: String;
  TextureRatePhase: String;
  TextureRateTick: LongWord;
  TextureRateBytes, TextureRatePerSecond: Int64;
  TextureRateAvailable: Boolean;

function TextureSetupProcessId: LongWord;
  external 'GetCurrentProcessId@kernel32.dll stdcall';
function TextureTick: LongWord;
  external 'GetTickCount@kernel32.dll stdcall';

function GetFileAttributes(FileName: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';

procedure RegisterTexturePackEntry(AppDir: String); forward;

function TextureForceRepair: Boolean;
begin
  Result := ExpandConstant('{param:REPAIRTEXTURES|0}') = '1';
end;

{ The pinned download for part I }
function TexturePartUrl(I: Integer): String;
begin
  case I of
    1: Result := '{#TextureURL1}';
#if TexturePartCount >= 2
    2: Result := '{#TextureURL2}';
#endif
#if TexturePartCount >= 3
    3: Result := '{#TextureURL3}';
#endif
#if TexturePartCount >= 4
    4: Result := '{#TextureURL4}';
#endif
  else Result := '';
  end;
end;

function TexturePartSHA(I: Integer): String;
begin
  case I of
    1: Result := '{#TextureSHA2561}';
#if TexturePartCount >= 2
    2: Result := '{#TextureSHA2562}';
#endif
#if TexturePartCount >= 3
    3: Result := '{#TextureSHA2563}';
#endif
#if TexturePartCount >= 4
    4: Result := '{#TextureSHA2564}';
#endif
  else Result := '';
  end;
end;

function TexturePartBytes(I: Integer): Int64;
begin
  case I of
    1: Result := {#TextureBytes1};
#if TexturePartCount >= 2
    2: Result := {#TextureBytes2};
#endif
#if TexturePartCount >= 3
    3: Result := {#TextureBytes3};
#endif
#if TexturePartCount >= 4
    4: Result := {#TextureBytes4};
#endif
  else Result := 0;
  end;
end;

function TextureInstalledPackCurrent(Dir: String): Boolean;
var
  Manifest, Rest, Relative: String;
  Lines: TArrayOfString;
  I, Space: Integer;
  ExpectedSize, ActualSize: Int64;
begin
  Result := False;
  if Dir = '' then Exit;
  Dir := AddBackslash(Dir) + 'texture_pack\';
  Manifest := Dir + 'manifest.txt';
  if not FileExists(Manifest) then Exit;
  try
    { Pin the expected file list rather than trusting a folder or an editable version label. Every listed file
      is checked for its size, so a missing or truncated one is caught. An explicit repair restores them all. }
    if CompareText(GetSHA256OfFile(Manifest), '{#TextureManifestSHA256}') <> 0 then Exit;
    if not LoadStringsFromFile(Manifest, Lines) then Exit;
    if GetArrayLength(Lines) <> {#TextureFileCount} then Exit;
    for I := 0 to GetArrayLength(Lines) - 1 do begin
      Rest := Lines[I];
      Space := Pos(' ', Rest);
      if Space <> 65 then Exit;
      Delete(Rest, 1, Space);
      Space := Pos(' ', Rest);
      if Space <= 1 then Exit;
      ExpectedSize := StrToInt64Def(Copy(Rest, 1, Space - 1), -1);
      Relative := Copy(Rest, Space + 1, Length(Rest));
      StringChangeEx(Relative, '/', '\', True);
      if (Relative = '') or (ExpectedSize < 0) then Exit;
      if not FileSize64(Dir + Relative, ActualSize) then Exit;
      if ActualSize <> ExpectedSize then Exit;
    end;
    Result := True;
  except
    Result := False; { Unreadable metadata or files never suppress an install. }
  end;
end;

function TexturePackNeedsInstall: Boolean;
begin
  Result := TextureForceRepair or not TextureInstalledPackCurrent(WizardDirValue);
  if not Result then Log('The current texture pack is installed; the texture download offer is hidden.');
end;

function WantTexturePack: Boolean;
begin
#ifdef TextureOptional
  Result := WizardIsTaskSelected('hires') or
    (CompareText(ExpandConstant('{param:HIRES|no}'), 'yes') = 0);
#else
  Result := True;
#endif
end;

function TexturePartName: String;
begin
  Result := Format('texture-%.2d.bin', [TexturePart]);
end;

function TextureLocalPart: String;
begin
  Result := AddBackslash(ExpandConstant('{src}')) + TexturePartName;
end;

{ Whether every part is beside Setup (the Ready page says so) }
function TextureAllPartsLocal: Boolean;
var I: Integer;
begin
  Result := True;
  for I := 1 to {#TexturePartCount} do
    if not FileExists(AddBackslash(ExpandConstant('{src}')) + Format('texture-%.2d.bin', [I])) then
      Result := False;
end;

procedure TextureCancelButtonClick(var Cancel, Confirm: Boolean);
begin
  if TextureTransferActive then begin
    TextureTransferCancelled := True;
    SaveStringToFile(TextureCancelFile, 'cancel', False);
    if TextureExtractActive then
      TextureProgress.SetText('Stopping extraction...', 'The verified download is kept for next time.')
    else
      TextureProgress.SetText('Stopping download...', 'Saved progress will be resumed next time.');
    Cancel := False;
    Confirm := False;
  end;
end;

function TextureNetworkPhase(Phase: String): Boolean;
begin
  Result := (Phase = 'DOWNLOADING') or (Phase = 'RESUMING') or (Phase = 'RESTARTING');
end;

function TextureFormatBytes(Bytes: Int64): String;
begin
  if Bytes >= 1000000000 then Result := Format('%.2f GB', [Bytes / 1000000000.0])
  else if Bytes >= 1000000 then Result := Format('%.1f MB', [Bytes / 1000000.0])
  else if Bytes >= 1000 then Result := Format('%.1f KB', [Bytes / 1000.0])
  else Result := IntToStr(Bytes) + ' B';
end;

procedure TextureUpdateDownloadRate(Phase: String; Done: Int64; Tick: LongWord);
begin
  { Begin at the first reported offset: saved or resumed bytes are not new network traffic. Verification,
    retries restarting at zero and clock wrap reset the sample. Refresh speed about once a second. }
  if not TextureNetworkPhase(Phase) or (Phase <> TextureRatePhase) or
     (Tick < TextureRateTick) or (Done < TextureRateBytes) then begin
    TextureRatePhase := Phase;
    TextureRateTick := Tick;
    TextureRateBytes := Done;
    TextureRatePerSecond := 0;
    TextureRateAvailable := False;
  end else if Tick - TextureRateTick >= 1000 then begin
    TextureRatePerSecond := (Done - TextureRateBytes) * 1000 div (Tick - TextureRateTick);
    TextureRateAvailable := True;
    TextureRateTick := Tick;
    TextureRateBytes := Done;
  end;
end;

function TextureProgressDetails(Phase: String; Done, Total: Int64): String;
var Percent: Extended; Speed: String;
begin
  if Done < 0 then Done := 0;
  if Done > Total then Done := Total;
  Percent := 0;
  if Total > 0 then Percent := Done * 100.0 / Total;
  if TextureNetworkPhase(Phase) then begin
    if TextureRateAvailable then Speed := TextureFormatBytes(TextureRatePerSecond) + '/s'
    else Speed := 'Measuring speed...';
    Result := Format('%.1f%% - %.1f / %.1f MB - %s', [Percent, Done / 1000000.0, Total / 1000000.0, Speed]);
  end else if Phase = 'VERIFYING' then
    Result := Format('%s - %s - %.1f%% verified', [TexturePartName, TextureFormatBytes(Total), Percent])
  else Result := TexturePartName + ' - Size: ' + TextureFormatBytes(Total);
end;

procedure TextureDownloadLog(const S: String; const Error, FirstLine: Boolean);
var
  Rest, Phase, LabelText: String;
  K: Integer;
  Done, Total: Int64;
begin
  if Error then begin TextureError := S; Log(S); Exit; end;
  if Copy(S, 1, 9) = 'PROGRESS ' then begin
    Rest := Copy(S, 10, Length(S));
    K := Pos(' ', Rest);
    Done := StrToInt64Def(Copy(Rest, 1, K - 1), 0);
    Delete(Rest, 1, K);
    K := Pos(' ', Rest);
    Total := StrToInt64Def(Copy(Rest, 1, K - 1), 0);
    Phase := Copy(Rest, K + 1, Length(Rest));
    if TextureTransferCancelled then LabelText := 'Stopping download...'
    else if Phase = 'WAITING' then LabelText := 'Waiting for another texture download to finish...'
    else if Phase = 'RESUMING' then LabelText := 'Resuming texture pack download...'
    else if Phase = 'VERIFYING' then LabelText := 'Verifying texture pack...'
    else if Phase = 'RESTARTING' then LabelText := 'The server cannot resume this copy. Restarting download...'
    else if Phase = 'CHECKING' then LabelText := 'Checking for a local texture pack...'
    else LabelText := 'Downloading texture pack...';
    TextureUpdateDownloadRate(Phase, Done, TextureTick);
    if TextureNetworkPhase(Phase) and not TextureTransferCancelled then
      LabelText := LabelText + ' (' + TextureFormatBytes(Total) + ')';
    if {#TexturePartCount} > 1 then
      LabelText := 'Part ' + IntToStr(TexturePart) + ' of {#TexturePartCount}: ' + LabelText;
    TextureProgress.SetText(LabelText, TextureProgressDetails(Phase, Done, Total));
    TextureProgress.SetProgress(Done div 1024, Total div 1024);
  end else begin
    Log(S);
    if S = 'READY local' then begin
      TexturePayloadPath := TextureLocalPart;
      TextureUsedCache := False;
    end else if S = 'READY cache' then begin
      TexturePayloadPath := TextureCacheDir + '\texture.bin';
      TextureUsedCache := True;
    end
    else if Copy(S, 1, 6) = 'ERROR ' then TextureError := Copy(S, 7, Length(S));
  end;
end;

function PrepareTexturePart(Url, SHA: String; Bytes: Int64): String;
var
  Params, LocalParam: String;
  Code: Integer;
  Started, RetryWithoutLocal: Boolean;
begin
  Result := '';
  TextureCacheDir := ExpandConstant('{#TextureCacheRoot}') + '\' + SHA;
  TextureCancelFile := ExpandConstant('{tmp}\texture-download.cancel');
  if TextureProgress = nil then
    TextureProgress := CreateOutputProgressPage('Hi-res texture pack', 'Preparing the hi-res texture pack.');
  TextureProgress.SetText('Checking for a local texture pack...', TexturePartName);
  TextureProgress.SetProgress(0, 0);
  if not FileExists(TextureLocalPart) and
     not FileExists(TextureCacheDir + '\texture.bin') then begin
    if FileExists(TextureCacheDir + '\texture.bin.part') then
      TextureProgress.SetText('Resuming texture pack download...', 'Saved progress will be used.')
    else
      TextureProgress.SetText('The texture pack will be downloaded.', 'You can stop and resume it later.');
  end;
  TextureProgress.Show;
  TextureTransferActive := True;
  TextureTransferCancelled := False;
  WizardForm.CancelButton.Enabled := True;
  WizardForm.CancelButton.Visible := True;
  LocalParam := ' --local ' + AddQuotes(TextureLocalPart);
  try
    repeat
      DeleteFile(TextureCancelFile);
      TexturePayloadPath := '';
      TextureUsedCache := False;
      TextureError := '';
      TextureRatePhase := '';
      TextureRateAvailable := False;
      Params := '--url ' + AddQuotes(Url) +
                ' --cache ' + AddQuotes(TextureCacheDir) +
                ' --sha256 ' + SHA + ' --bytes ' + IntToStr(Bytes) +
                ' --cancel ' + AddQuotes(TextureCancelFile) +
                ' --parent ' + IntToStr(TextureSetupProcessId) + LocalParam;
      Code := 99;
      Started := ExecAndLogOutput(ExpandConstant('{tmp}\mvc2_texture_download.exe'), Params, '',
                                 SW_SHOWNORMAL, ewWaitUntilTerminated, Code, @TextureDownloadLog);
      RetryWithoutLocal := False;
      if Started and (Code = 5) and not TextureTransferCancelled then begin
        if WizardSilent then RetryWithoutLocal := True
        else RetryWithoutLocal := MsgBox('The ' + TexturePartName + ' beside Setup does not match this texture pack.' + #13#10#13#10 +
          'Download the correct copy instead? Your local file will be kept.', mbConfirmation, MB_YESNO) = IDYES;
        if RetryWithoutLocal then LocalParam := '';
      end;
    until not RetryWithoutLocal;
    if TextureTransferCancelled or (Code = 2) then
      Result := 'Download stopped. Saved progress is kept. Retry to resume, or Cancel to close Setup.'
    else if not Started or (Code <> 0) or (TexturePayloadPath = '') then begin
      if TextureError = '' then TextureError := 'The texture pack could not be prepared. Retry to continue the saved download.';
      Result := TextureError;
    end else begin
      TexturePartPaths[TexturePart] := TexturePayloadPath;
      if TextureUsedCache then TexturePartCaches[TexturePart] := TexturePayloadPath;
    end;
  finally
    TextureTransferActive := False;
    TextureProgress.Hide;
  end;
end;

function PrepareTexturePayload(OtherRequiredBytes: Int64): String;
var FreeBytes, TotalBytes: Int64; I: Integer;
begin
  Result := '';
  if not WantTexturePack then Exit;
  TexturePackAlreadyCurrent := not TextureForceRepair and TextureInstalledPackCurrent(WizardDirValue);
  TexturePackInstalled := TexturePackAlreadyCurrent;
  if TexturePackAlreadyCurrent then begin
    Log('The current texture pack is already installed; no download or extraction is needed.');
    RegisterTexturePackEntry(WizardDirValue);
    Exit;
  end;
  if not GetSpaceOnDisk64(AddBackslash(ExtractFileDrive(WizardDirValue)), FreeBytes, TotalBytes) then begin
    Result := 'Could not check free space on the selected game drive.';
    Exit;
  end;
  if FreeBytes < ({#TextureExtractedBytes} + {#TextureDownloadBytes} + OtherRequiredBytes) then begin
    Result := 'There is not enough free space to download and stage the textures and install the selected game files.';
    Exit;
  end;
  MapArchiveExtensions('.bin', '.7z');
  if not FileExists(ExpandConstant('{tmp}\mvc2_texture_download.exe')) then
    ExtractTemporaryFile('mvc2_texture_download.exe');
  for I := 1 to {#TexturePartCount} do begin
    TexturePartPaths[I] := '';
    TexturePartCaches[I] := '';
  end;
  for I := 1 to {#TexturePartCount} do begin
    TexturePart := I;
    Result := PrepareTexturePart(TexturePartUrl(I), TexturePartSHA(I), TexturePartBytes(I));
    if Result <> '' then Exit;
  end;
end;

procedure TextureFinishCacheChoice;
var Choice: String; I: Integer; Any: Boolean;
begin
  { ssDone is reached only after a successful install. Never offer to discard partial downloads or a verified
    archive still needed after a failed extraction. }
  Any := False;
  for I := 1 to {#TexturePartCount} do
    if TexturePartCaches[I] <> '' then Any := True;
  if not TexturePackInstalled or TexturePackAlreadyCurrent or TextureCacheChoiceAsked or not Any then Exit;
  TextureCacheChoiceAsked := True;
  Choice := ExpandConstant('{param:KEEPTEXTUREBIN|}');
  if Choice = '0' then TextureDeleteCacheOnExit := True
  else if (Choice = '1') or WizardSilent then TextureDeleteCacheOnExit := False
  else
    TextureDeleteCacheOnExit := MsgBox(
      'Keep the downloaded texture archive for future repairs or reinstalls?' + #13#10#13#10 +
      'Yes keeps the cached download. No deletes it when Setup closes.',
      mbConfirmation, MB_YESNO) = IDNO;
  if TextureDeleteCacheOnExit then Log('The user chose to delete the cached texture archive on Setup exit.')
  else Log('The cached texture archive is kept for future repairs or reinstalls.');
end;

procedure TextureCleanupOnExit;
var I: Integer;
begin
  if not TextureDeleteCacheOnExit or not TexturePackInstalled then Exit;
  for I := 1 to {#TexturePartCount} do
    if (TexturePartCaches[I] <> '') and FileExists(TexturePartCaches[I]) then
      if not DeleteFile(TexturePartCaches[I]) then Log('A cached texture archive may still be in use.');
end;

function TextureExtractionProgress(const ArchiveName, FileName: String; const Progress, ProgressMax: Int64): Boolean;
var Tick: LongWord;
begin
  Tick := TextureTick;
  if (Tick - TextureLastUiTick >= 100) or (Progress = ProgressMax) then begin
    TextureLastUiTick := Tick;
    TextureProgress.SetText('Extracting hi-res textures...', ExtractFileName(FileName));
    TextureProgress.SetProgress(Progress div 1024, ProgressMax div 1024);
  end;
  Result := not TextureTransferCancelled;
end;

const
  TexturePackKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#PackGuid}}_is1';

{ The addon is extraction-only (no uninstaller of its own), so Programs and Features gets its entry here, with the
  revision as its version. The game's uninstaller removes it along with texture_pack. }
procedure RegisterTexturePackEntry(AppDir: String);
var
  SizeKB: Int64;
begin
  AppDir := RemoveBackslashUnlessRoot(AppDir);
  if AppDir = '' then Exit;
  if not RegWriteStringValue(HKCU, TexturePackKey, 'DisplayName', 'Marvel vs. Capcom 2 Hi-Res Textures') then begin
    Log('Could not register the hi-res texture pack in Programs and Features.');
    Exit;
  end;
  RegWriteStringValue(HKCU, TexturePackKey, 'DisplayVersion', '{#TextureDisplayVersion}');
  RegWriteStringValue(HKCU, TexturePackKey, 'Publisher', 'dalves408/mvsc2-recomp');
  RegWriteStringValue(HKCU, TexturePackKey, 'URLInfoAbout', 'https://github.com/dalves408/mvsc2-recomp');
  RegWriteStringValue(HKCU, TexturePackKey, 'InstallLocation', AddBackslash(AppDir));
  RegWriteStringValue(HKCU, TexturePackKey, 'Inno Setup: App Path', AppDir);
  RegWriteStringValue(HKCU, TexturePackKey, 'DisplayIcon', AddBackslash(AppDir) + 'mvc2_recomp.exe');
  RegWriteDWordValue(HKCU, TexturePackKey, 'NoModify', 1);
  RegWriteDWordValue(HKCU, TexturePackKey, 'NoRepair', 1);
  SizeKB := {#TextureExtractedBytes} div 1024;
  if SizeKB > $FFFFFFFF then SizeKB := $FFFFFFFF;
  RegWriteDWordValue(HKCU, TexturePackKey, 'EstimatedSize', SizeKB);
  Log('Registered hi-res textures as {#TextureDisplayVersion} in Programs and Features.');
end;

{ An existing texture_pack this Setup recognises (an earlier copy of this revision) can be deleted after the swap;
  anything else the player put there is kept beside it. }
function TextureKnownPack(PackDir: String): Boolean;
begin
  Result := False;
  if not FileExists(PackDir + '\manifest.txt') then Exit;
  try
    Result := CompareText(GetSHA256OfFile(PackDir + '\manifest.txt'), '{#TextureManifestSHA256}') = 0;
  except
    Result := False;
  end;
end;

procedure InstallTexturePack;
var
  AppDir, PackDir, Stage, Backup: String;
  I: Integer;
  OldMoved, KnownPack: Boolean;
begin
  if not WantTexturePack or TexturePackAlreadyCurrent then Exit;
  for I := 1 to {#TexturePartCount} do
    if TexturePartPaths[I] = '' then
      RaiseException('Every texture archive must be verified before extraction.');
  AppDir := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  PackDir := AddBackslash(AppDir) + 'texture_pack';
  Stage := AddBackslash(AppDir) + '.texture-stage-' + IntToStr(TextureSetupProcessId);
  Backup := PackDir + '.previous-' + IntToStr(TextureSetupProcessId);
  if DirExists(Stage) or FileExists(Stage) or DirExists(Backup) or FileExists(Backup) then
    RaiseException('A texture staging or backup folder already exists.');
  if not CreateDir(Stage) then RaiseException('Cannot create the texture staging folder.');
  OldMoved := False;
  TextureTransferCancelled := False;
  TextureExtractActive := True;
  TextureTransferActive := True;
  TextureLastUiTick := 0;
  TextureProgress.SetText('Extracting hi-res textures...', 'Preparing the archive');
  TextureProgress.SetProgress(0, 0);
  TextureProgress.Show;
  WizardForm.CancelButton.Enabled := True;
  WizardForm.CancelButton.Visible := True;
  try
    try
      for I := 1 to {#TexturePartCount} do
        ExtractArchive(TexturePartPaths[I], Stage, '', True, @TextureExtractionProgress);
      if TextureTransferCancelled or not TextureInstalledPackCurrent(Stage) then
        RaiseException('The staged texture pack is incomplete.');
      KnownPack := TextureKnownPack(PackDir);
      if DirExists(PackDir) then begin
        if not RenameFile(PackDir, Backup) then RaiseException('Cannot move the old texture folder.');
        OldMoved := True;
      end;
      if not RenameFile(Stage + '\texture_pack', PackDir) then begin
        if OldMoved and not RenameFile(Backup, PackDir) then
          RaiseException('Cannot restore the previous texture folder. It is saved at ' + Backup);
        RaiseException('Cannot move the verified texture pack into the game folder.');
      end;
      TexturePackInstalled := True;
      if OldMoved then begin
        if KnownPack and ((GetFileAttributes(Backup) and $400) = 0) then begin
          if not DelTree(Backup, True, True, True) then Log('Old texture backup remains at ' + Backup);
        end else Log('Previous texture folder preserved at ' + Backup);
      end;
      RegisterTexturePackEntry(AppDir);
      Log('{#TextureDisplayVersion} texture pack extraction completed.');
    except
      if TextureTransferCancelled then
        RaiseException('Texture extraction stopped. The verified downloads are kept for the next run.')
      else RaiseException('Texture installation could not finish. ' + GetExceptionMessage);
    end;
  finally
    DelTree(Stage, True, True, True);
    TextureTransferActive := False;
    TextureExtractActive := False;
    TextureProgress.Hide;
  end;
end;
