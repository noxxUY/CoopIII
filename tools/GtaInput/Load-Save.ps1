# Drives the frontend from the title screen to a loaded save.
#
#   .\Load-Save.ps1 -Slot 1
#
# The menu is a fixed tree, so the route never changes: the only variable is
# which slot to take. Worked out by driving it once and watching; recorded here
# so nobody has to work it out again.
#
#   START GAME  ->  LOAD GAME  ->  slot N  ->  YES
#
# Every step starts from a known highlight, which is why this is a sequence of
# taps and not a search: GTA III always opens a submenu with the first item
# selected.

[CmdletBinding()]
param(
    # 1-8. Slot 1 is the top entry under "Cancel".
    [ValidateRange(1, 8)]
    [int] $Slot = 1,

    # How long to let each menu transition settle. The frontend animates, and
    # a tap that lands mid-animation is swallowed.
    [int] $SettleMs = 700,

    # The load itself streams the world in; this is how long to wait for it.
    [int] $LoadTimeoutMs = 240000,

    # Which game, when more than one copy is running. Leave it out for the
    # usual single-instance case; run Get-GtaInstance to see what is up.
    [Alias('Pid')]
    [int] $GamePid = 0
)

$ErrorActionPreference = "Stop"
Import-Module (Join-Path $PSScriptRoot "GtaInput.psm1") -Force

# Resolve once, so every step below goes to the same game even if another
# instance starts or stops halfway through.
$GamePid = Resolve-GtaInstancePid -GamePid $GamePid

function Step {
    param([string] $Button, [string] $What)
    Write-Host ("  {0,-7} {1}" -f $Button, $What)
    Send-GtaInput -Button $Button -GamePid $GamePid
    Start-Sleep -Milliseconds $SettleMs
}

$status = Get-GtaInputStatus -GamePid $GamePid
if (-not $status.ModLoaded) {
    throw "AgentPad is not loaded. Start the game with the launcher first."
}
if ($status.GameState -ne "frontend") {
    throw "The game is in state '$($status.GameState)', not 'frontend'. This script starts from the title screen."
}

Write-Host "Loading slot $Slot in pid $GamePid"
Enable-GtaInput -GamePid $GamePid
try {
    Step Enter "main menu: START GAME"
    Step Down  "submenu: LOAD GAME"
    Step Enter "open the slot list"

    # The list opens on "Cancel"; slot N is N steps below it.
    for ($i = 0; $i -lt $Slot; $i++) {
        Step Down ("slot list: down to slot {0}" -f ($i + 1))
    }
    Step Enter "pick the slot"

    Step Down  "confirm: NO -> YES"
    Step Enter "confirm"

    Write-Host "  waiting for the world to stream in..."
    Wait-GtaGameState -State playing -TimeoutMs $LoadTimeoutMs -GamePid $GamePid | Out-Null
}
finally {
    # Always hand the keyboard back, even if a step threw: a stuck injected
    # key would be worse than a failed load.
    Reset-GtaInput -GamePid $GamePid
}

Write-Host "In game."
Get-GtaInputStatus -GamePid $GamePid
