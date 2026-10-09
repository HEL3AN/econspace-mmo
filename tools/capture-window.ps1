# Save a screenshot of a running EconSpace window (Windows).
#
#   powershell -File tools/capture-window.ps1 -Out shot.png                 # the game client
#   powershell -File tools/capture-window.ps1 -Out shot.png -Proc worldeditor
#
# Synthetic keyboard and mouse input does not reach a raylib window, so a view is set up
# with command-line arguments instead (econspace ... --zoom Z --shapes, worldeditor gallery)
# and captured with this. Every visual defect in this project was found by looking at a
# picture, not by reading code; this is how an agent gets one.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Proc = "econspace",
    [int]$SettleMs = 900
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class EconCapture {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

$windows = @(Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 })
if ($windows.Count -eq 0) {
    Write-Error "No window for '$Proc'. Is it running, and has it finished opening?"
    exit 1
}
$h = $windows[0].MainWindowHandle

# Asked of the window itself (PrintWindow), not copied off the screen: a screen copy takes
# whatever is on top, and a terminal or an editor is usually on top. The foreground request
# only helps a window that has not drawn yet.
[EconCapture]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds $SettleMs

$r = New-Object EconCapture+RECT
[EconCapture]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap(($r.R - $r.L), ($r.B - $r.T))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [EconCapture]::PrintWindow($h, $hdc, 2)  # 2 = PW_RENDERFULLCONTENT, for GPU-drawn windows
$g.ReleaseHdc($hdc)
if (-not $ok) { $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size) }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Output "saved $Out"
