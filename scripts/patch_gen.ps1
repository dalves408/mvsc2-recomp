# Post-regen patches to the generated code (src/recomp/gen is gitignored, so they are re-applied here).
# Idempotent; P0 always, P1 only with -DpcGuard. Run after scripts\regen.ps1 (regen.ps1 calls it).
param([switch]$DpcGuard, [switch]$Hoplat)
$ErrorActionPreference = 'Stop'
$enc = New-Object System.Text.UTF8Encoding($false)
function Edit-Sub($name, $marker, [scriptblock]$edit) {
  $f = Get-ChildItem C:\Recomp\mvc2_pc\src\recomp\gen\recomp_*.c | Where-Object { Select-String -Path $_.FullName -Pattern "void $name\(void\)" -Quiet }
  if (-not $f) { throw "$name not found in generated code" }
  $c = [IO.File]::ReadAllText($f.FullName)
  if ($c.Contains($marker)) { "$name : already patched ($marker)"; return }
  $c = & $edit $c
  [IO.File]::WriteAllText($f.FullName, $c, $enc)
  "$name : patched in $($f.Name)"
}
function Sub1($c, $re, $rep, $what) {
  $m = [regex]::Matches($c, $re)
  if ($m.Count -ne 1) { throw "anchor '$what' matched $($m.Count) times, expected 1" }
  return [regex]::Replace($c, $re, $rep)
}
# KickOff hook (src/recomp_manual.c, recomp_kick_fast): direct calls bypass recomp_lookup_manual, so sub_002893C0 asks the override itself.
# Was a silent regex in regen.ps1 with {4} header lines; the generator emits 3, so the hook was NOT applied after a regen.
Edit-Sub 'sub_002893C0' 'recomp_kick_fast' {
  param($c)
  return (Sub1 $c '(void sub_002893C0\(void\)\r?\n\{\r?\n(?:[^\r\n]*\r?\n){3}    \(void\)_fa; \(void\)_fb; \(void\)_fas; \(void\)_fbs;\r?\n)' ('$1    { extern int recomp_kick_fast(void); if (recomp_kick_fast()) return; }' + "`n") 'kick_fast')
}
# Flip drain (src/recomp_manual.c, mvc2_kick_put): right after KickOff writes DMA_PUT, a segment ending a frame waits until the
# executor has drawn it, so the title cannot rewrite sprite tile pages that are still queued (Ryu's Shinku Hadoken tiling).
Edit-Sub 'sub_002893C0' 'mvc2_kick_put' {
  param($c)
  return (Sub1 $c '(loc_0028940C: ;\r?\n(?:[^\r\n]*\r?\n){4}    MEM32\(eax \+ 0x40\) = edx;\r?\n)' ('$1    { extern void mvc2_kick_put(uint32_t, uint32_t); mvc2_kick_put(eax, edx); }' + "`n") 'kick_put')
}
# DrawVerticesUP probe (src/recomp_manual.c, mvc2_dvup_probe): logs who draws what while the frame-trace flag exists.
Edit-Sub 'sub_002870F0' 'mvc2_dvup_probe' {
  param($c)
  return (Sub1 $c '(void sub_002870F0\(void\)\r?\n\{\r?\n(?:[^\r\n]*\r?\n){3,6}?    \(void\)_fa; \(void\)_fb; \(void\)_fas; \(void\)_fbs;\r?\n)' ('$1    { extern void mvc2_dvup_probe(void); mvc2_dvup_probe(); }' + "`n") 'dvup')
}
# Sprite renderer probe (src/recomp_manual.c, mvc2_spr_probe): arguments of the cell layout for each sprite, while the flag exists.
Edit-Sub 'sub_001F6FD0' 'mvc2_spr_probe' {
  param($c)
  return (Sub1 $c '(void sub_001F6FD0\(void\)\r?\n\{\r?\n(?:[^\r\n]*\r?\n){3,6}?    \(void\)_fa; \(void\)_fb; \(void\)_fas; \(void\)_fbs;\r?\n)' ('$1    { extern void mvc2_spr_probe(void); mvc2_spr_probe(); }' + "`n") 'spr')
}
# Sprite object probe (src/recomp_manual.c, mvc2_obj_probe): each drawn sprite object's position/scale/frame in draw order.
Edit-Sub 'sub_0004EDE0' 'mvc2_obj_probe' {
  param($c)
  return (Sub1 $c '(void sub_0004EDE0\(void\)\r?\n\{\r?\n(?:[^\r\n]*\r?\n){3,6}?    \(void\)_fa; \(void\)_fb; \(void\)_fas; \(void\)_fbs;\r?\n)' ('$1    { extern void mvc2_obj_probe(void); mvc2_obj_probe(); }' + "`n") 'obj')
}
# Object-list insert probe (src/recomp_manual.c, mvc2_list_probe): head/tail inserts, RECOMP_OBJ_SPAWNLOG=1.
Edit-Sub 'sub_001E2830' 'mvc2_list_probe(0)' {
  param($c)
  return (Sub1 $c '(void sub_001E2830\(void\)\r?\n\{\r?\n(?:[^\r\n]*\r?\n){3,6}?    \(void\)_fa; \(void\)_fb; \(void\)_fas; \(void\)_fbs;\r?\n)' ('$1    { extern void mvc2_list_probe(int); mvc2_list_probe(0); }' + "`n") 'list head')
}
Edit-Sub 'sub_001E2890' 'mvc2_list_probe(1)' {
  param($c)
  return (Sub1 $c '(void sub_001E2890\(void\)\r?\n\{\r?\n(?:[^\r\n]*\r?\n){3,6}?    \(void\)_fa; \(void\)_fb; \(void\)_fas; \(void\)_fbs;\r?\n)' ('$1    { extern void mvc2_list_probe(int); mvc2_list_probe(1); }' + "`n") 'list tail')
}
# Tile-cache probe (src/recomp_manual.c, mvc2_tile_probe): cell key / slot / hit after the lookup in the sprite renderer.
Edit-Sub 'sub_001F6FD0' 'mvc2_tile_probe' {
  param($c)
  return (Sub1 $c '(loc_001F7242: ;\r?\n)' ('$1    { extern void mvc2_tile_probe(uint32_t); mvc2_tile_probe(eax); }' + "`n") 'tile')
}
# Animation-setter probe (src/recomp_manual.c, mvc2_anim_probe), RECOMP_OBJ_SPAWNLOG=1.
Edit-Sub 'sub_0004F1A0' 'mvc2_anim_probe(0)' {
  param($c)
  return (Sub1 $c '(void sub_0004F1A0\(void\)\r?\n\{\r?\n(?:[^\r\n]*\r?\n){3,6}?    \(void\)_fa; \(void\)_fb; \(void\)_fas; \(void\)_fbs;\r?\n)' ('$1    { extern void mvc2_anim_probe(int); mvc2_anim_probe(0); }' + "`n") 'anim3')
}
Edit-Sub 'sub_0004F280' 'mvc2_anim_probe(1)' {
  param($c)
  return (Sub1 $c '(void sub_0004F280\(void\)\r?\n\{\r?\n(?:[^\r\n]*\r?\n){3,6}?    \(void\)_fa; \(void\)_fb; \(void\)_fas; \(void\)_fbs;\r?\n)' ('$1    { extern void mvc2_anim_probe(int); mvc2_anim_probe(1); }' + "`n") 'anim4')
}

# P1 (opt-in, -DpcGuard): the cancel DPC (sub_002C6908) must survive "the request already retired".
# The title cancels a synchronous request that is merely LATE (50 ms timeout); the request lives on the
# caller's stack and the same address is queued again, and the walk for its TD runs into the dummy tail TD.
# P0: XpCancelRequest (sub_002C4F12) must not queue a request that has already COMPLETED ([req+0x22]&8).
# Completion unlinks a request from the cancel list only when bit 0 is already set, so a request that completes between the title's 50 ms timeout
# and this call would be pushed on the list after completion; the caller then returns and the node is a dead stack frame.
Edit-Sub 'sub_002C4F12' 'mvc2_cancel_completed' {
  param($c)
  return (Sub1 $c '(loc_002C4F3A: ;\r?\n)(    SET_LO16\(eax, MEM16\(esi \+ 0x22\)\);)' ('$1    extern int mvc2_p0_off(void); { extern void mvc2_cancel_seen(uint32_t, uint32_t); mvc2_cancel_seen(esi, MEM8(esi + 0x22)); }' + "`n" + '    if ((MEM8(esi + 0x22) & 8) && !mvc2_p0_off()) { extern void mvc2_cancel_completed(uint32_t); mvc2_cancel_completed(esi); goto loc_002C4FB4; } /* mvc2: already completed */' + "`n" + '$2') 'p0 completed')
}
if ($DpcGuard) {   # opt-in: P0 alone gave 0 crashes / 80 loaded launches; P1 can touch a stale ED, so it is not default
Edit-Sub 'sub_002C6908' 'mvc2_cancel_notfound' {
  param($c)
  $c = Sub1 $c '(loc_002C6931: ;\r?\n    ecx = MEM32\(ebx \+ 0x42C\);\r?\n    eax = MEM32\(ecx \+ 0x24\);\r?\n)' ('$1    if (eax == ecx) { extern void mvc2_cancel_selflink(uint32_t); mvc2_cancel_selflink(ecx); eax = 0; } /* mvc2: self-linked cancel node */' + "`n") 'a pop'
  $c = Sub1 $c '(    MEM32\(ebp \+ -16\) = eax;\r?\n)(    if \(CMP_EQ\(_fa, _fb\)\) goto loc_002C699B;)' ('$1    if (eax == (MEM32(esi + 4) & 0xFFFFFFF0u)) { extern void mvc2_cancel_notfound(uint32_t,uint32_t); mvc2_cancel_notfound(ecx, esi); goto loc_002C69DB; } /* mvc2: empty ED */' + "`n" + '$2') 'b empty ED'
  $c = Sub1 $c '(loc_002C6998: ;\r?\n)(    ebx = MEM32\(ebp \+ -8\);)' ('$1    if (eax == ebx) { extern void mvc2_cancel_notfound(uint32_t,uint32_t); mvc2_cancel_notfound(ecx, esi); ebx = MEM32(ebp + -8); goto loc_002C69DB; } /* mvc2: walk reached the tail */' + "`n" + '$2') 'c tail'
  return $c
}
}
if ($Hoplat) {   # diagnostic only: publish->guest completion latency probe ([HOPLAT])
Edit-Sub 'sub_002C685C' 'mvc2_done_hook' {
  param($c)
  return (Sub1 $c '(loc_002C685C: ;\r?\n)(    PUSH32\(esp, ebp\);)' ('$1    { extern void mvc2_done_hook(uint32_t); mvc2_done_hook(edx); }' + "`n" + '$2') 'hoplat')
}
}
