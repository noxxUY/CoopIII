# Screenshots a running GUI by process name, so a screen can be checked
# against design/screens/ without a person having to look at it.
#
#   powershell -ExecutionPolicy Bypass -File tools\shot.ps1 -Process coopiii-launcher -Out shot.png
#
# Uses PrintWindow with PW_RENDERFULLCONTENT, which photographs the window's
# own pixels rather than the screen. That matters here: the server binds a UDP
# port, so a fresh build gets a Windows Firewall prompt sitting right over the
# middle of it, and a screen capture would photograph the prompt.
param(
    [Parameter(Mandatory = $true)][string]$Process,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$WaitMs = 1500
)

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

Start-Sleep -Milliseconds $WaitMs

$proc = Get-Process -Name $Process -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $proc) {
    Write-Error "No running '$Process' with a window"
    exit 1
}

$hwnd = $proc.MainWindowHandle
[Win]::ShowWindow($hwnd, 5) | Out-Null   # SW_SHOW
[Win]::SetForegroundWindow($hwnd) | Out-Null
# Park the pointer off the window: a button under the cursor draws its hover
# state, which is not what the design shows.
[Win]::SetCursorPos(2, 2) | Out-Null
Start-Sleep -Milliseconds 700

# DWMWA_EXTENDED_FRAME_BOUNDS: the visible bounds, without the invisible
# resize border a frameless window still carries.
$visible = New-Object Win+RECT
$hr = [Win]::DwmGetWindowAttribute($hwnd, 9, [ref]$visible, 16)
if ($hr -ne 0) { [Win]::GetWindowRect($hwnd, [ref]$visible) | Out-Null }

# PrintWindow draws the whole window including the invisible border, so
# capture that and crop to the visible part.
$outer = New-Object Win+RECT
[Win]::GetWindowRect($hwnd, [ref]$outer) | Out-Null
$ow = $outer.Right - $outer.Left
$oh = $outer.Bottom - $outer.Top
$w = $visible.Right - $visible.Left
$h = $visible.Bottom - $visible.Top
if ($w -le 0 -or $h -le 0) { Write-Error "Window has no size"; exit 1 }

$full = New-Object System.Drawing.Bitmap $ow, $oh
$gfx = [System.Drawing.Graphics]::FromImage($full)
$dc = $gfx.GetHdc()
$ok = [Win]::PrintWindow($hwnd, $dc, 2)   # PW_RENDERFULLCONTENT
$gfx.ReleaseHdc($dc)
$gfx.Dispose()

# A window PrintWindow cannot reach comes back as one flat colour; fall back to
# the screen rather than saving a blank rectangle.
$probe = ($full.GetPixel(10, 10)).ToArgb()
$midX = [int]($ow / 2)
$midY = [int]($oh / 2)
$flat = (($full.GetPixel($midX, $midY)).ToArgb() -eq $probe) -and
        (($full.GetPixel($ow - 10, $oh - 10)).ToArgb() -eq $probe)

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g2 = [System.Drawing.Graphics]::FromImage($bmp)
if ($ok -and -not $flat) {
    $srcX = $visible.Left - $outer.Left
    $srcY = $visible.Top - $outer.Top
    $dest = New-Object System.Drawing.Rectangle -ArgumentList 0, 0, $w, $h
    $g2.DrawImage($full, $dest, $srcX, $srcY, $w, $h, [System.Drawing.GraphicsUnit]::Pixel)
    $how = "printwindow"
} else {
    $g2.CopyFromScreen($visible.Left, $visible.Top, 0, 0, $bmp.Size)
    $how = "screen"
}
$g2.Dispose()
$full.Dispose()

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

Write-Output "$Out ${w}x${h} ($how)"
