# Lightning Returns builder - patches applied to the recompiled code after every codegen. Both
# are idempotent. Dot-sourced by pipeline.ps1.

# lightningreturns_pch.h: ReXGlue's guest "interrupts off" macros (REX_CHECK/ENTER/LEAVE_GLOBAL_LOCK,
# one process-wide mutex) -> project\lr_guest_irq.inc (per-thread state; fixes the movie /
# loading / title freezes and most of the lock contention). Also swaps the flush-mode macros.
function Update-PchIrqLock([string]$pch, [string]$inc) {
  $text = [IO.File]::ReadAllText($pch)
  if ($text.Contains('LR_GUEST_IRQ_LOCK v7')) { return 'already patched' }
  if ($text.Contains('LR_GUEST_IRQ_LOCK') -or $text.Contains('LR_FAIR_GLOBAL_LOCK')) {
    throw "$pch carries an older interrupt-lock patch; delete the generated folder so codegen writes it again"
  }
  $start = $text.IndexOf('#define REX_CHECK_GLOBAL_LOCK()')
  if ($start -lt 0) { throw "REX_CHECK_GLOBAL_LOCK not found in $pch (another ReXGlue version?)" }
  $m = [regex]::new('(?s)#define REX_LEAVE_GLOBAL_LOCK\(\).*?\} while \(0\)\r?\n').Match($text, $start)
  if (-not $m.Success) { throw "REX_LEAVE_GLOBAL_LOCK not found in $pch" }
  $text = $text.Substring(0, $start) + [IO.File]::ReadAllText($inc) + $text.Substring($m.Index + $m.Length)
  [IO.File]::WriteAllText($pch, $text)
  $check = [IO.File]::ReadAllText($pch)
  if (-not $check.Contains('#define enableFlushMode disableFlushMode') -or $check.Contains('#define disableFlushMode enableFlushMode')) {
    throw "the interrupt-lock patch did not stick in $pch"
  }
  return 'patched'
}

# The recompiled functions: a memory fence after each guest lwsync / eieio / sync (codegen
# leaves only a comment). Required on ARM (Android): without them game threads can see a flag
# before the data it publishes and wait forever (a freeze loading a save). On x86 they compile
# to nothing more than compiler barriers.
function Add-MemoryBarriers([string]$generated) {
  $fence = @{
    'lwsync' = 'std::atomic_thread_fence(std::memory_order_acq_rel);'
    'eieio'  = 'std::atomic_thread_fence(std::memory_order_acq_rel);'
    'sync'   = 'std::atomic_thread_fence(std::memory_order_seq_cst);'
  }
  $re = [regex]::new('(?m)^(\t// (lwsync|eieio|sync) ?\n)(?!\tstd::atomic_thread_fence)')
  $total = 0; $files = 0
  foreach ($f in Get-ChildItem -LiteralPath $generated -Filter '*.cpp' -File | Sort-Object Name) {
    $s = [IO.File]::ReadAllText($f.FullName)
    $n = $re.Matches($s).Count
    $t = $re.Replace($s, { param($m) $m.Groups[1].Value + "`t" + $fence[$m.Groups[2].Value] + "`n" })
    if ($n) { [IO.File]::WriteAllText($f.FullName, $t); $total += $n; $files++ }
  }
  return "$total memory barriers in $files files"
}
