Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public class WinApi {
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")]
    public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc p, IntPtr l);
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$results = @()
[WinApi]::EnumWindows({
    param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [WinApi]::GetWindowText($h, $sb, 256) | Out-Null
    $title = $sb.ToString()
    if ($title -like "*s30gui*" -or $title -like "*Perco*") {
        $r = New-Object WinApi+RECT
        [WinApi]::GetWindowRect($h, [ref]$r) | Out-Null
        $script:results += "HWND=$h Title='$title' Rect=($($r.Left),$($r.Top),$r.Right,$r.Bottom) Visible=$([WinApi]::IsWindowVisible($h))"
    }
    return $true
}, [IntPtr]::Zero) | Out-Null
$results
