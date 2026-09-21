# End-to-end test of the AgentPad channel, without GTA III.
#
#   xmake build padtest
#   powershell -ExecutionPolicy Bypass -File tools\GtaInput\GtaInput.E2E.Tests.ps1
#
# `padtest --observe` stands in for the game: it creates the real named section
# and runs the real Engine::Step in a loop, exactly as the hook does, printing
# what it decided. This script is the real driver. So what is exercised here is
# the whole path: PowerShell writes bytes, the C++ side reads them, and the
# decision engine turns them into a controller state.
#
# The one link this cannot cover is the final memcpy into CPad::Pads,
# CPad::NewKeyState and CPad::NewMouseControllerState. That needs the game's
# address space and is what the first in-game run is for.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Checks = 0
$script:Failed = 0

function Check { param([bool]$Ok, [string]$What)
    $script:Checks++
    if (-not $Ok) { $script:Failed++; Write-Host "  FAIL  $What" -ForegroundColor Red }
}
function CheckEq { param($Got, $Want, [string]$What)
    $script:Checks++
    if ($Got -ne $Want) { $script:Failed++; Write-Host "  FAIL  ${What}: got '$Got', want '$Want'" -ForegroundColor Red }
}
function Section { param([string]$Name) Write-Host "`n$Name" }

# Poll for a condition instead of sleeping a guessed amount. A killed
# process releases its section handles a moment after WaitForExit returns,
# and how long "a moment" is depends on what else the machine is doing,
# which is exactly the shape of flake that makes a suite untrustworthy.
function Wait-Until {
    param([scriptblock]$Condition, [int]$TimeoutMs = 5000, [int]$PollMs = 50)
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds $PollMs
    }
    return [bool](& $Condition)
}

# ---------------------------------------------------------------------------

$padtest = Join-Path $PSScriptRoot '..\..\build\windows\x86\release\padtest.exe'
if (-not (Test-Path $padtest)) {
    Write-Host "padtest.exe not found. Run: xmake build padtest" -ForegroundColor Red
    exit 2
}

Import-Module (Join-Path $PSScriptRoot 'GtaInput.psm1') -Force

Write-Host 'GtaInput.E2E.Tests: PowerShell driver against the real decision engine'

$outFile = Join-Path ([System.IO.Path]::GetTempPath()) "agentpad-e2e-$PID.txt"
$proc = Start-Process -FilePath $padtest -ArgumentList '--observe', '60000' `
    -RedirectStandardOutput $outFile -NoNewWindow -PassThru

try {
    # Wait for the observer to publish the section before opening it. The pid
    # is on the READY line because the section name now carries it, and
    # because GTA III may well be running on this machine too, in which case
    # there are two instances and the driver must be told which.
    $ready  = $false
    $obsPid = 0
    for ($i = 0; $i -lt 100; $i++) {
        Start-Sleep -Milliseconds 50
        if (Test-Path $outFile) {
            $content = Get-Content $outFile -ErrorAction SilentlyContinue
            $joined  = if ($content) { $content -join "`n" } else { '' }
            if ($joined -match 'READY pid=(\d+)') { $obsPid = [int]$Matches[1]; $ready = $true; break }
            if ($joined -match 'ERROR') { break }
        }
    }
    Check $ready 'the observer published the shared section'
    if (-not $ready) { throw 'observer never became ready' }

    CheckEq $obsPid $proc.Id 'the observer announced its own pid'

    # -----------------------------------------------------------------------
    Section 'the instance shows up in the directory, by pid'

    $listed = @(Find-GtaInstancePid)
    Check ($listed -contains $obsPid) 'the observer registered itself in the instance index'

    $inst = @(Get-GtaInstance -GamePid $obsPid)
    CheckEq $inst.Count 1 'Get-GtaInstance finds it by pid'
    Check $inst[0].ModLoaded 'and reports the mod as loaded'
    CheckEq $inst[0].Section "CoopIII.AgentPad.v1.$obsPid" 'under its per-pid section name'

    # Looking must not claim: Get-GtaInstance opens read-only and never writes
    # the driver magic, so it is safe against a game somebody else is driving.
    Check (-not $inst[0].Attached) 'Get-GtaInstance did not attach to the section'

    Check (-not (Test-GtaInstance 0x7FFFFFF0)) 'a pid with no section is not live'

    # -----------------------------------------------------------------------
    Section 'the driver can see the mod, and the mod can see the driver'

    $status = Get-GtaInputStatus -GamePid $obsPid
    Check $status.ModLoaded 'the driver sees the mod magic'
    Check (Wait-Until { (Get-GtaInputStatus -GamePid $obsPid).Frames -gt 0 }) `
        'the driver sees frames advancing'

    # -----------------------------------------------------------------------
    Section 'a menu tap crosses the channel'

    Send-GtaInput -Button Enter -DurationMs 150 -GamePid $obsPid
    Start-Sleep -Milliseconds 150

    $status = Get-GtaInputStatus -GamePid $obsPid
    Check ($status.Applied -gt 0) 'the mod applied at least one injected frame'
    CheckEq $status.SeqSeen $status.Seq 'the mod kept up with the driver sequence'

    # -----------------------------------------------------------------------
    Section 'a walk sets the left stick'

    Set-GtaStick -Stick Left -X 0 -Y -128 -GamePid $obsPid
    Start-Sleep -Milliseconds 150

    # -----------------------------------------------------------------------
    Section 'a camera pan is delivered as a budget'

    Move-GtaCamera -Dx 300 -Dy 0 -StepMax 30 -GamePid $obsPid
    Start-Sleep -Milliseconds 400
    $status = Get-GtaInputStatus -GamePid $obsPid
    CheckEq $status.MouseLeftX 0 'the pan drained to completion'

    # -----------------------------------------------------------------------
    Section 'two instances at once: the thing the per-pid naming is for'

    # A second observer, so everything below is the real multi-instance path:
    # two sections, two index entries, two independent held-button states.
    $outFile2 = Join-Path ([System.IO.Path]::GetTempPath()) "agentpad-e2e2-$PID.txt"
    $obsPid2  = 0
    $proc2 = Start-Process -FilePath $padtest -ArgumentList '--observe', '60000' `
        -RedirectStandardOutput $outFile2 -NoNewWindow -PassThru
    try {
        for ($i = 0; $i -lt 100; $i++) {
            Start-Sleep -Milliseconds 50
            if (Test-Path $outFile2) {
                $c = Get-Content $outFile2 -ErrorAction SilentlyContinue
                $j = if ($c) { $c -join "`n" } else { '' }
                if ($j -match 'READY pid=(\d+)') { $obsPid2 = [int]$Matches[1]; break }
            }
        }
        Check ($obsPid2 -gt 0) 'a second instance started'
        Check ($obsPid2 -ne $obsPid) 'and it is a different process'

        $both = @(Find-GtaInstancePid)
        Check ($both -contains $obsPid)  'the first instance still enumerates'
        Check ($both -contains $obsPid2) 'and so does the second'
        CheckEq (@(Get-GtaInstance).Count) $both.Count 'Get-GtaInstance reports one object each'

        # The whole point: a command with no -GamePid must be an error that
        # names the instances, not a coin flip about which game gets the key.
        try {
            Get-GtaInputStatus | Out-Null
            Check $false 'an ambiguous command must throw'
        } catch {
            Check ($_.Exception.Message -like "*$obsPid*")  'the error lists the first pid'
            Check ($_.Exception.Message -like "*$obsPid2*") 'and the second'
            Check ($_.Exception.Message -like '*-GamePid*') 'and says how to disambiguate'
        }

        # Held state is per instance: a command to one must not touch the
        # other. This is why the held pad state moved out of one module-wide
        # variable and into the per-instance entry.
        $appliedBefore2 = (Get-GtaInputStatus -GamePid $obsPid2).Applied
        Send-GtaInput -Button Enter -DurationMs 120 -GamePid $obsPid2
        Start-Sleep -Milliseconds 200
        Check ((Get-GtaInputStatus -GamePid $obsPid2).Applied -gt $appliedBefore2) `
            'the second instance acted on its own commands'

        # Selecting a default makes the short form target that one.
        Select-GtaInstance -GamePid $obsPid2
        CheckEq (Get-GtaInputStatus).GamePid $obsPid2 'a selected default is what the short form uses'
        Select-GtaInstance -None

        # And one instance going away leaves the other alone.
        Close-GtaInput -GamePid $obsPid2
        $proc2.Kill(); $proc2.WaitForExit(5000) | Out-Null
        Check (Wait-Until { -not (Test-GtaInstance $obsPid2) }) 'the second section goes with its process'
        Check (Test-GtaInstance $obsPid) 'the first one is untouched'
        CheckEq (Get-GtaInputStatus).GamePid $obsPid 'and is unambiguous again'
    } finally {
        if ($proc2 -and -not $proc2.HasExited) { $proc2.Kill(); $proc2.WaitForExit(5000) | Out-Null }
        if ($obsPid2 -gt 0) { try { Close-GtaInput -GamePid $obsPid2 } catch { } }
        Remove-Item $outFile2 -ErrorAction SilentlyContinue
    }

    # -----------------------------------------------------------------------
    Section 'releasing hands control back'

    Reset-GtaInput -GamePid $obsPid
    Start-Sleep -Milliseconds 200
    $status = Get-GtaInputStatus -GamePid $obsPid
    CheckEq $status.Status 'pass-through' 'the mod reports pass-through after a reset'
    Check (-not $status.Enabled) 'injection is off after a reset'

    Close-GtaInput -GamePid $obsPid
    Start-Sleep -Milliseconds 200
    if (-not $proc.HasExited) { $proc.Kill() }
    $proc.WaitForExit(10000) | Out-Null

    # The section dies with the process, and so does the index entry.
    Check (Wait-Until { -not (Test-GtaInstance $obsPid) }) 'the section is gone once the observer exits'
    Check (@(Find-GtaInstancePid) -notcontains $obsPid) 'and it no longer enumerates'

    # -----------------------------------------------------------------------
    Section 'what the observer actually decided'

    $lines  = Get-Content $outFile
    $frames = @($lines | Where-Object { $_ -like 'FRAME *' })
    Check ($frames.Count -ge 4) "the observer logged decision changes (got $($frames.Count))"

    function Parse-Frame { param([string]$Line)
        $o = @{}
        foreach ($tok in ($Line -replace '^FRAME ', '') -split ' ') {
            $kv = $tok -split '=', 2
            if ($kv.Count -eq 2) { $o[$kv[0]] = $kv[1] }
        }
        $o
    }

    $parsed = @($frames | ForEach-Object { Parse-Frame $_ })

    # The Enter tap: status 2 (injecting) with the Enter key bit (16) set and
    # Cross (pad index 16) held at 255. Both routes, as the module promises.
    $contract = Get-GtaInputContract
    $enterBit = $contract.KeyBits['Enter']
    $crossIdx = $contract.PadIndex['Cross']

    $enterFrames = @($parsed | Where-Object {
        $_.ContainsKey('keys') -and (([int]$_['keys'] -band $enterBit) -ne 0)
    })
    Check ($enterFrames.Count -ge 1) 'the Enter key bit reached the mod'

    $crossHeld = @($parsed | Where-Object {
        $_.ContainsKey('pad') -and (([int](($_['pad'] -split ',')[$crossIdx])) -eq 255)
    })
    Check ($crossHeld.Count -ge 1) 'Cross reached the mod at 255, the value the game itself writes'

    # The walk: LeftStickY at -128.
    $walkFrames = @($parsed | Where-Object {
        $_.ContainsKey('pad') -and (([int](($_['pad'] -split ',')[1])) -eq -128)
    })
    Check ($walkFrames.Count -ge 1) 'the forward walk reached the mod as LeftStickY = -128'

    # The pan: a non-zero mouseDx, capped at the step size.
    $panFrames = @($parsed | Where-Object {
        $_.ContainsKey('mouse') -and ((($_['mouse'] -split ',')[0]) -ne '0')
    })
    Check ($panFrames.Count -ge 1) 'the camera pan produced mouse deltas'
    foreach ($p in $panFrames) {
        $dx = [int](($p['mouse'] -split ',')[0])
        Check ([Math]::Abs($dx) -le 30) "a pan step never exceeds the requested cap (saw $dx)"
    }

    # And the final state: released.
    $last = $parsed[-1]
    CheckEq ([int]$last['status']) 1 'the last decision the observer made was pass-through'
    CheckEq ([int]$last['channels']) 0 'and it wrote nothing'

} finally {
    if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }
    try { Close-GtaInput } catch { }
    Remove-Item $outFile -ErrorAction SilentlyContinue
}

Write-Host ''
if ($script:Failed -eq 0) {
    Write-Host "$($script:Checks) checks, 0 failed" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$($script:Checks) checks, $($script:Failed) failed" -ForegroundColor Red
    exit 1
}
