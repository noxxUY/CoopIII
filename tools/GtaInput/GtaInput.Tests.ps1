# Tests for the GtaInput driver module. No Pester, no GTA III, no mod loaded.
#
#   powershell -ExecutionPolicy Bypass -File tools\GtaInput\GtaInput.Tests.ps1
#
# Two things are covered:
#
#   1. Argument handling: every name resolves to the right slot, aliases work,
#      case does not matter, and an unknown name is an error rather than a
#      guess. A typo that silently presses the wrong button is the worst
#      possible failure for a script that is driving a game unattended.
#
#   2. The contract against the C++ side. The offsets in the module are not
#      checked against a copy of themselves; they are checked against what
#      agentpad actually compiled, read out of `padtest --contract`. Change one
#      side and this fails.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Checks = 0
$script:Failed = 0

function Check {
    param([bool]$Ok, [string]$What)
    $script:Checks++
    if (-not $Ok) {
        $script:Failed++
        Write-Host "  FAIL  $What" -ForegroundColor Red
    }
}

function CheckEq {
    param($Got, $Want, [string]$What)
    $script:Checks++
    if ($Got -ne $Want) {
        $script:Failed++
        Write-Host "  FAIL  ${What}: got '$Got', want '$Want'" -ForegroundColor Red
    }
}

function CheckThrows {
    param([scriptblock]$Block, [string]$What)
    $script:Checks++
    try {
        & $Block | Out-Null
        $script:Failed++
        Write-Host "  FAIL  ${What}: expected an error, got none" -ForegroundColor Red
    } catch {
        # Expected.
    }
}

function Section { param([string]$Name) Write-Host "`n$Name" }

# ---------------------------------------------------------------------------

$modulePath = Join-Path $PSScriptRoot 'GtaInput.psm1'
Import-Module $modulePath -Force

Write-Host 'GtaInput.Tests: driver argument handling and the C++ contract'

# ---------------------------------------------------------------------------
Section 'menu actions: each drives both routes the frontend reads'

$contract = Get-GtaInputContract

# CMenuManager checks the keyboard state and the pad state, and which branch a
# given build takes is not something we can prove from outside the game. So a
# menu action drives both: `Up` is `GetUp() || GetDPadUpJustDown()`, `Enter` is
# `GetEnterJustDown() || GetCrossJustDown()`.
$enter = Resolve-GtaInputName -Name 'Enter'
CheckEq $enter.KeyMask $contract.KeyBits['Enter'] 'Enter sets the Enter key bit'
CheckEq ($enter.Pad -join ',') 'Cross' 'Enter also presses Cross on the pad'

$escape = Resolve-GtaInputName -Name 'Escape'
CheckEq $escape.KeyMask $contract.KeyBits['Escape'] 'Escape sets the Escape key bit'
CheckEq ($escape.Pad -join ',') 'Triangle' 'Escape also presses Triangle'

$up = Resolve-GtaInputName -Name 'Up'
CheckEq $up.KeyMask $contract.KeyBits['Up'] 'Up sets the Up key bit'
CheckEq ($up.Pad -join ',') 'DPadUp' 'Up also presses DPadUp'

$down = Resolve-GtaInputName -Name 'Down'
CheckEq $down.KeyMask $contract.KeyBits['Down'] 'Down sets the Down key bit'
CheckEq ($down.Pad -join ',') 'DPadDown' 'Down also presses DPadDown'

$left = Resolve-GtaInputName -Name 'Left'
CheckEq ($left.Pad -join ',') 'DPadLeft' 'Left presses DPadLeft'
$right = Resolve-GtaInputName -Name 'Right'
CheckEq ($right.Pad -join ',') 'DPadRight' 'Right presses DPadRight'

# ---------------------------------------------------------------------------
Section 'aliases: the spellings people actually type'

CheckEq (Resolve-GtaInputName -Name 'Return').KeyMask $contract.KeyBits['Enter'] 'Return aliases Enter'
CheckEq (Resolve-GtaInputName -Name 'Accept').KeyMask $contract.KeyBits['Enter'] 'Accept aliases Enter'
CheckEq (Resolve-GtaInputName -Name 'Esc').KeyMask    $contract.KeyBits['Escape'] 'Esc aliases Escape'
CheckEq (Resolve-GtaInputName -Name 'Back').KeyMask   $contract.KeyBits['Escape'] 'Back aliases Escape'

CheckEq ((Resolve-GtaInputName -Name 'L1').Pad -join ',') 'LeftShoulder1'  'L1 aliases LeftShoulder1'
CheckEq ((Resolve-GtaInputName -Name 'L2').Pad -join ',') 'LeftShoulder2'  'L2 aliases LeftShoulder2'
CheckEq ((Resolve-GtaInputName -Name 'R1').Pad -join ',') 'RightShoulder1' 'R1 aliases RightShoulder1'
CheckEq ((Resolve-GtaInputName -Name 'R2').Pad -join ',') 'RightShoulder2' 'R2 aliases RightShoulder2'

# ---------------------------------------------------------------------------
Section 'case insensitivity'

CheckEq (Resolve-GtaInputName -Name 'enter').KeyMask $contract.KeyBits['Enter'] 'lowercase enter resolves'
CheckEq (Resolve-GtaInputName -Name 'ENTER').KeyMask $contract.KeyBits['Enter'] 'uppercase ENTER resolves'
CheckEq ((Resolve-GtaInputName -Name 'cross').Pad -join ',') 'Cross' 'lowercase cross resolves'
CheckEq ((Resolve-GtaInputName -Name 'dpadup').Pad -join ',') 'DPadUp' 'lowercase dpadup resolves'
CheckEq (Resolve-GtaInputName -Name 'pageup').KeyMask $contract.KeyBits['PageUp'] 'lowercase pageup resolves'

# Whitespace is trimmed, because it comes free with pipelines and files.
CheckEq (Resolve-GtaInputName -Name '  Enter  ').KeyMask $contract.KeyBits['Enter'] 'surrounding whitespace is trimmed'

# ---------------------------------------------------------------------------
Section 'raw pad buttons and raw keys'

CheckEq ((Resolve-GtaInputName -Name 'Cross').Pad -join ',')    'Cross'    'Cross resolves to the pad'
CheckEq (Resolve-GtaInputName -Name 'Cross').KeyMask 0 'Cross touches no key'
CheckEq ((Resolve-GtaInputName -Name 'Triangle').Pad -join ',') 'Triangle' 'Triangle resolves to the pad'
CheckEq ((Resolve-GtaInputName -Name 'Start').Pad -join ',')    'Start'    'Start resolves to the pad'

CheckEq (Resolve-GtaInputName -Name 'Tab').KeyMask $contract.KeyBits['Tab'] 'Tab resolves to a key'
CheckEq ((Resolve-GtaInputName -Name 'Tab').Pad.Count) 0 'Tab touches no pad field'
CheckEq (Resolve-GtaInputName -Name 'Space').KeyMask $contract.KeyBits['Space'] 'Space resolves to a key'
CheckEq (Resolve-GtaInputName -Name 'Delete').KeyMask $contract.KeyBits['Delete'] 'Delete resolves to a key'

# ---------------------------------------------------------------------------
Section 'unknown names are errors, never guesses'

CheckThrows { Resolve-GtaInputName -Name 'Banana' }   'an unknown name throws'
CheckThrows { Resolve-GtaInputName -Name '' }         'an empty name throws'
CheckThrows { Resolve-GtaInputName -Name '   ' }      'a whitespace-only name throws'
CheckThrows { Resolve-GtaInputName -Name $null }      'a null name throws'
CheckThrows { Resolve-GtaInputName -Name 'Ente' }     'a truncated name throws rather than matching a prefix'
CheckThrows { Resolve-GtaInputName -Name 'Enter2' }   'a name with a suffix throws'

# The error has to say what IS valid, or an unattended script leaves a log
# nobody can act on.
try {
    Resolve-GtaInputName -Name 'Banana' | Out-Null
} catch {
    Check ($_.Exception.Message -like '*Banana*') 'the error names the offending button'
    Check ($_.Exception.Message -like '*Enter*')  'the error lists the valid names'
}

# ---------------------------------------------------------------------------
Section 'a bad name fails before the section is touched'

# Send-GtaInput and Set-GtaInput resolve the name before opening the shared
# memory. With no mod loaded, both must fail with the *name* error, not with
# "cannot open shared memory", otherwise a typo would be indistinguishable
# from the game not running.
$sawNameError = $false
try { Send-GtaInput -Button 'Banana' } catch { $sawNameError = $_.Exception.Message -like "*Unknown button*" }
Check $sawNameError 'Send-GtaInput rejects a bad name before opening the section'

$sawNameError = $false
try { Set-GtaInput -Button 'Banana' -State Down } catch { $sawNameError = $_.Exception.Message -like "*Unknown button*" }
Check $sawNameError 'Set-GtaInput rejects a bad name before opening the section'

# ---------------------------------------------------------------------------
Section 'parameter validation'

CheckThrows { Send-GtaInput -Button 'Enter' -DurationMs 0 }       'DurationMs 0 is rejected'
CheckThrows { Send-GtaInput -Button 'Enter' -DurationMs -5 }      'a negative DurationMs is rejected'
CheckThrows { Send-GtaInput -Button 'Enter' -Repeat 0 }           'Repeat 0 is rejected'
CheckThrows { Send-GtaInput -Button 'Enter' -Repeat 100000 }      'an absurd Repeat is rejected'
CheckThrows { Set-GtaInput -Button 'Cross' -State 'Sideways' }    'an invalid State is rejected'
CheckThrows { Set-GtaStick -Stick 'Middle' -X 0 -Y 0 }            'an invalid stick is rejected'
CheckThrows { Set-GtaStick -Stick 'Left' -X 99999 }               'an out-of-range axis is rejected'
CheckThrows { Set-GtaStick -Stick 'Left' -Y -99999 }              'an out-of-range negative axis is rejected'
CheckThrows { Start-GtaWalk -Direction 'Upwards' }                'an invalid walk direction is rejected'
CheckThrows { Move-GtaCamera -StepMax -1 }                        'a negative StepMax is rejected'

# The valid sets are not empty and contain what the docs promise.
$names = Get-GtaInputNames
Check ($names -contains 'Enter')  'Get-GtaInputNames lists Enter'
Check ($names -contains 'Escape') 'Get-GtaInputNames lists Escape'
Check ($names -contains 'Cross')  'Get-GtaInputNames lists Cross'
Check ($names -contains 'L1')     'Get-GtaInputNames lists L1'
Check ($names -contains 'Space')  'Get-GtaInputNames lists Space'
Check ($names.Count -eq ($names | Select-Object -Unique).Count) 'Get-GtaInputNames has no duplicates'

# Every advertised name must actually resolve. A name in the help that throws
# is worse than one that is missing.
foreach ($n in $names) {
    $script:Checks++
    try { Resolve-GtaInputName -Name $n | Out-Null } catch {
        $script:Failed++
        Write-Host "  FAIL  advertised name '$n' does not resolve" -ForegroundColor Red
    }
}

# ---------------------------------------------------------------------------
Section 'bit and index tables are internally consistent'

# No two keys share a bit, or one name would press two things.
$seen = 0
foreach ($k in $contract.KeyBits.Keys) {
    $bit = $contract.KeyBits[$k]
    Check ($bit -ne 0) "key bit for $k is non-zero"
    Check (($seen -band $bit) -eq 0) "key bit for $k does not collide"
    $seen = $seen -bor $bit
}

# The 21 controller indices are exactly 0..20, each used once.
$indices = @($contract.PadIndex.Values) | Sort-Object
CheckEq $indices.Count 21 'there are 21 controller fields'
for ($i = 0; $i -lt 21; $i++) { CheckEq $indices[$i] $i "controller index $i is present exactly once" }

# Every alias points at a real field.
foreach ($a in $contract.PadAlias.Keys) {
    Check ($contract.PadIndex.Contains($contract.PadAlias[$a])) "alias $a points at a real field"
}

# Movement magnitudes. Not 32767: CPlayerPed::PlayerControlZelda divides the
# stick magnitude by 60 for world speed, and the keyboard path produces
# (DPadDown - DPadUp) / 2 = 127, so 128 is "what the forward key does".
CheckEq $contract.MoveRun 128 'the run magnitude matches what the keyboard produces'
CheckEq $contract.MoveWalk 60 'the walk magnitude is 1.0 world unit'
CheckEq $contract.ButtonDown 255 "a pressed button is 255, as the game's own input layer writes"

# ---------------------------------------------------------------------------
Section 'contract against the compiled C++ side'

$padtest = Join-Path $PSScriptRoot '..\..\build\windows\x86\release\padtest.exe'
if (-not (Test-Path $padtest)) {
    Write-Host "  SKIP  padtest.exe not built, run: xmake build padtest" -ForegroundColor Yellow
} else {
    $lines = & $padtest --contract
    $cpp = @{}
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') { $cpp[$Matches[1]] = $Matches[2] }
    }

    Check ($cpp.Count -gt 40) 'padtest --contract produced a contract'

    CheckEq $contract.ShmPrefix    $cpp['ShmPrefix']    'the section name prefix matches the compiled mod'
    CheckEq $contract.ShmIndexName $cpp['ShmIndexName'] 'the instance index name matches'
    CheckEq $contract.ShmSize ([int]$cpp['ShmSize']) 'the section size matches'
    CheckEq $contract.IndexSize  ([int]$cpp['IndexSize'])  'the index size matches'
    CheckEq $contract.IndexSlots ([int]$cpp['IndexSlots']) 'the number of index slots matches'
    CheckEq $contract.Magic   ([int64]$cpp['Magic'])   'the magic matches'
    CheckEq $contract.Version ([int]$cpp['Version'])   'the protocol version matches'

    foreach ($o in $contract.IndexOffsets.PSBase.Keys) {
        $key = "IndexOffset.$o"
        $script:Checks++
        if (-not $cpp.ContainsKey($key)) {
            $script:Failed++
            Write-Host "  FAIL  the C++ side has no index offset for '$o'" -ForegroundColor Red
        } elseif ([int]$cpp[$key] -ne $contract.IndexOffsets[$o]) {
            $script:Failed++
            Write-Host ("  FAIL  index offset '{0}': driver says 0x{1:X2}, mod says 0x{2:X2}" -f `
                $o, $contract.IndexOffsets[$o], [int]$cpp[$key]) -ForegroundColor Red
        }
    }

    # The section name the driver would build for a pid has to be the one the
    # mod actually creates. This is the whole multi-instance contract: get it
    # wrong by one character and every command silently talks to nothing.
    CheckEq (Get-GtaSectionName 4312) "$($cpp['ShmPrefix']).4312" 'the per-pid section name matches the mod'

    foreach ($o in $contract.Offsets.PSBase.Keys) {
        $key = "Offset.$o"
        $script:Checks++
        if (-not $cpp.ContainsKey($key)) {
            $script:Failed++
            Write-Host "  FAIL  the C++ side has no offset for '$o'" -ForegroundColor Red
        } elseif ([int]$cpp[$key] -ne $contract.Offsets[$o]) {
            $script:Failed++
            Write-Host ("  FAIL  offset '{0}': driver says 0x{1:X2}, mod says 0x{2:X2}" -f `
                $o, $contract.Offsets[$o], [int]$cpp[$key]) -ForegroundColor Red
        }
    }

    CheckEq $contract.Channels['Pad']   ([int]$cpp['Channel.Pad'])   'the pad channel bit matches'
    CheckEq $contract.Channels['Keys']  ([int]$cpp['Channel.Keys'])  'the keys channel bit matches'
    CheckEq $contract.Channels['Mouse'] ([int]$cpp['Channel.Mouse']) 'the mouse channel bit matches'
    CheckEq $contract.ChannelAll        ([int]$cpp['ChannelAll'])    'the all-channels mask matches'

    foreach ($k in $contract.KeyBits.Keys) {
        $key = "Key.$k"
        $script:Checks++
        if (-not $cpp.ContainsKey($key)) {
            $script:Failed++
            Write-Host "  FAIL  the C++ side does not know the key '$k'" -ForegroundColor Red
        } elseif ([int]$cpp[$key] -ne $contract.KeyBits[$k]) {
            $script:Failed++
            Write-Host "  FAIL  key '$k': driver says $($contract.KeyBits[$k]), mod says $($cpp[$key])" -ForegroundColor Red
        }
    }

    foreach ($p in $contract.PadIndex.Keys) {
        $key = "Pad.$p"
        $script:Checks++
        if (-not $cpp.ContainsKey($key)) {
            $script:Failed++
            Write-Host "  FAIL  the C++ side does not know the controller field '$p'" -ForegroundColor Red
        } elseif ([int]$cpp[$key] -ne $contract.PadIndex[$p]) {
            $script:Failed++
            Write-Host "  FAIL  field '$p': driver says $($contract.PadIndex[$p]), mod says $($cpp[$key])" -ForegroundColor Red
        }
    }

    CheckEq $contract.ButtonDown ([int]$cpp['ButtonDown']) 'the pressed-button value matches'
}

# ---------------------------------------------------------------------------
Section 'instance naming and selection'

# One name per game process. The fixed name was the bug: two copies of the game
# collided on it, and padtest (which creates the same name to test the
# protocol) used to attach to a live game's section and write a button into it.
CheckEq (Get-GtaSectionName 1)    'CoopIII.AgentPad.v1.1'    'a section name is prefix.pid'
CheckEq (Get-GtaSectionName 4312) 'CoopIII.AgentPad.v1.4312' 'and the pid is decimal'
Check ((Get-GtaSectionName 12) -ne (Get-GtaSectionName 123)) 'different pids get different names'

# A pid nothing is publishing must read as absent, not as an error: that is how
# a stale entry in the instance index is recognised.
Check (-not (Test-GtaInstance 0x7FFFFFF0)) 'a pid with no section is not a live instance'

# Enumeration must never throw when nothing is running, or a script cannot ask
# "is anything up?" without wrapping it.
$instances = @(Get-GtaInstance)
Check ($instances.Count -ge 0) 'Get-GtaInstance returns without throwing when nothing is running'
foreach ($i in $instances) {
    Check ($i.GamePid -gt 0) 'every reported instance has a pid'
    Check ($null -ne $i.GameState) 'and a game state'
}

# Every command that can target an instance takes -GamePid, and -Pid as the
# alias a caller actually types. $PID is a PowerShell automatic variable, which
# is why the real parameter is not called Pid.
foreach ($cmd in @('Send-GtaInput', 'Set-GtaInput', 'Set-GtaStick', 'Start-GtaWalk',
                   'Stop-GtaWalk', 'Move-GtaCamera', 'Reset-GtaInput', 'Enable-GtaInput',
                   'Disable-GtaInput', 'Get-GtaInputStatus', 'Wait-GtaGameState',
                   'Get-GtaInputView', 'Close-GtaInput', 'Write-GtaInputState')) {
    $params = (Get-Command $cmd).Parameters
    Check ($params.ContainsKey('GamePid')) "$cmd takes -GamePid"
    Check ($params['GamePid'].Aliases -contains 'Pid') "$cmd accepts -Pid as an alias"
}

CheckThrows { Select-GtaInstance -GamePid 0x7FFFFFF0 } 'selecting a pid with no section throws'
Select-GtaInstance -None   # must not throw even with nothing selected

# ---------------------------------------------------------------------------
Section 'with no mod loaded, the failure says so'

# Every command that needs the section must fail with a message that points at
# the actual cause. This runs with no game, which is the state the tests always
# run in, so it is the one failure mode that is always reachable.
$running = @(Find-GtaInstancePid)
if ($running.Count -eq 1) {
    Write-Host '  NOTE  one instance is running. Is GTA III up with AgentPad.asi loaded?' -ForegroundColor Yellow
} elseif ($running.Count -gt 1) {
    # With two or more up, leaving -GamePid off must be an error that lists
    # them, not a coin flip about which game gets the keystroke.
    try {
        Get-GtaInputStatus | Out-Null
        Check $false 'ambiguous instance selection must throw'
    } catch {
        Check ($_.Exception.Message -like '*-GamePid*') 'the error says how to disambiguate'
        Check ($_.Exception.Message -like '*Get-GtaInstance*') 'and how to list the instances'
    }
} else {
    try {
        Get-GtaInputStatus | Out-Null
        Check $false 'with nothing running, asking for status must throw'
    } catch {
        Check ($_.Exception.Message -like '*AgentPad*') 'the error names the mod that should be loaded'
        Check ($_.Exception.Message -like '*GTA III*') 'and the thing that should be running'
    }
}

# ---------------------------------------------------------------------------

Write-Host ''
if ($script:Failed -eq 0) {
    Write-Host "$($script:Checks) checks, 0 failed" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$($script:Checks) checks, $($script:Failed) failed" -ForegroundColor Red
    exit 1
}
