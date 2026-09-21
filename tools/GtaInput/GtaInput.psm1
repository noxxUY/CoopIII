# GtaInput: drive GTA III's input from PowerShell.
#
# Talks to AgentPad.asi through the named shared-memory section it publishes.
# The mod hooks CPad::UpdatePads and, after the game has composed its own input
# for the frame, overwrites it with whatever this module has written.
#
#   Import-Module .\tools\GtaInput\GtaInput.psm1
#   Send-GtaInput -Button Enter
#
# Nothing here launches or modifies the game. It only writes to a section the
# already-running game has published; if the mod is not loaded, every command
# fails with a clear message instead of doing something surprising.
#
# ---------------------------------------------------------------------------
# More than one game at a time
# ---------------------------------------------------------------------------
#
# Every command takes an optional -GamePid (aliased -Pid). Leave it out and the
# module picks the instance for you, which is the common case and stays a
# one-liner:
#
#   Send-GtaInput -Button Enter          # one game running: unambiguous
#
# With two or three games up, say which:
#
#   Get-GtaInstance                      # pid, state, window title, per game
#   Send-GtaInput -Button Enter -GamePid 4312
#   Select-GtaInstance -GamePid 4312     # or set a default for the session
#   Send-GtaInput -Button Enter          # ...and go back to the short form
#
# It is deliberately an error, not a guess, to leave -GamePid off while two
# instances are running: pressing Enter into whichever game happened to sort
# first is exactly the kind of thing that wastes an afternoon.
#
# -GamePid rather than -Pid as the real parameter name because $PID is a
# PowerShell automatic variable holding *this* shell's process id, and a
# parameter called Pid shadows it inside every function here. -Pid still works
# as an alias, which is what a caller types.
#
# The layout below is the contract in agentpad/src/protocol.h. Both sides are
# tested against it: padtest asserts the C++ offsets, GtaInput.Tests.ps1
# asserts these.

Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# Contract
# ---------------------------------------------------------------------------

# The section name is per game process: this prefix, a dot, and the pid. It
# used to be a single fixed name, which meant two copies of the game shared one
# section and neither could be addressed on its own.
$script:ShmPrefix    = 'CoopIII.AgentPad.v1'
$script:ShmIndexName = 'CoopIII.AgentPad.v1.index'
$script:ShmSize      = 0x80
$script:IndexSize    = 0x40
$script:IndexSlots   = 8
$script:Magic        = 0x44415041   # 'APAD'
$script:Version      = 1

# Byte offsets into the section. Mirrors struct agentpad::Shared.
$script:Off = @{
    Magic         = 0x00
    Version       = 0x04
    Seq           = 0x08
    Enabled       = 0x0C
    Channels      = 0x10
    WriterTickMs  = 0x14
    TimeoutMs     = 0x18
    KeyMask       = 0x1C
    MouseBudgetX  = 0x20
    MouseBudgetY  = 0x24
    MouseSeq      = 0x28
    MouseStepMax  = 0x2C
    ModMagic      = 0x30
    ModPid        = 0x34
    ModFrames     = 0x38
    ModApplied    = 0x3C
    ModSeqSeen    = 0x40
    ModStatus     = 0x44
    ModMouseLeftX = 0x48
    ModMouseLeftY = 0x4C
    Pad           = 0x50
    ModGameState  = 0x7C
}

# Byte offsets into the instance directory. Mirrors struct agentpad::Index.
$script:IndexOff = @{
    Magic    = 0x00
    Version  = 0x04
    Capacity = 0x08
    Pid      = 0x10
}

$script:Channel = @{ Pad = 1; Keys = 2; Mouse = 4 }
$script:ChannelAll = 7

# Named keys -> bit in the `keys` mask. Mirrors agentpad::Key.
$script:KeyBits = [ordered]@{
    Up = 1; Down = 2; Left = 4; Right = 8
    Enter = 16; Escape = 32; Tab = 64; Backspace = 128
    Space = 256; PageUp = 512; PageDown = 1024; Home = 2048
    End = 4096; Delete = 8192; Insert = 16384; Shift = 32768
}

# Controller field -> index into pad[21]. Mirrors agentpad::PadIndex, which is
# re3's CControllerState declaration order.
$script:PadIndex = [ordered]@{
    LeftStickX = 0; LeftStickY = 1; RightStickX = 2; RightStickY = 3
    LeftShoulder1 = 4; LeftShoulder2 = 5; RightShoulder1 = 6; RightShoulder2 = 7
    DPadUp = 8; DPadDown = 9; DPadLeft = 10; DPadRight = 11
    Start = 12; Select = 13
    Square = 14; Triangle = 15; Cross = 16; Circle = 17
    LeftShock = 18; RightShock = 19; NetworkTalk = 20
}

# Shorthand nobody will resist using.
$script:PadAlias = @{ L1 = 'LeftShoulder1'; L2 = 'LeftShoulder2'; R1 = 'RightShoulder1'; R2 = 'RightShoulder2' }

$script:StatusName = @{ 0 = 'unknown'; 1 = 'pass-through'; 2 = 'injecting'; 3 = 'stale'; 4 = 'no driver' }

# gGameState (0x008F5838), the variable WinMain switches on. A script needs
# this to know whether to send menu keys or gameplay input. 'frontend' is the
# main menu; 'playing' is in the world. 'logo-movie' and 'intro-movie' are the
# two startup movies, which AgentPad will skip for you when SkipIntro is on.
$script:GameStateName = @{
    0 = 'startup'; 1 = 'init-logo'; 2 = 'logo-movie'; 3 = 'init-intro'; 4 = 'intro-movie'
    5 = 'init-once'; 6 = 'init-frontend'; 7 = 'frontend'; 8 = 'init-playing'; 9 = 'playing'
}

# What the game's own PC input layer writes for a pressed digital button
# (re3 ControllerConfig.cpp sets PCTempJoyState members to 255).
$script:ButtonDown = 255

# Full deflection for the walk sticks. NOT 32767: CPlayerPed::PlayerControlZelda
# divides the stick magnitude by 60 to get world speed, and the keyboard path
# feeds it (DPadDown - DPadUp) / 2 = 127. So 128 is "what holding the forward
# key does", and anything much larger asks the game for a speed it never
# normally sees.
$script:MoveRun  = 128
$script:MoveWalk = 60

# The executable to probe when the instance index is missing: an older mod, or
# a game that failed to register. Not authoritative; the index is.
$script:GameProcessName = 'gta3'

# ---------------------------------------------------------------------------
# Instances
# ---------------------------------------------------------------------------

# pid -> @{ Mmf; View; PadState; KeyState }. One entry per game this session has
# talked to. The held button state lives here rather than in one module-wide
# variable because two games hold different buttons, and a walk in one of them
# must not be cancelled by a tap in the other.
$script:Instances = @{}

# Set by Select-GtaInstance. 0 means "work it out".
$script:DefaultPid = 0

function Get-GtaSectionName {
    <#
    .SYNOPSIS
    The shared-memory section name for a given game process.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory, Position = 0)][int]$GamePid)
    "$($script:ShmPrefix).$GamePid"
}

function Test-GtaInstance {
    <#
    .SYNOPSIS
    True if a process with this pid is publishing an AgentPad section.

    .DESCRIPTION
    This is how a stale entry in the instance index is told from a live one:
    the section dies with its last handle, so a game that crashed leaves a pid
    in the table whose section can no longer be opened.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory, Position = 0)][int]$GamePid)

    if ($script:Instances.ContainsKey($GamePid)) { return $true }
    try {
        $mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
            (Get-GtaSectionName $GamePid),
            [System.IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
        $mmf.Dispose()
        return $true
    } catch {
        return $false
    }
}

function Get-GtaIndexedPid {
    <#
    .SYNOPSIS
    Every pid listed in the instance directory, stale entries included.
    #>
    [CmdletBinding()]
    param()

    $found = New-Object System.Collections.Generic.List[int]
    $mmf = $null
    try {
        $mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
            $script:ShmIndexName,
            [System.IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
    } catch {
        # No index at all: nothing has published one this session.
        return $found.ToArray()
    }

    try {
        $v = $mmf.CreateViewAccessor(0, $script:IndexSize,
            [System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
        try {
            # Trust the table's own capacity over ours, but never past the end
            # of the section we mapped.
            $cap = [int]$v.ReadUInt32([int64]$script:IndexOff.Capacity)
            if ($cap -le 0 -or $cap -gt $script:IndexSlots) { $cap = $script:IndexSlots }
            for ($i = 0; $i -lt $cap; $i++) {
                $p = $v.ReadUInt32([int64]($script:IndexOff.Pid + $i * 4))
                if ($p -ne 0) { $found.Add([int]$p) }
            }
        } finally { $v.Dispose() }
    } finally { $mmf.Dispose() }

    # Emitted as elements, not as one array object: every caller wraps in @(),
    # which is correct for none, one and many. (The `, $array` idiom would make
    # the two-instance case arrive as a nested array, and the conversion error
    # that produces is a long way from its cause.)
    $found.ToArray()
}

function Find-GtaInstancePid {
    <#
    .SYNOPSIS
    Every game process currently publishing a section, confirmed.

    .DESCRIPTION
    The instance directory first, because it does not depend on what the
    executable is called. Anything it lists is then confirmed by opening that
    pid's section, which is what filters out a game that crashed without
    clearing its slot.

    Falls back to probing gta3.exe processes if the directory is missing, so an
    older AgentPad, or one that could not register, is still reachable.
    #>
    [CmdletBinding()]
    param()

    $live = New-Object System.Collections.Generic.List[int]
    foreach ($p in @(Get-GtaIndexedPid)) {
        if ((Test-GtaInstance $p) -and -not $live.Contains($p)) { $live.Add($p) }
    }

    if ($live.Count -eq 0) {
        foreach ($proc in @(Get-Process -Name $script:GameProcessName -ErrorAction SilentlyContinue)) {
            if ((Test-GtaInstance $proc.Id) -and -not $live.Contains($proc.Id)) { $live.Add($proc.Id) }
        }
    }

    # Anything this session already has open counts, even if it is neither
    # listed nor named gta3, like padtest --observe.
    foreach ($p in @($script:Instances.Keys)) {
        if (-not $live.Contains([int]$p)) { $live.Add([int]$p) }
    }

    $live.ToArray() | Sort-Object
}

function Resolve-GtaInstancePid {
    <#
    .SYNOPSIS
    Turns an optional -GamePid into the instance a command should act on.

    .DESCRIPTION
    Explicit pid, then the session default from Select-GtaInstance, then "the
    only one running". With more than one running and nothing chosen, this
    throws and lists them rather than picking: an unattended script pressing
    Enter into the wrong game is a failure that looks like a working run.
    #>
    [CmdletBinding()]
    param([int]$GamePid = 0)

    if ($GamePid -gt 0) { return $GamePid }
    if ($script:DefaultPid -gt 0) { return $script:DefaultPid }

    $found = @(Find-GtaInstancePid)
    if ($found.Count -eq 1) { return [int]$found[0] }

    if ($found.Count -eq 0) {
        throw ("No AgentPad instance found. Is GTA III running with AgentPad.asi loaded? " +
               "(The section only exists while the game does, and only for the same Windows " +
               "user in the same session.)")
    }

    throw ("$($found.Count) game instances are running (pids: $($found -join ', ')). " +
           "Say which one with -GamePid, or pick a default with Select-GtaInstance -GamePid <n>. " +
           "Run Get-GtaInstance to see them.")
}

function Select-GtaInstance {
    <#
    .SYNOPSIS
    Makes one instance the default for every later command in this session.

    .DESCRIPTION
    Saves passing -GamePid to everything when a script is driving one of
    several games. Pass -None to go back to automatic resolution.

    .EXAMPLE
    Select-GtaInstance -GamePid 4312
    Send-GtaInput -Button Enter        # goes to 4312

    .EXAMPLE
    Get-GtaInstance | Where-Object GameState -eq 'frontend' | Select-Object -First 1 | Select-GtaInstance
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, ValueFromPipelineByPropertyName)]
        [Alias('Pid')]
        [int]$GamePid = 0,
        [switch]$None
    )
    process {
        if ($None) { $script:DefaultPid = 0; return }
        if ($GamePid -le 0) { throw "Select-GtaInstance needs a -GamePid, or -None to clear it." }
        if (-not (Test-GtaInstance $GamePid)) {
            throw "No AgentPad section for pid $GamePid. Run Get-GtaInstance to see what is running."
        }
        $script:DefaultPid = $GamePid
    }
}

function Get-GtaInstance {
    <#
    .SYNOPSIS
    Every running game AgentPad is loaded in, one object each.

    .DESCRIPTION
    This is the first thing to run when more than one copy of GTA III is up. It
    reports the pid, what the mod is doing, which part of the game is running,
    and the window title, so a caller can tell the instances apart and pick one
    with -GamePid.

    Reading an instance's status does NOT claim its section: this is safe to
    run against a game somebody else's script is driving.

    .EXAMPLE
    Get-GtaInstance

    .EXAMPLE
    Get-GtaInstance -GamePid 4312
    #>
    [CmdletBinding()]
    param([Alias('Pid')][int]$GamePid = 0)

    $pids = if ($GamePid -gt 0) { @($GamePid) } else { @(Find-GtaInstancePid) }

    foreach ($p in $pids) {
        $mmf = $null
        try {
            $mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
                (Get-GtaSectionName $p),
                [System.IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
        } catch {
            if ($GamePid -gt 0) {
                throw "No AgentPad section for pid $p. Is that game running with AgentPad.asi loaded?"
            }
            continue   # a stale index entry: the game is gone
        }

        try {
            $v = $mmf.CreateViewAccessor(0, $script:ShmSize,
                [System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
            try {
                $statusCode = $v.ReadUInt32([int64]$script:Off.ModStatus)
                $gameCode   = $v.ReadUInt32([int64]$script:Off.ModGameState)

                $procName = ''
                $window   = ''
                try {
                    $proc     = Get-Process -Id $p -ErrorAction Stop
                    $procName = $proc.ProcessName
                    $window   = $proc.MainWindowTitle
                } catch { }

                [pscustomobject]@{
                    GamePid     = $p
                    ProcessName = $procName
                    Window      = $window
                    ModLoaded   = ($v.ReadUInt32([int64]$script:Off.ModMagic) -eq $script:Magic)
                    Status      = $(if ($script:StatusName.ContainsKey([int]$statusCode)) { $script:StatusName[[int]$statusCode] } else { "unknown ($statusCode)" })
                    GameState   = $(if ($script:GameStateName.ContainsKey([int]$gameCode)) { $script:GameStateName[[int]$gameCode] } else { "unknown ($gameCode)" })
                    Frames      = $v.ReadUInt32([int64]$script:Off.ModFrames)
                    Applied     = $v.ReadUInt32([int64]$script:Off.ModApplied)
                    Enabled     = ($v.ReadUInt32([int64]$script:Off.Enabled) -ne 0)
                    Attached    = $script:Instances.ContainsKey($p)
                    IsDefault   = ($script:DefaultPid -eq $p)
                    Section     = (Get-GtaSectionName $p)
                }
            } finally { $v.Dispose() }
        } finally { $mmf.Dispose() }
    }
}

# ---------------------------------------------------------------------------
# Section access
# ---------------------------------------------------------------------------

function Get-GtaInputView {
    <#
    .SYNOPSIS
    Opens the shared-memory section one game publishes, or reuses an open one.
    #>
    [CmdletBinding()]
    param([Alias('Pid')][int]$GamePid = 0)

    $target = Resolve-GtaInstancePid -GamePid $GamePid
    if ($script:Instances.ContainsKey($target)) { return $script:Instances[$target].View }

    $name = Get-GtaSectionName $target
    try {
        $mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
            $name,
            [System.IO.MemoryMappedFiles.MemoryMappedFileRights]::ReadWrite)
    } catch {
        throw ("Cannot open shared memory '{0}'. Is AgentPad.asi loaded and that game running? " +
               "(The section only exists while the game does, and only for the same Windows " +
               "user in the same session.)") -f $name
    }

    $view = $mmf.CreateViewAccessor(0, $script:ShmSize)

    # Claim the section. The mod treats a header it does not recognise as "no
    # driver" and passes input through, so this is what turns the mod on at all.
    $view.Write([int64]$script:Off.Magic,   [uint32]$script:Magic)
    $view.Write([int64]$script:Off.Version, [uint32]$script:Version)

    $script:Instances[$target] = @{
        Mmf      = $mmf
        View     = $view
        PadState = (New-Object 'int16[]' 21)
        KeyState = 0
    }
    return $view
}

function Close-GtaInput {
    <#
    .SYNOPSIS
    Releases everything and hands control back to the keyboard.

    .DESCRIPTION
    Clears the injected state, disables injection and unmaps the section. Run
    this when you are done, or the last state you wrote stays latched.

    With no arguments this releases EVERY instance this session has opened,
    which is what a finally block wants. Pass -GamePid to release just one.
    #>
    [CmdletBinding()]
    param([Alias('Pid')][int]$GamePid = 0)

    $targets = if ($GamePid -gt 0) { @($GamePid) } else { @($script:Instances.Keys) }

    foreach ($p in @($targets)) {
        $key = [int]$p
        if (-not $script:Instances.ContainsKey($key)) { continue }
        $inst = $script:Instances[$key]
        try { Reset-GtaInput -GamePid $key } catch { }
        try { $inst.View.Dispose() } catch { }
        try { $inst.Mmf.Dispose() } catch { }
        $script:Instances.Remove($key)
        if ($script:DefaultPid -eq $key) { $script:DefaultPid = 0 }
    }
}

# ---------------------------------------------------------------------------
# Name resolution, exported so the tests can reach it without a running game
# ---------------------------------------------------------------------------

function Resolve-GtaInputName {
    <#
    .SYNOPSIS
    Turns a button or key name into the channel and slot it drives.

    .DESCRIPTION
    Resolution order, which is also the design: a few friendly *action* names
    first (Enter, Escape, Up, ...), because those are what a script author
    reaches for and the frontend menu accepts more than one route to each; then
    raw controller buttons; then raw key names.

    Returns a hashtable with Keys (bitmask) and Pad (array of field names), or
    throws on an unknown name. It never guesses: a typo presses nothing.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyString()][AllowNull()][string]$Name)

    if ([string]::IsNullOrWhiteSpace($Name)) {
        throw "Button name is empty. Try one of: $((Get-GtaInputNames) -join ', ')"
    }
    $n = $Name.Trim()

    # Menu actions. Each drives both routes the frontend reads, because
    # CMenuManager checks the keyboard state *and* the pad: 'Up' is
    # `GetUp() || GetDPadUpJustDown()`, 'Enter' is
    # `GetEnterJustDown() || GetCrossJustDown()`. Driving both is what makes a
    # menu tap work regardless of which branch this build takes.
    $actions = @{
        'Enter'  = @{ Key = @('Enter'); Pad = @('Cross') }
        'Return' = @{ Key = @('Enter'); Pad = @('Cross') }
        'Accept' = @{ Key = @('Enter'); Pad = @('Cross') }
        'Escape' = @{ Key = @('Escape'); Pad = @('Triangle') }
        'Esc'    = @{ Key = @('Escape'); Pad = @('Triangle') }
        'Back'   = @{ Key = @('Escape'); Pad = @('Triangle') }
        'Up'     = @{ Key = @('Up'); Pad = @('DPadUp') }
        'Down'   = @{ Key = @('Down'); Pad = @('DPadDown') }
        'Left'   = @{ Key = @('Left'); Pad = @('DPadLeft') }
        'Right'  = @{ Key = @('Right'); Pad = @('DPadRight') }
    }

    foreach ($k in $actions.Keys) {
        if ($k -eq $n) {
            $a = $actions[$k]
            $mask = 0
            foreach ($key in $a.Key) { $mask = $mask -bor $script:KeyBits[$key] }
            return @{ KeyMask = $mask; Pad = $a.Pad }
        }
    }

    # Raw controller button or axis.
    $padName = $null
    foreach ($k in $script:PadIndex.Keys) { if ($k -eq $n) { $padName = $k; break } }
    if ($null -eq $padName) {
        foreach ($k in $script:PadAlias.Keys) { if ($k -eq $n) { $padName = $script:PadAlias[$k]; break } }
    }
    if ($null -ne $padName) {
        return @{ KeyMask = 0; Pad = @($padName) }
    }

    # Raw key name.
    foreach ($k in $script:KeyBits.Keys) {
        if ($k -eq $n) { return @{ KeyMask = $script:KeyBits[$k]; Pad = @() } }
    }

    throw "Unknown button '$Name'. Known names: $((Get-GtaInputNames) -join ', ')"
}

function Get-GtaInputContract {
    <#
    .SYNOPSIS
    The wire contract this module believes in: offsets, constants and tables.

    .DESCRIPTION
    Exported so GtaInput.Tests.ps1 can diff it against what the C++ side
    actually compiled (`padtest --contract`). Two hardcoded copies of a layout
    that merely agree with themselves are worth nothing; this is what makes
    them agree with each other.
    #>
    [CmdletBinding()]
    param()
    [pscustomobject]@{
        ShmPrefix    = $script:ShmPrefix
        ShmIndexName = $script:ShmIndexName
        ShmSize      = $script:ShmSize
        IndexSize    = $script:IndexSize
        IndexSlots   = $script:IndexSlots
        Magic        = $script:Magic
        Version      = $script:Version
        Offsets      = $script:Off
        IndexOffsets = $script:IndexOff
        Channels     = $script:Channel
        ChannelAll   = $script:ChannelAll
        KeyBits      = $script:KeyBits
        PadIndex     = $script:PadIndex
        PadAlias     = $script:PadAlias
        GameState    = $script:GameStateName
        ButtonDown   = $script:ButtonDown
        MoveRun      = $script:MoveRun
        MoveWalk     = $script:MoveWalk
    }
}

function Get-GtaInputNames {
    <#
    .SYNOPSIS
    Every name Send-GtaInput and Set-GtaInput accept.
    #>
    [CmdletBinding()]
    param()
    @('Enter', 'Escape', 'Up', 'Down', 'Left', 'Right') +
    @($script:PadIndex.Keys) + @($script:PadAlias.Keys) + @($script:KeyBits.Keys) |
        Select-Object -Unique
}

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------

# The state this module is currently asking a given mod to inject. Kept per
# instance rather than read back from the section so that two overlapping holds
# compose: a script can hold Forward and then tap Cross without the tap
# clearing the walk, and so that a tap in one game does not clear a walk in
# another.
function Get-GtaInstanceState {
    [CmdletBinding()]
    param([Parameter(Mandatory)][int]$GamePid)
    if (-not $script:Instances.ContainsKey($GamePid)) {
        $null = Get-GtaInputView -GamePid $GamePid
    }
    $script:Instances[$GamePid]
}

function Write-GtaInputState {
    <#
    .SYNOPSIS
    Publishes the current state and bumps the sequence number.

    .DESCRIPTION
    Everything is written before `seq`, which is written last. The mod reads
    seq, copies, and reads seq again; if it moved, the copy might be a blend of
    two generations and the mod reuses the previous frame instead. That is the
    whole locking protocol and it only works if seq really is written last.
    #>
    [CmdletBinding()]
    param(
        [int]$TimeoutMs = 0,
        [switch]$Disable,
        [Nullable[int]]$MouseX = $null,
        [Nullable[int]]$MouseY = $null,
        [int]$MouseStepMax = 0,
        [Alias('Pid')][int]$GamePid = 0
    )

    $target = Resolve-GtaInstancePid -GamePid $GamePid
    $inst   = Get-GtaInstanceState -GamePid $target
    $v      = $inst.View

    for ($i = 0; $i -lt 21; $i++) {
        $v.Write([int64]($script:Off.Pad + $i * 2), [int16]$inst.PadState[$i])
    }
    $v.Write([int64]$script:Off.KeyMask,      [uint32]$inst.KeyState)
    $v.Write([int64]$script:Off.Channels,     [uint32]$script:ChannelAll)
    $v.Write([int64]$script:Off.TimeoutMs,    [uint32]$TimeoutMs)
    $v.Write([int64]$script:Off.WriterTickMs, [uint32]([Environment]::TickCount -band 0xFFFFFFFF))
    $v.Write([int64]$script:Off.Enabled,      [uint32]$(if ($Disable) { 0 } else { 1 }))

    if ($null -ne $MouseX -or $null -ne $MouseY) {
        $v.Write([int64]$script:Off.MouseBudgetX, [int32]$(if ($null -ne $MouseX) { $MouseX } else { 0 }))
        $v.Write([int64]$script:Off.MouseBudgetY, [int32]$(if ($null -ne $MouseY) { $MouseY } else { 0 }))
        $v.Write([int64]$script:Off.MouseStepMax, [uint32]$MouseStepMax)
        $mseq = $v.ReadUInt32([int64]$script:Off.MouseSeq)
        $v.Write([int64]$script:Off.MouseSeq, [uint32](($mseq + 1) -band 0xFFFFFFFF))
    }

    $seq = $v.ReadUInt32([int64]$script:Off.Seq)
    $v.Write([int64]$script:Off.Seq, [uint32](($seq + 1) -band 0xFFFFFFFF))
}

function Reset-GtaInput {
    <#
    .SYNOPSIS
    Clears every held button and disables injection.

    .EXAMPLE
    Reset-GtaInput
    #>
    [CmdletBinding()]
    param([Alias('Pid')][int]$GamePid = 0)

    $target = Resolve-GtaInstancePid -GamePid $GamePid
    $inst   = Get-GtaInstanceState -GamePid $target
    for ($i = 0; $i -lt 21; $i++) { $inst.PadState[$i] = 0 }
    $inst.KeyState = 0
    Write-GtaInputState -Disable -MouseX 0 -MouseY 0 -GamePid $target
}

function Enable-GtaInput {
    <#
    .SYNOPSIS
    Turns injection on without pressing anything.
    #>
    [CmdletBinding()]
    param([Alias('Pid')][int]$GamePid = 0)
    Write-GtaInputState -GamePid $GamePid
}

function Disable-GtaInput {
    <#
    .SYNOPSIS
    Hands control back to the keyboard, keeping the section open.
    #>
    [CmdletBinding()]
    param([Alias('Pid')][int]$GamePid = 0)
    Write-GtaInputState -Disable -GamePid $GamePid
}

# ---------------------------------------------------------------------------
# Input
# ---------------------------------------------------------------------------

function Set-GtaInput {
    <#
    .SYNOPSIS
    Holds a button down, or releases it, and leaves it that way.

    .DESCRIPTION
    Use this for a hold that outlives the command: walking, or holding a menu
    direction to scroll. Pair every Down with an Up, or use Send-GtaInput,
    which does that for you.

    .PARAMETER Button
    A name from Get-GtaInputNames.

    .PARAMETER State
    Down or Up.

    .PARAMETER Value
    Override the value written for a Down. Digital buttons default to 255,
    which is what the game's own input layer writes. Analog axes take
    -32767..32767.

    .PARAMETER TimeoutMs
    Arm the mod's watchdog: if this script stops refreshing for this long, the
    mod releases everything by itself. Worth setting for a long hold, so a
    crashed script does not leave the player walking into the sea.

    .PARAMETER GamePid
    Which game, when more than one is running. See Get-GtaInstance.

    .EXAMPLE
    Set-GtaInput -Button DPadDown -State Down
    Set-GtaInput -Button DPadDown -State Up
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0)][string]$Button,
        [Parameter(Position = 1)][ValidateSet('Down', 'Up')][string]$State = 'Down',
        [Nullable[int]]$Value = $null,
        [ValidateRange(0, 3600000)][int]$TimeoutMs = 0,
        [Alias('Pid')][int]$GamePid = 0
    )

    $r    = Resolve-GtaInputName -Name $Button
    $down = $State -eq 'Down'

    $target = Resolve-GtaInstancePid -GamePid $GamePid
    $inst   = Get-GtaInstanceState -GamePid $target

    if ($r.KeyMask -ne 0) {
        if ($down) { $inst.KeyState = $inst.KeyState -bor  $r.KeyMask }
        else       { $inst.KeyState = $inst.KeyState -band (-bnot $r.KeyMask) }
    }

    foreach ($p in $r.Pad) {
        $idx = $script:PadIndex[$p]
        if ($down) {
            $val = if ($null -ne $Value) { $Value } else { $script:ButtonDown }
            if ($val -lt -32768 -or $val -gt 32767) {
                throw "Value $val is out of range for a controller field (-32768..32767)."
            }
            $inst.PadState[$idx] = [int16]$val
        } else {
            $inst.PadState[$idx] = [int16]0
        }
    }

    Write-GtaInputState -TimeoutMs $TimeoutMs -GamePid $target
}

function Send-GtaInput {
    <#
    .SYNOPSIS
    Presses a button, holds it briefly, and releases it.

    .DESCRIPTION
    The default hold is long enough to survive a frame at any sane frame rate:
    the game only sees an input if it is present when CPad::UpdatePads runs, and
    a 60 fps frame is 16 ms. 80 ms is roughly five frames, which is also short
    enough not to auto-repeat in the menu.

    The release is in a finally block, so Ctrl+C does not leave a button stuck.

    .PARAMETER Button
    A name from Get-GtaInputNames.

    .PARAMETER DurationMs
    How long to hold it.

    .PARAMETER Repeat
    Send it this many times, with -IntervalMs between presses.

    .PARAMETER GamePid
    Which game, when more than one is running. See Get-GtaInstance.

    .EXAMPLE
    Send-GtaInput -Button Enter

    .EXAMPLE
    Send-GtaInput -Button Down -Repeat 3
    Move three rows down a menu.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0)][string]$Button,
        [ValidateRange(1, 60000)][int]$DurationMs = 80,
        [ValidateRange(1, 1000)][int]$Repeat = 1,
        [ValidateRange(0, 60000)][int]$IntervalMs = 120,
        [Alias('Pid')][int]$GamePid = 0
    )

    # Resolve before touching the section, so a bad name is an error and not a
    # half-applied press.
    $null = Resolve-GtaInputName -Name $Button

    for ($i = 0; $i -lt $Repeat; $i++) {
        try {
            Set-GtaInput -Button $Button -State Down -GamePid $GamePid
            Start-Sleep -Milliseconds $DurationMs
        } finally {
            Set-GtaInput -Button $Button -State Up -GamePid $GamePid
        }
        if ($i -lt $Repeat - 1) { Start-Sleep -Milliseconds $IntervalMs }
    }
}

function Set-GtaStick {
    <#
    .SYNOPSIS
    Sets an analog stick directly.

    .DESCRIPTION
    Left drives movement (CPad::GetPedWalkLeftRight/UpDown read LeftStickX/Y).
    Right is the pad's camera axis; on PC the follow camera reads the mouse
    instead, so use Move-GtaCamera for that.

    .EXAMPLE
    Set-GtaStick -Stick Left -X 0 -Y -128
    Full forward, at the same magnitude the keyboard produces.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet('Left', 'Right')][string]$Stick,
        [ValidateRange(-32767, 32767)][int]$X = 0,
        [ValidateRange(-32767, 32767)][int]$Y = 0,
        [ValidateRange(0, 3600000)][int]$TimeoutMs = 0,
        [Alias('Pid')][int]$GamePid = 0
    )

    $target = Resolve-GtaInstancePid -GamePid $GamePid
    $inst   = Get-GtaInstanceState -GamePid $target

    if ($Stick -eq 'Left') { $xi = 0; $yi = 1 } else { $xi = 2; $yi = 3 }
    $inst.PadState[$xi] = [int16]$X
    $inst.PadState[$yi] = [int16]$Y
    Write-GtaInputState -TimeoutMs $TimeoutMs -GamePid $target
}

function Start-GtaWalk {
    <#
    .SYNOPSIS
    Walks or runs in a direction, relative to the camera.

    .DESCRIPTION
    Sets the left stick and leaves it set. Call Stop-GtaWalk to stop, or pass
    -DurationMs to have this command stop for you.

    The sign convention comes from the game: CPad::GetPedWalkUpDown derives its
    keyboard value as (DPadDown - DPadUp) / 2, so forward is *negative* Y. That
    is read out of re3 and has not been confirmed in a running game yet. If
    Forward walks backwards, this is the line to flip.

    -Run (the default) uses 128, the magnitude the keyboard produces. -Walk uses
    60, which is 1.0 in the game's world units.

    .EXAMPLE
    Start-GtaWalk -Direction Forward -DurationMs 2000
    Walk forward for two seconds, then stop.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0)][ValidateSet('Forward', 'Back', 'Left', 'Right')][string]$Direction,
        [switch]$Walk,
        [ValidateRange(0, 600000)][int]$DurationMs = 0,
        [switch]$Sprint,
        [Alias('Pid')][int]$GamePid = 0
    )

    $target = Resolve-GtaInstancePid -GamePid $GamePid
    $inst   = Get-GtaInstanceState -GamePid $target

    $m = if ($Walk) { $script:MoveWalk } else { $script:MoveRun }

    switch ($Direction) {
        'Forward' { $x = 0;   $y = -$m }
        'Back'    { $x = 0;   $y =  $m }
        'Left'    { $x = -$m; $y = 0 }
        'Right'   { $x =  $m; $y = 0 }
    }

    $inst.PadState[0] = [int16]$x
    $inst.PadState[1] = [int16]$y
    # CPad::GetSprint() is `NewState.Cross`, straight out of re3 Pad.cpp.
    $inst.PadState[$script:PadIndex['Cross']] = [int16]$(if ($Sprint) { $script:ButtonDown } else { 0 })

    # Arm the watchdog on an open-ended hold. If this script dies while the
    # player is running, the mod lets go after a second rather than never.
    Write-GtaInputState -TimeoutMs 1000 -GamePid $target

    if ($DurationMs -gt 0) {
        try {
            $deadline = [Environment]::TickCount + $DurationMs
            # Refresh the heartbeat while we wait, or the watchdog we just armed
            # would fire mid-walk.
            while ([Environment]::TickCount -lt $deadline) {
                Start-Sleep -Milliseconds 250
                Write-GtaInputState -TimeoutMs 1000 -GamePid $target
            }
        } finally {
            Stop-GtaWalk -GamePid $target
        }
    }
}

function Stop-GtaWalk {
    <#
    .SYNOPSIS
    Centres the left stick and drops sprint.
    #>
    [CmdletBinding()]
    param([Alias('Pid')][int]$GamePid = 0)

    $target = Resolve-GtaInstancePid -GamePid $GamePid
    $inst   = Get-GtaInstanceState -GamePid $target
    $inst.PadState[0] = [int16]0
    $inst.PadState[1] = [int16]0
    $inst.PadState[$script:PadIndex['Cross']] = [int16]0
    Write-GtaInputState -GamePid $target
}

function Move-GtaCamera {
    <#
    .SYNOPSIS
    Pans the camera by a relative mouse movement.

    .DESCRIPTION
    On PC the follow camera does not read the right stick: CCam's mouse cam
    reads CPad::GetMouseX()/GetMouseY(), which are
    CPad::NewMouseControllerState.x/y. This writes those.

    -Dx and -Dy are a *total*, not a speed. The mod delivers them over as many
    frames as -StepMax allows, so the same command turns the camera the same
    amount at 30 fps and at 60. Positive Dx turns right.

    How many units make 90 degrees depends on the in-game mouse sensitivity
    slider (CCam multiplies by TheCamera.m_fMouseAccelHorzntl), so it cannot be
    computed from outside the game. Calibrate once against your own settings:
    pan a known amount, read the heading, and scale. This is the one number in
    the module that needs a running game to pin down.

    .EXAMPLE
    Move-GtaCamera -Dx 900
    Pan right by 900 mouse units, delivered 30 per frame, half a second at 60 fps.

    .EXAMPLE
    Move-GtaCamera -Dx 0 -Dy 0
    Cancel a pan that is still in flight.
    #>
    [CmdletBinding()]
    param(
        [int]$Dx = 0,
        [int]$Dy = 0,
        [ValidateRange(0, 100000)][int]$StepMax = 30,
        [switch]$Wait,
        [Alias('Pid')][int]$GamePid = 0
    )

    $target = Resolve-GtaInstancePid -GamePid $GamePid
    Write-GtaInputState -MouseX $Dx -MouseY $Dy -MouseStepMax $StepMax -GamePid $target

    if ($Wait) {
        $v = Get-GtaInputView -GamePid $target
        # A generous ceiling: this is a progress wait, not a timeout policy.
        $deadline = [Environment]::TickCount + 30000
        while ([Environment]::TickCount -lt $deadline) {
            $lx = $v.ReadInt32([int64]$script:Off.ModMouseLeftX)
            $ly = $v.ReadInt32([int64]$script:Off.ModMouseLeftY)
            if ($lx -eq 0 -and $ly -eq 0) { break }
            Start-Sleep -Milliseconds 20
        }
    }
}

function Get-GtaInputStatus {
    <#
    .SYNOPSIS
    What the mod is doing right now, for one game.

    .DESCRIPTION
    Use this first when something is not working. It distinguishes the three
    failure modes that look identical from the outside: the mod is not loaded
    (this throws), the mod is loaded but nobody has claimed the section
    ("no driver"), and the mod is loaded and injecting but the game is ignoring
    it (Applied climbing while nothing moves, usually player controls disabled
    by a cutscene).

    Unlike Get-GtaInstance this CLAIMS the section, because it is the status of
    the thing this module is driving. Use Get-GtaInstance to look without
    touching.

    .EXAMPLE
    Get-GtaInputStatus
    #>
    [CmdletBinding()]
    param([Alias('Pid')][int]$GamePid = 0)

    $target     = Resolve-GtaInstancePid -GamePid $GamePid
    $v          = Get-GtaInputView -GamePid $target
    $statusCode = $v.ReadUInt32([int64]$script:Off.ModStatus)
    $gameCode   = $v.ReadUInt32([int64]$script:Off.ModGameState)

    [pscustomobject]@{
        GamePid      = $target
        ModLoaded    = ($v.ReadUInt32([int64]$script:Off.ModMagic) -eq $script:Magic)
        GamePidSaid  = $v.ReadUInt32([int64]$script:Off.ModPid)
        Status       = $(if ($script:StatusName.ContainsKey([int]$statusCode)) { $script:StatusName[[int]$statusCode] } else { "unknown ($statusCode)" })
        GameState    = $(if ($script:GameStateName.ContainsKey([int]$gameCode)) { $script:GameStateName[[int]$gameCode] } else { "unknown ($gameCode)" })
        Frames       = $v.ReadUInt32([int64]$script:Off.ModFrames)
        Applied      = $v.ReadUInt32([int64]$script:Off.ModApplied)
        Seq          = $v.ReadUInt32([int64]$script:Off.Seq)
        SeqSeen      = $v.ReadUInt32([int64]$script:Off.ModSeqSeen)
        Enabled      = ($v.ReadUInt32([int64]$script:Off.Enabled) -ne 0)
        MouseLeftX   = $v.ReadInt32([int64]$script:Off.ModMouseLeftX)
        MouseLeftY   = $v.ReadInt32([int64]$script:Off.ModMouseLeftY)
    }
}

function Wait-GtaGameState {
    <#
    .SYNOPSIS
    Blocks until the game reaches a given state.

    .DESCRIPTION
    The thing that makes unattended menu driving possible: after picking a save
    you have to wait for the load, and the load takes as long as it takes.
    Polling gGameState is how you know, rather than sleeping a guessed number
    of seconds and pressing keys into whatever happens to be on screen.

    .EXAMPLE
    Send-GtaInput -Button Enter
    Wait-GtaGameState -State playing -TimeoutMs 120000
    Start-GtaWalk -Direction Forward -DurationMs 2000
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0)]
        [ValidateSet('startup', 'init-logo', 'logo-movie', 'init-intro', 'intro-movie',
                     'init-once', 'init-frontend', 'frontend', 'init-playing', 'playing')]
        [string]$State,
        [ValidateRange(1, 3600000)][int]$TimeoutMs = 120000,
        [ValidateRange(10, 5000)][int]$PollMs = 200,
        [Alias('Pid')][int]$GamePid = 0
    )

    $target   = Resolve-GtaInstancePid -GamePid $GamePid
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        $now = (Get-GtaInputStatus -GamePid $target).GameState
        if ($now -eq $State) { return $true }
        Start-Sleep -Milliseconds $PollMs
    }
    throw "Timed out after ${TimeoutMs}ms waiting for game state '$State' on pid ${target}; it is '$((Get-GtaInputStatus -GamePid $target).GameState)'."
}

Export-ModuleMember -Function @(
    'Get-GtaInstance', 'Select-GtaInstance', 'Test-GtaInstance',
    'Find-GtaInstancePid', 'Get-GtaIndexedPid', 'Resolve-GtaInstancePid', 'Get-GtaSectionName',
    'Get-GtaInputView', 'Close-GtaInput',
    'Resolve-GtaInputName', 'Get-GtaInputNames', 'Get-GtaInputContract',
    'Write-GtaInputState', 'Reset-GtaInput', 'Enable-GtaInput', 'Disable-GtaInput',
    'Set-GtaInput', 'Send-GtaInput', 'Set-GtaStick',
    'Start-GtaWalk', 'Stop-GtaWalk', 'Move-GtaCamera',
    'Get-GtaInputStatus', 'Wait-GtaGameState'
)
