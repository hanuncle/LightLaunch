[CmdletBinding()]
param(
    [string]$Executable = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $projectRoot "dist\LightLaunch.exe"
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LightLaunchSmokeNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MONITORINFO
    {
        public uint Size;
        public RECT Monitor;
        public RECT Work;
        public uint Flags;
    }

    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder className, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr window, StringBuilder title, int maximumCount);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr window, out RECT rect);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT point);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern IntPtr MonitorFromPoint(POINT point, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);

    [DllImport("user32.dll")]
    public static extern uint GetDoubleClickTime();

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr window, int id);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr childAfter,
                                             string className, string windowName);

    [DllImport("user32.dll")]
    public static extern IntPtr WindowFromPoint(POINT point);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    public static extern IntPtr GetWindowLongPtr(IntPtr window, int index);

    [DllImport("user32.dll")]
    public static extern bool GetLayeredWindowAttributes(IntPtr window, out uint colorKey,
                                                         out byte alpha, out uint flags);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    public static extern IntPtr SendMessageRect(IntPtr window, uint message, IntPtr wParam,
                                                ref RECT rect);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    public static extern IntPtr SendMessageText(IntPtr window, uint message, IntPtr wParam, string text);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalLock(IntPtr memory);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GlobalUnlock(IntPtr memory);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalFree(IntPtr memory);

    public static bool PostDropFiles(IntPtr window, string[] paths)
    {
        const uint GMEM_MOVEABLE = 0x0002;
        const uint GMEM_ZEROINIT = 0x0040;
        const uint WM_DROPFILES = 0x0233;
        string names = String.Join("\0", paths) + "\0\0";
        byte[] encoded = Encoding.Unicode.GetBytes(names);
        IntPtr memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                                    new UIntPtr((uint)(20 + encoded.Length)));
        if (memory == IntPtr.Zero) return false;
        IntPtr data = GlobalLock(memory);
        if (data == IntPtr.Zero)
        {
            GlobalFree(memory);
            return false;
        }

        Marshal.WriteInt32(data, 0, 20);
        Marshal.WriteInt32(data, 4, 0);
        Marshal.WriteInt32(data, 8, 0);
        Marshal.WriteInt32(data, 12, 0);
        Marshal.WriteInt32(data, 16, 1);
        Marshal.Copy(encoded, 0, IntPtr.Add(data, 20), encoded.Length);
        GlobalUnlock(memory);
        if (!PostMessage(window, WM_DROPFILES, memory, IntPtr.Zero))
        {
            GlobalFree(memory);
            return false;
        }
        return true;
    }

    private static bool TryGetListViewItemCoordinates(IntPtr listView, int itemIndex,
                                                      out IntPtr coordinates)
    {
        const uint LVM_ENSUREVISIBLE = 0x1013;
        SendMessage(listView, LVM_ENSUREVISIBLE, new IntPtr(itemIndex), IntPtr.Zero);
        int x = 40;
        int y = 40;
        coordinates = new IntPtr((y << 16) | (x & 0xFFFF));
        return true;
    }

    public static bool ClickListViewItem(IntPtr listView, int itemIndex)
    {
        const uint WM_LBUTTONDOWN = 0x0201;
        const uint WM_LBUTTONUP = 0x0202;
        IntPtr coordinates;
        if (!TryGetListViewItemCoordinates(listView, itemIndex, out coordinates)) return false;
        return PostMessage(listView, WM_LBUTTONDOWN, new IntPtr(1), coordinates) &&
               PostMessage(listView, WM_LBUTTONUP, IntPtr.Zero, coordinates);
    }

    public static bool DoubleClickListViewItem(IntPtr listView, int itemIndex)
    {
        const uint WM_LBUTTONDOWN = 0x0201;
        const uint WM_LBUTTONUP = 0x0202;
        const uint WM_LBUTTONDBLCLK = 0x0203;
        IntPtr coordinates;
        if (!TryGetListViewItemCoordinates(listView, itemIndex, out coordinates)) return false;
        return PostMessage(listView, WM_LBUTTONDOWN, new IntPtr(1), coordinates) &&
               PostMessage(listView, WM_LBUTTONUP, IntPtr.Zero, coordinates) &&
               PostMessage(listView, WM_LBUTTONDBLCLK, new IntPtr(1), coordinates) &&
               PostMessage(listView, WM_LBUTTONUP, IntPtr.Zero, coordinates);
    }

    public static bool ClickListBoxItem(IntPtr listBox, int itemIndex)
    {
        const uint LB_GETITEMHEIGHT = 0x01A1;
        const uint LB_GETITEMRECT = 0x0198;
        const uint LB_GETTOPINDEX = 0x018E;
        const uint LB_SETTOPINDEX = 0x0197;
        const uint WM_LBUTTONDOWN = 0x0201;
        const uint WM_LBUTTONUP = 0x0202;
        int itemHeight = SendMessage(listBox, LB_GETITEMHEIGHT,
                                     IntPtr.Zero, IntPtr.Zero).ToInt32();
        if (itemHeight <= 0) return false;
        RECT client;
        if (!GetClientRect(listBox, out client)) return false;
        int topRow = SendMessage(listBox, LB_GETTOPINDEX,
                                 IntPtr.Zero, IntPtr.Zero).ToInt32();
        int visibleRows = Math.Max(1, (client.Bottom - client.Top) / itemHeight);
        if (itemIndex < topRow || itemIndex >= topRow + visibleRows)
        {
            SendMessage(listBox, LB_SETTOPINDEX, new IntPtr(itemIndex), IntPtr.Zero);
        }
        RECT itemRect = new RECT();
        if (SendMessageRect(listBox, LB_GETITEMRECT, new IntPtr(itemIndex),
                            ref itemRect).ToInt64() == -1) return false;
        int x = (itemRect.Left + itemRect.Right) / 2;
        int y = (itemRect.Top + itemRect.Bottom) / 2;
        IntPtr coordinates = new IntPtr((y << 16) | (x & 0xFFFF));
        SendMessage(listBox, WM_LBUTTONDOWN, new IntPtr(1), coordinates);
        SendMessage(listBox, WM_LBUTTONUP, IntPtr.Zero, coordinates);
        return true;
    }

    public static IntPtr FindWindowForProcess(string className, string title, uint processId,
                                              bool visibleOnly)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint ownerProcessId;
            GetWindowThreadProcessId(window, out ownerProcessId);
            if (ownerProcessId != processId || (visibleOnly && !IsWindowVisible(window))) return true;

            StringBuilder actualClass = new StringBuilder(256);
            StringBuilder actualTitle = new StringBuilder(512);
            GetClassName(window, actualClass, actualClass.Capacity);
            GetWindowText(window, actualTitle, actualTitle.Capacity);
            if (actualClass.ToString() == className && actualTitle.ToString() == title)
            {
                result = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
"@

[LightLaunchSmokeNative]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$BM_CLICK = 0x00F5
$BM_GETCHECK = 0x00F0
$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$WM_SETTEXT = 0x000C
$LB_GETCOUNT = 0x018B
$LB_GETCURSEL = 0x0188
$LB_SETCURSEL = 0x0186
$LVM_GETITEMCOUNT = 0x1004
$trayCallbackMessage = 0x8002
$activateWindowMessage = 0x8003
$trayIconId = 1
$trayExitId = 5002
$addCategoryId = 1002
$deleteCategoryId = 1004
$pinButtonId = 3001
$closeContentButtonId = 3002
$categorySelectionCommand = (1 -shl 16) -bor 1001
$NIN_SELECT = 0x0400
$MONITOR_DEFAULTTONEAREST = 2
$IDOK = 1
$IDYES = 6
$BST_UNCHECKED = 0
$revealBudgetMs = 200
$hideBudgetMs = 220
$mainTitle = "LightLaunch - " + (-join @([char]0x8F7B, [char]0x91CF, [char]0x5E94, [char]0x7528, [char]0x542F, [char]0x52A8, [char]0x5668))
$contentTitle = "LightLaunch - " + (-join @([char]0x5206, [char]0x7EC4, [char]0x5185, [char]0x5BB9))
$GWL_EXSTYLE = -20
$WS_EX_LAYERED = 0x00080000
$LWA_COLORKEY = 0x00000001
$LWA_ALPHA = 0x00000002
$railColorKey = 0x00030201
$newCategoryTitle = -join @([char]0x65B0, [char]0x5EFA, [char]0x5206, [char]0x7EC4)
$deleteCategoryTitle = -join @([char]0x5220, [char]0x9664, [char]0x5206, [char]0x7EC4)
$smokeCategoryName = "UI " + (-join @([char]0x5192, [char]0x70DF, [char]0x6D4B, [char]0x8BD5))

function Wait-ForWindow(
    [string]$ClassName,
    [string]$Title,
    [Diagnostics.Process]$OwnerProcess,
    [int]$TimeoutMs = 3000,
    [bool]$VisibleOnly = $true
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        $OwnerProcess.Refresh()
        if ($OwnerProcess.HasExited) {
            throw "LightLaunch exited before its test window appeared. Close any existing LightLaunch instance and retry."
        }

        $window = [LightLaunchSmokeNative]::FindWindowForProcess(
            $ClassName, $Title, $OwnerProcess.Id, $VisibleOnly
        )
        if ($window -ne [IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 20
    }
    throw "Window did not appear: $ClassName / $Title"
}

function Wait-ForControl(
    [IntPtr]$Window,
    [int]$ControlId,
    [Diagnostics.Process]$OwnerProcess,
    [int]$TimeoutMs = 3000
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        $OwnerProcess.Refresh()
        if ($OwnerProcess.HasExited) {
            throw "LightLaunch exited while waiting for control $ControlId."
        }
        $control = [LightLaunchSmokeNative]::GetDlgItem($Window, $ControlId)
        if ($control -ne [IntPtr]::Zero) { return $control }
        Start-Sleep -Milliseconds 10
    }
    throw "Control $ControlId did not become ready."
}

function Wait-ForWindowVisibility(
    [IntPtr]$Window,
    [bool]$Visible,
    [Diagnostics.Process]$OwnerProcess,
    [int]$TimeoutMs = 3000
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        $OwnerProcess.Refresh()
        if ($OwnerProcess.HasExited) {
            throw "LightLaunch exited while waiting for its window visibility to change."
        }
        if ([LightLaunchSmokeNative]::IsWindowVisible($Window) -eq $Visible) { return }
        Start-Sleep -Milliseconds 20
    }
    throw "Window visibility did not become $Visible."
}

function Stop-TestProcess(
    [Diagnostics.Process]$Process,
    [IntPtr]$Window,
    [bool]$RequireGraceful = $false
) {
    if ($null -ne $Process -and -not $Process.HasExited) {
        if ($Window -eq [IntPtr]::Zero) {
            $Window = [LightLaunchSmokeNative]::FindWindowForProcess(
                "LightLaunch.MainWindow", $mainTitle, $Process.Id, $false
            )
        }
        if ($Window -ne [IntPtr]::Zero) {
            [LightLaunchSmokeNative]::PostMessage(
                $Window, $WM_COMMAND, [IntPtr]$trayExitId, [IntPtr]::Zero
            ) | Out-Null
        }
        if (-not $Process.WaitForExit(2000)) {
            Stop-Process -Id $Process.Id -Force
            $Process.WaitForExit()
            if ($RequireGraceful) {
                throw "LightLaunch did not exit through the notification-area Exit command."
            }
        }
    }
}

function Wait-ForLockRemoval([string]$LockPath, [int]$TimeoutMs = 2000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ((Test-Path -LiteralPath $LockPath) -and $watch.ElapsedMilliseconds -lt $TimeoutMs) {
        Start-Sleep -Milliseconds 20
    }
    if (Test-Path -LiteralPath $LockPath) {
        throw "The per-config lock file remained after the process exited: $LockPath"
    }
}

function ConvertTo-ConfigValue([string]$Value) {
    return "b64:" + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Value))
}

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$smokeRoot = [IO.Path]::GetFullPath(
    (Join-Path $temporaryRoot ("LightLaunch-ui-smoke-" + [Guid]::NewGuid().ToString("N")))
)
if (-not $smokeRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to create a smoke-test directory outside the system temporary directory."
}

New-Item -ItemType Directory -Path $smokeRoot -Force | Out-Null
$smokeConfig = Join-Path $smokeRoot "config.ini"
$probeTarget = Join-Path $smokeRoot "launch-probe.cmd"
$probeMarker = Join-Path $smokeRoot "launch-marker.txt"
$dropTarget = Join-Path $smokeRoot "dragged-item.txt"
[IO.File]::WriteAllText(
    $probeTarget,
    "@echo off`r`n>> launch-marker.txt echo %1`r`n",
    [Text.Encoding]::ASCII
)
[IO.File]::WriteAllText($dropTarget, "LightLaunch drag-and-drop probe", [Text.Encoding]::UTF8)
$configText = "[General]`r`n" +
              "SchemaVersion=2`r`n" +
              "CategoryCount=1`r`n" +
              "SelectedCategory=0`r`n`r`n" +
              "[Category.0]`r`n" +
              "Name=$(ConvertTo-ConfigValue "Launch Probe")`r`n" +
              "ItemCount=1`r`n`r`n" +
              "[Category.0.Item.0]`r`n" +
              "Name=$(ConvertTo-ConfigValue "Probe")`r`n" +
              "Target=$(ConvertTo-ConfigValue $probeTarget)`r`n" +
              "Arguments=$(ConvertTo-ConfigValue "probe-value")`r`n" +
              "WorkingDirectory=$(ConvertTo-ConfigValue $smokeRoot)`r`n"
[IO.File]::WriteAllText($smokeConfig, $configText, [Text.Encoding]::Unicode)

$quotedConfig = '"' + $smokeConfig + '"'
$process = $null
$mainWindow = [IntPtr]::Zero
$contentWindow = [IntPtr]::Zero
$railInputWindow = [IntPtr]::Zero
$originalCursor = [LightLaunchSmokeNative+POINT]::new()
$cursorCaptured = [LightLaunchSmokeNative]::GetCursorPos([ref]$originalCursor)
try {
    if (-not $cursorCaptured) {
        throw "Unable to read the current pointer position."
    }
    $monitor = [LightLaunchSmokeNative]::MonitorFromPoint(
        $originalCursor, $MONITOR_DEFAULTTONEAREST
    )
    $monitorInfo = [LightLaunchSmokeNative+MONITORINFO]::new()
    $monitorInfo.Size = [Runtime.InteropServices.Marshal]::SizeOf(
        [type][LightLaunchSmokeNative+MONITORINFO]
    )
    if ($monitor -eq [IntPtr]::Zero -or
        -not [LightLaunchSmokeNative]::GetMonitorInfo($monitor, [ref]$monitorInfo)) {
        throw "Unable to resolve the current monitor work area."
    }
    $safeX = $monitorInfo.Work.Left + 20
    $safeY = [math]::Floor(($monitorInfo.Work.Top + $monitorInfo.Work.Bottom) / 2)
    [LightLaunchSmokeNative]::SetCursorPos($safeX, $safeY) | Out-Null

    $process = Start-Process -FilePath $Executable -ArgumentList @("--config", $quotedConfig) -PassThru -WindowStyle Normal

    $mainWindow = Wait-ForWindow "LightLaunch.MainWindow" $mainTitle $process 3000 $false
    $contentWindow = Wait-ForWindow "LightLaunch.ContentWindow" $contentTitle $process 3000 $false
    $railInputWindow = Wait-ForWindow "LightLaunch.RailInputWindow" "" $process 3000 $false
    if ([LightLaunchSmokeNative]::IsWindowVisible($mainWindow)) {
        throw "LightLaunch should start hidden in the notification area."
    }
    if ([LightLaunchSmokeNative]::IsWindowVisible($contentWindow) -or
        [LightLaunchSmokeNative]::IsWindowVisible($railInputWindow)) {
        throw "The content or transparent-rail input window was visible during hidden startup."
    }

    $edgeX = $monitorInfo.Work.Right - 1
    $edgeY = [math]::Floor(($monitorInfo.Work.Top + $monitorInfo.Work.Bottom) / 2)
    $revealWatch = [Diagnostics.Stopwatch]::StartNew()
    [LightLaunchSmokeNative]::SetCursorPos($edgeX, $edgeY) | Out-Null
    Wait-ForWindowVisibility $mainWindow $true $process
    $firstRevealMs = $revealWatch.ElapsedMilliseconds
    if ($firstRevealMs -gt $revealBudgetMs) {
        throw "Right-edge reveal took ${firstRevealMs}ms (budget: ${revealBudgetMs}ms)."
    }

    $panelRect = [LightLaunchSmokeNative+RECT]::new()
    if (-not [LightLaunchSmokeNative]::GetWindowRect($mainWindow, [ref]$panelRect)) {
        throw "Unable to inspect the edge panel rectangle."
    }
    if ($panelRect.Right -ne $monitorInfo.Work.Right -or
        $panelRect.Top -ne $monitorInfo.Work.Top -or
        $panelRect.Bottom -ne $monitorInfo.Work.Bottom) {
        throw "The transparent edge host was not aligned to the monitor work area's right edge."
    }
    $collapsedWidth = $panelRect.Right - $panelRect.Left

    $categoryList = Wait-ForControl $mainWindow 1001 $process
    $pinButton = Wait-ForControl $mainWindow $pinButtonId $process
    $itemList = Wait-ForControl $contentWindow 2001 $process
    $closeContentButton = Wait-ForControl $contentWindow $closeContentButtonId $process
    if ([LightLaunchSmokeNative]::GetDlgItem($mainWindow, 1002) -ne [IntPtr]::Zero) {
        throw "The removed new-category button still exists in the group rail."
    }
    if ([LightLaunchSmokeNative]::FindWindowEx(
            $mainWindow, [IntPtr]::Zero, "Static", "分组"
        ) -ne [IntPtr]::Zero) {
        throw "The removed group heading still exists in the group rail."
    }
    $mainExStyle = [LightLaunchSmokeNative]::GetWindowLongPtr(
        $mainWindow, $GWL_EXSTYLE
    ).ToInt64()
    if (($mainExStyle -band $WS_EX_LAYERED) -eq 0) {
        throw "The group rail is not configured as a transparent layered window."
    }
    $actualColorKey = [uint32]0
    $actualAlpha = [byte]0
    $actualLayerFlags = [uint32]0
    if (-not [LightLaunchSmokeNative]::GetLayeredWindowAttributes(
            $mainWindow, [ref]$actualColorKey, [ref]$actualAlpha,
            [ref]$actualLayerFlags
        ) -or ($actualLayerFlags -band $LWA_COLORKEY) -eq 0 -or
        $actualColorKey -ne $railColorKey) {
        throw "The group rail did not expose the expected transparent color key."
    }
    if (-not [LightLaunchSmokeNative]::IsWindowVisible($railInputWindow)) {
        throw "The transparent rail did not expose its blank-area input surface."
    }
    $inputAlpha = [byte]0
    $inputColorKey = [uint32]0
    $inputLayerFlags = [uint32]0
    if (-not [LightLaunchSmokeNative]::GetLayeredWindowAttributes(
            $railInputWindow, [ref]$inputColorKey, [ref]$inputAlpha,
            [ref]$inputLayerFlags
        ) -or ($inputLayerFlags -band $LWA_ALPHA) -eq 0 -or
        ($inputLayerFlags -band $LWA_COLORKEY) -eq 0 -or
        $inputColorKey -ne $railColorKey -or $inputAlpha -lt 200 -or
        $inputAlpha -ge 255) {
        throw "The macOS-style panel is not using the expected translucent color-key surface."
    }
    if ([LightLaunchSmokeNative]::IsWindowVisible($itemList)) {
        throw "The content area should be collapsed when the edge panel first appears."
    }
    $collapsedItemCount = [LightLaunchSmokeNative]::SendMessage(
        $itemList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
    ).ToInt32()
    if ($collapsedItemCount -ne 0) {
        throw "The collapsed content area retained launch items."
    }

    $categoryListRect = [LightLaunchSmokeNative+RECT]::new()
    $railInputRect = [LightLaunchSmokeNative+RECT]::new()
    if (-not [LightLaunchSmokeNative]::GetWindowRect($categoryList, [ref]$categoryListRect) -or
        -not [LightLaunchSmokeNative]::GetWindowRect($railInputWindow, [ref]$railInputRect)) {
        throw "The macOS-style panel geometry could not be inspected."
    }
    $railWidth = $railInputRect.Right - $railInputRect.Left
    $railHeight = $railInputRect.Bottom - $railInputRect.Top
    if ($railInputRect.Left -gt $categoryListRect.Left -or
        $railInputRect.Top -gt $categoryListRect.Top -or
        $railInputRect.Right -lt $categoryListRect.Right -or
        $railInputRect.Bottom -lt $categoryListRect.Bottom -or
        $railHeight -le $railWidth -or
        $railHeight -ge ($monitorInfo.Work.Bottom - $monitorInfo.Work.Top) -or
        [math]::Abs(($railInputRect.Top + $railInputRect.Bottom) -
                    ($monitorInfo.Work.Top + $monitorInfo.Work.Bottom)) -gt 2 -or
        $railInputRect.Right -gt $monitorInfo.Work.Right -or
        ($monitorInfo.Work.Right - $railInputRect.Right) -gt 64) {
        throw "The visible panel is not a centered vertical surface at the monitor's right edge."
    }
    $pinRect = [LightLaunchSmokeNative+RECT]::new()
    if (-not [LightLaunchSmokeNative]::GetWindowRect($pinButton, [ref]$pinRect)) {
        throw "The compact fixed icon geometry could not be inspected."
    }
    $pinWidth = $pinRect.Right - $pinRect.Left
    $pinHeight = $pinRect.Bottom - $pinRect.Top
    if ([math]::Abs($pinWidth - $pinHeight) -gt 2 -or
        $pinWidth -gt 56 -or $pinWidth -lt 20 -or
        $pinRect.Left -lt $railInputRect.Left -or
        $pinRect.Top -lt $railInputRect.Top -or
        $pinRect.Right -gt $railInputRect.Right -or
        $pinRect.Bottom -gt $railInputRect.Bottom) {
        throw "The fixed control is not a small square icon inside the panel."
    }
    [LightLaunchSmokeNative]::SetCursorPos(
        [math]::Floor(($pinRect.Left + $pinRect.Right) / 2),
        [math]::Floor(($pinRect.Top + $pinRect.Bottom) / 2)
    ) | Out-Null
    $cardHeight = [LightLaunchSmokeNative]::SendMessage(
        $categoryList, 0x01A1, [IntPtr]::Zero, [IntPtr]::Zero
    ).ToInt32()
    if ($cardHeight -lt 56 -or $cardHeight -gt 120) {
        throw "The group item is not compact enough for the single-column Dock layout (row=$cardHeight, pin=$pinHeight)."
    }
    $blankPoint = [LightLaunchSmokeNative+POINT]::new()
    $blankPoint.X = $railInputRect.Left + 12
    $blankPoint.Y = $railInputRect.Top + 12
    if ([LightLaunchSmokeNative]::WindowFromPoint($blankPoint) -ne $railInputWindow) {
        throw "The Dock header blank area does not retain mouse hit testing."
    }

    if (-not [LightLaunchSmokeNative]::ClickListBoxItem($categoryList, 0)) {
        throw "The first group card could not be clicked."
    }
    Wait-ForWindowVisibility $contentWindow $true $process
    if (-not [LightLaunchSmokeNative]::IsWindowVisible($itemList)) {
        throw "Clicking a group did not open its content area."
    }
    $expandedRect = [LightLaunchSmokeNative+RECT]::new()
    if (-not [LightLaunchSmokeNative]::GetWindowRect($mainWindow, [ref]$expandedRect)) {
        throw "Unable to inspect the group rail after opening a group."
    }
    if ($expandedRect.Left -ne $panelRect.Left -or
        $expandedRect.Top -ne $panelRect.Top -or
        $expandedRect.Right -ne $panelRect.Right -or
        $expandedRect.Bottom -ne $panelRect.Bottom) {
        throw "Opening a group changed the transparent rail's position or width."
    }
    $contentRect = [LightLaunchSmokeNative+RECT]::new()
    if (-not [LightLaunchSmokeNative]::GetWindowRect($contentWindow, [ref]$contentRect)) {
        throw "Unable to inspect the centered content window."
    }
    if ($contentRect.Left -lt $monitorInfo.Work.Left -or
        $contentRect.Top -lt $monitorInfo.Work.Top -or
        $contentRect.Right -gt $monitorInfo.Work.Right -or
        $contentRect.Bottom -gt $monitorInfo.Work.Bottom -or
        [math]::Abs(($contentRect.Left + $contentRect.Right) -
                    ($monitorInfo.Work.Left + $monitorInfo.Work.Right)) -gt 2 -or
        [math]::Abs(($contentRect.Top + $contentRect.Bottom) -
                    ($monitorInfo.Work.Top + $monitorInfo.Work.Bottom)) -gt 2) {
        throw "The group content window is not centered inside the active monitor work area."
    }

    $transitStartX = $categoryListRect.Left +
                     [math]::Floor(($categoryListRect.Right - $categoryListRect.Left) / 6)
    $transitStartY = $categoryListRect.Top + [math]::Floor($cardHeight / 2)
    $transitEndX = $contentRect.Right - 40
    $transitEndY = [math]::Floor(($contentRect.Top + $contentRect.Bottom) / 2)
    [LightLaunchSmokeNative]::SetCursorPos($transitStartX, $transitStartY) | Out-Null
    for ($transitStep = 1; $transitStep -le 7; $transitStep++) {
        $transitX = [math]::Round(
            $transitStartX + (($transitEndX - $transitStartX) * $transitStep / 7)
        )
        $transitY = [math]::Round(
            $transitStartY + (($transitEndY - $transitStartY) * $transitStep / 7)
        )
        [LightLaunchSmokeNative]::SetCursorPos($transitX, $transitY) | Out-Null
        Start-Sleep -Milliseconds 45
        if (-not [LightLaunchSmokeNative]::IsWindowVisible($mainWindow) -or
            -not [LightLaunchSmokeNative]::IsWindowVisible($contentWindow)) {
            throw "The launcher hid while the pointer was moving from a group card to the centered content window."
        }
    }

    $itemCount = [LightLaunchSmokeNative]::SendMessage($itemList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    if ($itemCount -ne 1) {
        throw "Expected one launch probe item, found $itemCount."
    }

    [LightLaunchSmokeNative]::SendMessage(
        $closeContentButton, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero
    ) | Out-Null
    Wait-ForWindowVisibility $contentWindow $false $process
    if ([LightLaunchSmokeNative]::IsWindowVisible($itemList) -or
        -not [LightLaunchSmokeNative]::IsWindowVisible($mainWindow)) {
        throw "The content close button did not collapse the current group."
    }
    $closedRect = [LightLaunchSmokeNative+RECT]::new()
    [LightLaunchSmokeNative]::GetWindowRect($mainWindow, [ref]$closedRect) | Out-Null
    if ($closedRect.Left -ne $panelRect.Left -or
        $closedRect.Right -ne $panelRect.Right) {
        throw "Closing the content window changed the group rail width."
    }

    if (-not [LightLaunchSmokeNative]::ClickListBoxItem($categoryList, 0)) {
        throw "The first group card could not be reopened after closing its content."
    }
    Wait-ForWindowVisibility $contentWindow $true $process
    $itemListRect = [LightLaunchSmokeNative+RECT]::new()
    if (-not [LightLaunchSmokeNative]::GetWindowRect($itemList, [ref]$itemListRect)) {
        throw "Unable to inspect the opened content area."
    }
    [LightLaunchSmokeNative]::SetCursorPos(
        $itemListRect.Left + 40, $itemListRect.Top + 40
    ) | Out-Null
    if (-not [LightLaunchSmokeNative]::ClickListViewItem($itemList, 0)) {
        throw "Launch probe item position could not be resolved."
    }
    Start-Sleep -Milliseconds ([LightLaunchSmokeNative]::GetDoubleClickTime() + 150)
    if (Test-Path -LiteralPath $probeMarker) {
        throw "A single click unexpectedly launched the selected item."
    }
    if (-not [LightLaunchSmokeNative]::IsWindowVisible($mainWindow)) {
        $diagnosticCursor = [LightLaunchSmokeNative+POINT]::new()
        [LightLaunchSmokeNative]::GetCursorPos([ref]$diagnosticCursor) | Out-Null
        $diagnosticContentRect = [LightLaunchSmokeNative+RECT]::new()
        [LightLaunchSmokeNative]::GetWindowRect(
            $contentWindow, [ref]$diagnosticContentRect
        ) | Out-Null
        $diagnosticContentVisible = [LightLaunchSmokeNative]::IsWindowVisible($contentWindow)
        throw "A single click unexpectedly hid the main window (cursor=$($diagnosticCursor.X),$($diagnosticCursor.Y); content=$($diagnosticContentRect.Left),$($diagnosticContentRect.Top)-$($diagnosticContentRect.Right),$($diagnosticContentRect.Bottom); visible=$diagnosticContentVisible)."
    }

    if (-not [LightLaunchSmokeNative]::DoubleClickListViewItem($itemList, 0)) {
        throw "The launch probe item could not be double-clicked."
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    while (-not (Test-Path -LiteralPath $probeMarker) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 20
    }
    $probeLines = @(if (Test-Path -LiteralPath $probeMarker) {
        Get-Content -LiteralPath $probeMarker
    })
    if ($probeLines.Count -ne 1 -or $probeLines[0].Trim() -ne "probe-value") {
        throw "Double-click launch was not exactly once, or did not preserve target arguments and working directory."
    }
    Wait-ForWindowVisibility $mainWindow $false $process
    if ([LightLaunchSmokeNative]::IsWindowVisible($contentWindow) -or
        [LightLaunchSmokeNative]::IsWindowVisible($railInputWindow)) {
        throw "Launching an item did not hide every launcher surface."
    }
    $process.Refresh()
    if ($process.HasExited) {
        throw "LightLaunch exited instead of remaining in the notification area."
    }

    $revealWatch = [Diagnostics.Stopwatch]::StartNew()
    [LightLaunchSmokeNative]::SetCursorPos($edgeX, $edgeY) | Out-Null
    Wait-ForWindowVisibility $mainWindow $true $process
    $secondRevealMs = $revealWatch.ElapsedMilliseconds
    if ($secondRevealMs -gt $revealBudgetMs) {
        throw "Second right-edge reveal took ${secondRevealMs}ms (budget: ${revealBudgetMs}ms)."
    }
    $reopenedRect = [LightLaunchSmokeNative+RECT]::new()
    [LightLaunchSmokeNative]::GetWindowRect($mainWindow, [ref]$reopenedRect) | Out-Null
    if (($reopenedRect.Right - $reopenedRect.Left) -ne $collapsedWidth -or
        [LightLaunchSmokeNative]::IsWindowVisible($itemList)) {
        throw "The panel did not return in its default collapsed state after hiding."
    }

    $hideWatch = [Diagnostics.Stopwatch]::StartNew()
    [LightLaunchSmokeNative]::SetCursorPos($safeX, $safeY) | Out-Null
    Wait-ForWindowVisibility $mainWindow $false $process
    $firstHideMs = $hideWatch.ElapsedMilliseconds
    if ($firstHideMs -gt $hideBudgetMs) {
        throw "Mouse-leave hide took ${firstHideMs}ms (budget: ${hideBudgetMs}ms)."
    }

    [LightLaunchSmokeNative]::SetCursorPos($edgeX, $edgeY) | Out-Null
    Wait-ForWindowVisibility $mainWindow $true $process
    if (-not [LightLaunchSmokeNative]::ClickListBoxItem($categoryList, 0)) {
        throw "The first group could not be opened for the fixed-window test."
    }
    Wait-ForWindowVisibility $contentWindow $true $process
    [LightLaunchSmokeNative]::SendMessage(
        $pinButton, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero
    ) | Out-Null
    [LightLaunchSmokeNative]::SetCursorPos($safeX, $safeY) | Out-Null
    Start-Sleep -Milliseconds ($hideBudgetMs + 200)
    if (-not [LightLaunchSmokeNative]::IsWindowVisible($mainWindow) -or
        -not [LightLaunchSmokeNative]::IsWindowVisible($contentWindow) -or
        -not [LightLaunchSmokeNative]::IsWindowVisible($railInputWindow)) {
        throw "A launcher surface hid while the group rail was fixed."
    }

    [LightLaunchSmokeNative]::SendMessage(
        $pinButton, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero
    ) | Out-Null
    $unpinHideWatch = [Diagnostics.Stopwatch]::StartNew()
    Wait-ForWindowVisibility $mainWindow $false $process
    $unpinHideMs = $unpinHideWatch.ElapsedMilliseconds
    if ($unpinHideMs -gt $hideBudgetMs) {
        throw "The panel did not quickly hide after unpinning (${unpinHideMs}ms)."
    }
    if ([LightLaunchSmokeNative]::IsWindowVisible($contentWindow) -or
        [LightLaunchSmokeNative]::IsWindowVisible($railInputWindow)) {
        throw "Unpinning did not hide the centered content and rail input surfaces."
    }

    $traySelect = [IntPtr](($trayIconId -shl 16) -bor $NIN_SELECT)
    [LightLaunchSmokeNative]::PostMessage(
        $mainWindow, $trayCallbackMessage, [IntPtr]::Zero, $traySelect
    ) | Out-Null
    Wait-ForWindowVisibility $mainWindow $true $process
    if ([LightLaunchSmokeNative]::SendMessage(
            $pinButton, $BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero
        ).ToInt32() -ne $BST_UNCHECKED) {
        throw "The pin button remained checked after the explicit unpin action."
    }
    [LightLaunchSmokeNative]::SetCursorPos(
        [math]::Floor(($pinRect.Left + $pinRect.Right) / 2),
        [math]::Floor(($pinRect.Top + $pinRect.Bottom) / 2)
    ) | Out-Null

    [LightLaunchSmokeNative]::PostMessage(
        $mainWindow, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero
    ) | Out-Null
    Wait-ForWindowVisibility $mainWindow $false $process

    $secondProcess = Start-Process -FilePath $Executable -WorkingDirectory $smokeRoot `
        -ArgumentList @("--config", ".\config.ini") -PassThru -WindowStyle Hidden
    if (-not $secondProcess.WaitForExit(2000)) {
        Stop-Process -Id $secondProcess.Id -Force
        throw "Second LightLaunch process did not exit."
    }
    $process.Refresh()
    if ($process.HasExited) {
        throw "Primary LightLaunch process exited during the single-instance test."
    }
    Wait-ForWindowVisibility $mainWindow $true $process
    [LightLaunchSmokeNative]::SetCursorPos(
        [math]::Floor(($pinRect.Left + $pinRect.Right) / 2),
        [math]::Floor(($pinRect.Top + $pinRect.Bottom) / 2)
    ) | Out-Null

    if ([LightLaunchSmokeNative]::IsWindowVisible($itemList)) {
        throw "Single-instance activation should restore the collapsed group panel."
    }
    [LightLaunchSmokeNative]::SendMessage(
        $categoryList, $LB_SETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero
    ) | Out-Null
    [LightLaunchSmokeNative]::SendMessage(
        $mainWindow, $WM_COMMAND, [IntPtr]$categorySelectionCommand, $categoryList
    ) | Out-Null
    Wait-ForWindowVisibility $contentWindow $true $process

    if (-not [LightLaunchSmokeNative]::PostDropFiles($itemList, @($dropTarget))) {
        throw "The drag-and-drop probe could not be posted to the content area."
    }
    $dropDeadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        Start-Sleep -Milliseconds 20
        $droppedItemCount = [LightLaunchSmokeNative]::SendMessage(
            $itemList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
        ).ToInt32()
    } while ($droppedItemCount -ne 2 -and [DateTime]::UtcNow -lt $dropDeadline)
    if ($droppedItemCount -ne 2) {
        throw "Dropping a file did not add it to the selected group."
    }
    if (-not [LightLaunchSmokeNative]::PostDropFiles($itemList, @($dropTarget))) {
        throw "The duplicate drag-and-drop probe could not be posted."
    }
    Start-Sleep -Milliseconds 250
    $duplicateItemCount = [LightLaunchSmokeNative]::SendMessage(
        $itemList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
    ).ToInt32()
    if ($duplicateItemCount -ne 2) {
        throw "Dropping the same file twice created a duplicate item."
    }

    $initialRowCount = [LightLaunchSmokeNative]::SendMessage($categoryList, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    [LightLaunchSmokeNative]::PostMessage(
        $mainWindow, $WM_COMMAND, [IntPtr]$addCategoryId, [IntPtr]::Zero
    ) | Out-Null

    $inputDialog = Wait-ForWindow "LightLaunch.InputDialog" $newCategoryTitle $process
    $edit = [LightLaunchSmokeNative]::GetDlgItem($inputDialog, 100)
    $okButton = [LightLaunchSmokeNative]::GetDlgItem($inputDialog, $IDOK)
    if ($edit -eq [IntPtr]::Zero -or $okButton -eq [IntPtr]::Zero) {
        throw "Category input controls were not found."
    }
    [LightLaunchSmokeNative]::SendMessageText($edit, $WM_SETTEXT, [IntPtr]::Zero, $smokeCategoryName) | Out-Null
    [LightLaunchSmokeNative]::PostMessage($okButton, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null

    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    $encodedSmokeCategoryName = ConvertTo-ConfigValue $smokeCategoryName
    do {
        Start-Sleep -Milliseconds 20
        $createdConfig = [IO.File]::ReadAllText($smokeConfig, [Text.Encoding]::Unicode)
    } while ((-not $createdConfig.Contains("CategoryCount=2") -or
              -not $createdConfig.Contains($encodedSmokeCategoryName)) -and
             [DateTime]::UtcNow -lt $deadline)
    if (-not $createdConfig.Contains("CategoryCount=2") -or
        -not $createdConfig.Contains($encodedSmokeCategoryName)) {
        throw "Category creation was not persisted."
    }
    $createdRowCount = [LightLaunchSmokeNative]::SendMessage(
        $categoryList, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
    ).ToInt32()
    if ($createdRowCount -ne 2) {
        throw "Each group should occupy one item in the single-column Dock."
    }
    $dockLayoutDeadline = [DateTime]::UtcNow.AddSeconds(1)
    do {
        $secondDockRect = [LightLaunchSmokeNative+RECT]::new()
        $dockClientRect = [LightLaunchSmokeNative+RECT]::new()
        [void][LightLaunchSmokeNative]::SendMessageRect(
            $categoryList, 0x0198, [IntPtr]1, [ref]$secondDockRect
        )
        [void][LightLaunchSmokeNative]::GetClientRect($categoryList, [ref]$dockClientRect)
        if ($secondDockRect.Bottom -le $dockClientRect.Bottom) { break }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $dockLayoutDeadline)
    if ($secondDockRect.Bottom -gt $dockClientRect.Bottom) {
        $updatedRailRect = [LightLaunchSmokeNative+RECT]::new()
        [void][LightLaunchSmokeNative]::GetWindowRect($railInputWindow, [ref]$updatedRailRect)
        throw "The Dock did not grow to expose the newly created group (rail=$($updatedRailRect.Bottom - $updatedRailRect.Top), client=$($dockClientRect.Bottom), secondBottom=$($secondDockRect.Bottom))."
    }
    $reusedContentWindow = [LightLaunchSmokeNative]::FindWindowForProcess(
        "LightLaunch.ContentWindow", $contentTitle, $process.Id, $false
    )
    if ($reusedContentWindow -ne $contentWindow) {
        throw "Switching groups created a second content window instead of reusing one."
    }

    if (-not [LightLaunchSmokeNative]::ClickListBoxItem($categoryList, 0)) {
        throw "The original group tile could not be selected after category creation."
    }
    $firstCategoryItems = [LightLaunchSmokeNative]::SendMessage(
        $itemList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
    ).ToInt32()
    if ($firstCategoryItems -ne 2) {
        throw "Selecting the original group did not display its persisted items."
    }

    if (-not [LightLaunchSmokeNative]::ClickListBoxItem($categoryList, 1)) {
        throw "The second group tile could not be selected in the single-column Dock."
    }
    $newCategoryItems = [LightLaunchSmokeNative]::SendMessage(
        $itemList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
    ).ToInt32()
    if ($newCategoryItems -ne 0) {
        $selectedDockItem = [LightLaunchSmokeNative]::SendMessage(
            $categoryList, $LB_GETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero
        ).ToInt32()
        throw "Selecting the newly created group did not display its empty contents (selected=$selectedDockItem, items=$newCategoryItems)."
    }

    [LightLaunchSmokeNative]::PostMessage(
        $mainWindow, $WM_COMMAND, [IntPtr]$deleteCategoryId, [IntPtr]::Zero
    ) | Out-Null
    $confirmation = Wait-ForWindow "#32770" $deleteCategoryTitle $process
    $yesButton = [LightLaunchSmokeNative]::GetDlgItem($confirmation, $IDYES)
    if ($yesButton -eq [IntPtr]::Zero) { throw "Delete confirmation button was not found." }
    [LightLaunchSmokeNative]::PostMessage($yesButton, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null

    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        Start-Sleep -Milliseconds 20
        $deletedConfig = [IO.File]::ReadAllText($smokeConfig, [Text.Encoding]::Unicode)
    } while ((-not $deletedConfig.Contains("CategoryCount=1") -or
              $deletedConfig.Contains($encodedSmokeCategoryName)) -and
             [DateTime]::UtcNow -lt $deadline)
    $finalRowCount = [LightLaunchSmokeNative]::SendMessage(
        $categoryList, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
    ).ToInt32()
    if (-not $deletedConfig.Contains("CategoryCount=1") -or
        $deletedConfig.Contains($encodedSmokeCategoryName) -or
        $finalRowCount -ne $initialRowCount) {
        throw "Category deletion did not restore the original Dock list."
    }

    Stop-TestProcess $process $mainWindow $true
    Wait-ForLockRemoval ($smokeConfig + ".lock")
    $process = Start-Process -FilePath $Executable -ArgumentList @("--config", $quotedConfig) -PassThru -WindowStyle Normal
    $mainWindow = Wait-ForWindow "LightLaunch.MainWindow" $mainTitle $process 3000 $false
    $contentWindow = Wait-ForWindow "LightLaunch.ContentWindow" $contentTitle $process 3000 $false
    $railInputWindow = Wait-ForWindow "LightLaunch.RailInputWindow" "" $process 3000 $false
    if ([LightLaunchSmokeNative]::IsWindowVisible($mainWindow) -or
        [LightLaunchSmokeNative]::IsWindowVisible($contentWindow) -or
        [LightLaunchSmokeNative]::IsWindowVisible($railInputWindow)) {
        throw "LightLaunch did not return to hidden startup after restart."
    }
    $categoryList = Wait-ForControl $mainWindow 1001 $process
    $itemList = Wait-ForControl $contentWindow 2001 $process
    $pinButton = Wait-ForControl $mainWindow $pinButtonId $process
    $persistedCategories = [LightLaunchSmokeNative]::SendMessage($categoryList, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    $collapsedPersistedItems = [LightLaunchSmokeNative]::SendMessage($itemList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    if ($persistedCategories -ne 1 -or $collapsedPersistedItems -ne 0) {
        throw "Restart did not preserve groups in a collapsed content state (categories=$persistedCategories, visible-items=$collapsedPersistedItems)."
    }
    if ([LightLaunchSmokeNative]::SendMessage(
            $pinButton, $BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero
        ).ToInt32() -ne $BST_UNCHECKED) {
        throw "The panel unexpectedly persisted its fixed state across restart."
    }

    [LightLaunchSmokeNative]::SetCursorPos($edgeX, $edgeY) | Out-Null
    Wait-ForWindowVisibility $mainWindow $true $process
    if ([LightLaunchSmokeNative]::IsWindowVisible($itemList)) {
        throw "The content area should remain collapsed on the first reveal after restart."
    }
    $restartSelectionResult = [LightLaunchSmokeNative]::SendMessage(
        $categoryList, $LB_SETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero
    ).ToInt32()
    [LightLaunchSmokeNative]::SendMessage(
        $mainWindow, $WM_COMMAND, [IntPtr]$categorySelectionCommand, $categoryList
    ) | Out-Null
    Wait-ForWindowVisibility $contentWindow $true $process
    $restartOpenDeadline = [DateTime]::UtcNow.AddSeconds(1)
    do {
        $persistedItems = [LightLaunchSmokeNative]::SendMessage(
            $itemList, $LVM_GETITEMCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
        ).ToInt32()
        if ($persistedItems -eq 2) { break }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $restartOpenDeadline)
    if ($persistedItems -ne 2) {
        $restartSelected = [LightLaunchSmokeNative]::SendMessage(
            $categoryList, $LB_GETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero
        ).ToInt32()
        $restartContentVisible = [LightLaunchSmokeNative]::IsWindowVisible($itemList)
        throw "Opening the persisted group after restart found $persistedItems items instead of 2 (LB_SETCURSEL=$restartSelectionResult, selected=$restartSelected, content-visible=$restartContentVisible)."
    }

    Stop-TestProcess $process $mainWindow $true
    Wait-ForLockRemoval ($smokeConfig + ".lock")
    $process = $null
    $mainWindow = [IntPtr]::Zero
    Write-Host "UI smoke test passed: ${firstRevealMs}ms reveal, ${firstHideMs}ms hide, transparent rail hit testing, fixed/unfixed behavior, centered group content, close and switch reuse, double-click launch, drag-and-drop deduplication, tray lifecycle, single instance, category CRUD, and restart persistence."
}
finally {
    Stop-TestProcess $process $mainWindow
    Wait-ForLockRemoval ($smokeConfig + ".lock")

    if ($cursorCaptured) {
        [LightLaunchSmokeNative]::SetCursorPos($originalCursor.X, $originalCursor.Y) | Out-Null
    }

    if (Test-Path -LiteralPath $smokeRoot) {
        $resolvedSmokeRoot = [IO.Path]::GetFullPath($smokeRoot)
        if ($resolvedSmokeRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedSmokeRoot).StartsWith("LightLaunch-ui-smoke-")) {
            Remove-Item -LiteralPath $resolvedSmokeRoot -Recurse -Force
        }
    }
}
