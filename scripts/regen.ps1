$ErrorActionPreference='Continue'
$env:Path=[Environment]::GetEnvironmentVariable('Path','Machine')+';'+[Environment]::GetEnvironmentVariable('Path','User')
Set-Location C:\Recomp\tools\xboxrecomp
$xbe='C:\Recomp\MvsC2\extracted\default.xbe'
$o='C:\Recomp\MvsC2'
"start disasm $(Get-Date)" | Out-File $o\pipe_log.txt
py -3.12 -m tools.disasm $xbe --text-only -v --force --extra-sections D3D,D3DX,XGRPH,DSOUND,XPP --seed-functions C:\Recomp\mvc2_pc\config\seed_functions.json > $o\disasm_out2.txt 2> $o\disasm_err2.txt
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: tools.disasm exited with $LASTEXITCODE (see $o)" -ForegroundColor Red; "FATAL tools.disasm rc=$LASTEXITCODE $(Get-Date)" | Out-File $o\pipe_log.txt -Append; exit 1 }
"start func_id $(Get-Date)" | Out-File $o\pipe_log.txt -Append
py -3.12 -m tools.func_id $xbe -v > $o\funcid_out2.txt 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: tools.func_id exited with $LASTEXITCODE (see $o)" -ForegroundColor Red; "FATAL tools.func_id rc=$LASTEXITCODE $(Get-Date)" | Out-File $o\pipe_log.txt -Append; exit 1 }
"start abi $(Get-Date)" | Out-File $o\pipe_log.txt -Append
py -3.12 -m tools.abi_analysis $xbe -v > $o\abi_out2.txt 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: tools.abi_analysis exited with $LASTEXITCODE (see $o)" -ForegroundColor Red; "FATAL tools.abi_analysis rc=$LASTEXITCODE $(Get-Date)" | Out-File $o\pipe_log.txt -Append; exit 1 }
"start recomp $(Get-Date)" | Out-File $o\pipe_log.txt -Append
py -3.12 -m tools.recomp $xbe --all --split 250 --gen-dir C:\Recomp\mvc2_pc\src\recomp\gen --game-name "Marvel vs. Capcom 2" > $o\recomp_out2.txt 2> $o\recomp_err2.txt
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: tools.recomp exited with $LASTEXITCODE (see $o)" -ForegroundColor Red; "FATAL tools.recomp rc=$LASTEXITCODE $(Get-Date)" | Out-File $o\pipe_log.txt -Append; exit 1 }
"done $(Get-Date)" | Out-File $o\pipe_log.txt -Append
# The KickOff hook (sub_002893C0) is applied by patch_gen.ps1 below.
# Post-generation fixes to the USB cancel path (scripts\patch_gen.ps1). The generated code is not in git, so a silently
# unpatched build would look like a regression: fail loudly if any anchor does not match or the marker is missing afterwards.
try { & C:\Recomp\mvc2_pc\scripts\patch_gen.ps1 } catch { Write-Host "FATAL: patch_gen.ps1 failed: $_" -ForegroundColor Red; exit 1 }
if (-not (Select-String -Path C:\Recomp\mvc2_pc\src\recomp\gen\recomp_*.c -Pattern "mvc2_cancel_completed" -Quiet) -or -not (Select-String -Path C:\Recomp\mvc2_pc\src\recomp\gen\recomp_*.c -Pattern "recomp_kick_fast" -Quiet) -or -not (Select-String -Path C:\Recomp\mvc2_pc\src\recomp\gen\recomp_*.c -Pattern "mvc2_kick_put" -Quiet)) { Write-Host "FATAL: generated code is not patched (mvc2_cancel_completed, recomp_kick_fast or mvc2_kick_put missing)" -ForegroundColor Red; exit 1 }
