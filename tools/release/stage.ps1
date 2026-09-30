# Puts every file a CoopIII release carries into one folder, with the exact
# names the Setup and the website expect, and a SHA256SUMS.txt beside them.
# Nothing is uploaded: whoever owns the release looks at the folder and
# attaches its contents to a GitHub release of noxxUY/CoopIII.
#
#   powershell -ExecutionPolicy Bypass -File tools\release\stage.ps1
#   powershell -ExecutionPolicy Bypass -File tools\release\stage.ps1 -SkipBuild
#   powershell -ExecutionPolicy Bypass -File tools\release\stage.ps1 -Patch C:\x\gta3-<build>-v10.c3patch
#
# What lands in build\release-staging:
#
#   CoopIII-Setup.exe       the download the website's button points at
#   CoopIII.asi, coopiii-launcher.exe, server.exe
#                           the same builds on their own, for people who
#                           install by hand and for running a server
#   mirror-*.zip            byte-identical copies of the MIT components'
#                           upstream releases, used only when upstream fails
#   FramerateVigilante.III.asi + FramerateVigilante-LICENSE.txt
#                           the one component with no upstream download
#   *.c3patch               the downgrade patches the manifest names, if given,
#                           and only if each stores at most -MaxStoredPercent
#                           (25) of v1.0 as itself
#   THIRD-PARTY-NOTICES.txt who made each component and under what licence
#   SHA256SUMS.txt
#
# Every third-party file is checked against the SHA-256 in
# installer/assets/components.json before it is staged, and the script
# refuses to stage anything whose MD5 is a gta3.exe build the manifest knows.
param(
    [string]$Out = "",
    [string]$VigilanteAsi = "$env:USERPROFILE\Downloads\essentials-mod-pack_1741967254_825844 (1)\essentials-mod-pack_1741967254_825844\modloader\_ESSENTIALS\FramerateVigilante\FramerateVigilante.III.asi",
    [string[]]$Patch = @(),
    [double]$MaxStoredPercent = 25,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ($Out -eq "") { $Out = Join-Path $root "build\release-staging" }
$bin      = Join-Path $root "build\windows\x86\release"
$upstream = Join-Path $root "build\upstream"
$ours     = "https://github.com/noxxUY/CoopIII/releases/"

function Sha256Of([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }
function Md5Of([string]$path)    { (Get-FileHash -Algorithm MD5 -LiteralPath $path).Hash.ToUpper() }
function Leaf([string]$url)      { ($url -split '/')[-1] }

# Downloads $url into the upstream cache unless a copy with the right hash is
# already there, and returns its path.
function Fetch([string]$url, [string]$sha) {
    New-Item -ItemType Directory -Force $upstream | Out-Null
    $dest = Join-Path $upstream ("{0}-{1}" -f $sha.Substring(0, 12), (Leaf $url))
    if ((Test-Path -LiteralPath $dest) -and (Sha256Of $dest) -eq $sha) { return $dest }
    Write-Host "  fetching $url"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    $got = Sha256Of $dest
    if ($got -ne $sha) {
        Remove-Item -LiteralPath $dest
        throw "$url does not match the manifest: expected $sha, got $got"
    }
    return $dest
}

if (-not $SkipBuild) {
    Push-Location $root
    try {
        & xmake f -p windows -a x86 -m release -y | Out-Null
        foreach ($t in "client", "launcher", "server", "installer") {
            & xmake build -y $t
            if ($LASTEXITCODE -ne 0) { throw "xmake build $t failed" }
        }
    } finally { Pop-Location }
}

if (Test-Path -LiteralPath $Out) { Remove-Item -Recurse -Force -LiteralPath $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

Write-Host "our own builds"
foreach ($f in "CoopIII-Setup.exe", "CoopIII.asi", "coopiii-launcher.exe", "server.exe") {
    $src = Join-Path $bin $f
    if (-not (Test-Path -LiteralPath $src)) { throw "$src is missing; build first" }
    Copy-Item -LiteralPath $src -Destination (Join-Path $Out $f)
    Write-Host "  $f"
}

$manifest = Get-Content -Raw -LiteralPath (Join-Path $root "installer\assets\components.json") | ConvertFrom-Json

Write-Host "third-party files"
$notices = New-Object System.Collections.Generic.List[string]
$notices.Add("CoopIII-Setup installs the following components. They are the work of their")
$notices.Add("authors, not of CoopIII. Each is fetched from its authors' own release; the")
$notices.Add("files named mirror-* in this release are unmodified copies of MIT-licensed")
$notices.Add("releases, used only if the original download is unavailable.")
$notices.Add("")
foreach ($c in $manifest.components) {
    if ($c.source -ne "download") { continue }
    $notices.Add(("{0} {1}" -f $c.name, $c.version))
    $notices.Add(("  licence:  {0}" -f $c.license))
    $notices.Add(("  homepage: {0}" -f $c.homepage))
    $notices.Add(("  source:   {0}" -f $c.url))
    $notices.Add("")

    $mirrors = @()
    if ($c.mirrors) { $mirrors = @($c.mirrors) }
    $isOurs = $c.url.StartsWith($ours)
    if (($mirrors.Count -gt 0 -or $isOurs) -and $c.license -notlike "MIT*") {
        throw "$($c.id) would be rehosted but its licence is '$($c.license)'"
    }

    foreach ($m in $mirrors) {
        if (-not $m.StartsWith($ours)) { continue }
        $file = Fetch $c.url $c.sha256
        Copy-Item -LiteralPath $file -Destination (Join-Path $Out (Leaf $m))
        Write-Host ("  {0}  <- {1}" -f (Leaf $m), $c.url)
    }

    if ($isOurs) {
        # Only Framerate Vigilante today: no stable upstream download exists,
        # so the release is its source.
        $name = Leaf $c.url
        $src  = $null
        if ($c.id -eq "vigilante" -and (Test-Path -LiteralPath $VigilanteAsi) -and (Sha256Of $VigilanteAsi) -eq $c.sha256) {
            $src = $VigilanteAsi
        } else {
            # The manifest deliberately pins third-party assets to the first
            # public release. Do not silently move them with CoopIII's latest
            # tag: the recorded hash and tested mod stack are immutable.
            $src = Fetch $c.url $c.sha256
        }
        Copy-Item -LiteralPath $src -Destination (Join-Path $Out $name)
        Write-Host "  $name"
    }
}
Copy-Item -LiteralPath (Join-Path $root "installer\assets\FramerateVigilante.LICENSE.txt") -Destination (Join-Path $Out "FramerateVigilante-LICENSE.txt")
[IO.File]::WriteAllLines((Join-Path $Out "THIRD-PARTY-NOTICES.txt"), $notices)

if ($manifest.patches) {
    Write-Host "downgrade patches"
    foreach ($p in $manifest.patches) {
        $given = $Patch | Where-Object { (Split-Path -Leaf $_) -eq $p.file } | Select-Object -First 1
        if (-not $given) { throw "the manifest names $($p.file) but no -Patch gave it" }
        if ((Sha256Of $given) -ne $p.sha256) { throw "$given does not match the manifest's SHA-256" }
        # The same ceiling mkpatch applies (kMaxStoredFraction): the share of
        # v1.0 the patch carries as itself. Over it, the file is a copy of the
        # game rather than a patch, and it does not go out.
        $bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $given).Path)
        if ($bytes.Length -lt 124 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 8) -ne "C3PATCH2") {
            throw "$given is not a CoopIII downgrade patch"
        }
        $newSize = [BitConverter]::ToUInt32($bytes, 44)
        $stored  = [BitConverter]::ToUInt32($bytes, 100)
        $percent = 100.0 * $stored / [Math]::Max(1, $newSize)
        if ($percent -gt $MaxStoredPercent) {
            throw ("{0} stores {1:N1}% of v1.0 as itself, over the {2}% ceiling. A patch like that is a copy of the game and is never released. See docs/installer.md section 2." -f $p.file, $percent, $MaxStoredPercent)
        }
        Copy-Item -LiteralPath $given -Destination (Join-Path $Out $p.file)
        Write-Host "  $($p.file)"
    }
}

# The one rule this project never bends: no Rockstar executable goes out.
$known = @($manifest.builds | ForEach-Object { $_.md5.ToUpper() })
foreach ($f in Get-ChildItem -LiteralPath $Out -File) {
    if ($f.Name -ieq "gta3.exe" -or $known -contains (Md5Of $f.FullName)) {
        Remove-Item -Recurse -Force -LiteralPath $Out
        throw "$($f.Name) is a gta3.exe build; nothing was staged"
    }
}

$sums = Get-ChildItem -LiteralPath $Out -File | Sort-Object Name | ForEach-Object {
    "{0} *{1}" -f (Sha256Of $_.FullName), $_.Name
}
[IO.File]::WriteAllLines((Join-Path $Out "SHA256SUMS.txt"), $sums)

Write-Host ""
Write-Host "staged in $Out"
Get-ChildItem -LiteralPath $Out -File | Sort-Object Name | ForEach-Object {
    Write-Host ("  {0,10:N0}  {1}" -f $_.Length, $_.Name)
}
