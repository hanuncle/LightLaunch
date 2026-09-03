[CmdletBinding()]
param(
    [string]$Executable = "",
    [string]$Output = "",
    [string]$RailOutput = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $projectRoot "dist\LightLaunch.exe"
}
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $projectRoot "build\ui-smoke.png"
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$Output = [IO.Path]::GetFullPath($Output)
if ([string]::IsNullOrWhiteSpace($RailOutput)) {
    $RailOutput = Join-Path (Split-Path -Parent $Output) `
        (([IO.Path]::GetFileNameWithoutExtension($Output)) + "-rail.png")
}
$RailOutput = [IO.Path]::GetFullPath($RailOutput)
$outputDirectory = Split-Path -Parent $Output
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LightLaunchCaptureNative
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder className,
                                          int maximumCount);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr deviceContext, uint flags);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message,
                                          IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr window, int id);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr window, uint message,
                                            IntPtr wParam, IntPtr lParam);

    public static IntPtr FindWindowForProcess(string className, uint processId)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint ownerProcessId;
            GetWindowThreadProcessId(window, out ownerProcessId);
            if (ownerProcessId != processId) return true;

            StringBuilder actualClass = new StringBuilder(128);
            GetClassName(window, actualClass, actualClass.Capacity);
            if (actualClass.ToString() == className)
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

# Match the launcher's per-monitor DPI mode before Windows.Forms reads screen bounds.
[LightLaunchCaptureNative]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function ConvertTo-ConfigValue([string]$Value) {
    return "b64:" + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Value))
}

$WM_COMMAND = 0x0111
$BM_CLICK = 0x00F5
$LB_SETCURSEL = 0x0186
$categorySelectionCommand = (1 -shl 16) -bor 1001
$activateMessage = 0x8003
$trayExitId = 5002

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$captureRoot = [IO.Path]::GetFullPath(
    (Join-Path $temporaryRoot ("LightLaunch-capture-" + [Guid]::NewGuid().ToString("N")))
)
if (-not $captureRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to create a capture directory outside the system temporary directory."
}
New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null
$captureConfig = Join-Path $captureRoot "config.ini"
$quotedConfig = '"' + $captureConfig + '"'
$sampleTargets = @(
    @{ Name = "Notepad"; Target = (Join-Path $env:SystemRoot "System32\notepad.exe") },
    @{ Name = "Terminal"; Target = (Join-Path $env:SystemRoot "System32\cmd.exe") },
    @{ Name = "PowerShell"; Target = (Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe") },
    @{ Name = "Explorer"; Target = (Join-Path $env:SystemRoot "explorer.exe") }
)
$developerToolsName = -join @([char]0x5F00, [char]0x53D1, [char]0x5DE5, [char]0x5177)
$dailyOfficeName = -join @([char]0x65E5, [char]0x5E38, [char]0x529E, [char]0x516C)
$gameDevelopmentName = -join @([char]0x6E38, [char]0x620F, [char]0x5F00, [char]0x53D1)
$designName = -join @([char]0x8BBE, [char]0x8BA1)
$mediaName = -join @([char]0x5F71, [char]0x97F3, [char]0x5A31, [char]0x4E50)
$temporaryName = -join @([char]0x4E34, [char]0x65F6, [char]0x6536, [char]0x85CF)
$productivityName = -join @([char]0x6548, [char]0x7387, [char]0x5DE5, [char]0x5177)
$systemToolsName = -join @([char]0x7CFB, [char]0x7EDF, [char]0x5DE5, [char]0x5177)
$learningName = -join @([char]0x5B66, [char]0x4E60, [char]0x8D44, [char]0x6599)
$sampleCategories = @(
    @{ Name = $developerToolsName; Targets = @($sampleTargets[0], $sampleTargets[1], $sampleTargets[2], $sampleTargets[3]) },
    @{ Name = $dailyOfficeName; Targets = @($sampleTargets[0], $sampleTargets[3]) },
    @{ Name = $gameDevelopmentName; Targets = @($sampleTargets[2], $sampleTargets[1], $sampleTargets[3]) },
    @{ Name = $designName; Targets = @($sampleTargets[3], $sampleTargets[0]) },
    @{ Name = $mediaName; Targets = @($sampleTargets[1], $sampleTargets[2]) },
    @{ Name = $temporaryName; Targets = @() },
    @{ Name = $productivityName; Targets = @($sampleTargets[0], $sampleTargets[2]) },
    @{ Name = $systemToolsName; Targets = @($sampleTargets[3], $sampleTargets[1], $sampleTargets[2]) },
    @{ Name = $learningName; Targets = @($sampleTargets[0], $sampleTargets[3]) }
)
$captureConfigBuilder = [Text.StringBuilder]::new()
[void]$captureConfigBuilder.AppendLine("[General]")
[void]$captureConfigBuilder.AppendLine("SchemaVersion=2")
[void]$captureConfigBuilder.AppendLine("CategoryCount=$($sampleCategories.Count)")
[void]$captureConfigBuilder.AppendLine("SelectedCategory=0")
for ($categoryIndex = 0; $categoryIndex -lt $sampleCategories.Count; $categoryIndex++) {
    $category = $sampleCategories[$categoryIndex]
    [void]$captureConfigBuilder.AppendLine()
    [void]$captureConfigBuilder.AppendLine("[Category.$categoryIndex]")
    [void]$captureConfigBuilder.AppendLine("Name=$(ConvertTo-ConfigValue $category.Name)")
    [void]$captureConfigBuilder.AppendLine("ItemCount=$($category.Targets.Count)")
    for ($itemIndex = 0; $itemIndex -lt $category.Targets.Count; $itemIndex++) {
        [void]$captureConfigBuilder.AppendLine()
        [void]$captureConfigBuilder.AppendLine("[Category.$categoryIndex.Item.$itemIndex]")
        [void]$captureConfigBuilder.AppendLine("Name=$(ConvertTo-ConfigValue $category.Targets[$itemIndex].Name)")
        [void]$captureConfigBuilder.AppendLine("Target=$(ConvertTo-ConfigValue $category.Targets[$itemIndex].Target)")
        [void]$captureConfigBuilder.AppendLine("Arguments=$(ConvertTo-ConfigValue '')")
        [void]$captureConfigBuilder.AppendLine("WorkingDirectory=$(ConvertTo-ConfigValue '')")
    }
}
[IO.File]::WriteAllText(
    $captureConfig, $captureConfigBuilder.ToString(), [Text.Encoding]::Unicode
)

$process = $null
$mainWindow = [IntPtr]::Zero
$backdrop = $null
try {
    $process = Start-Process -FilePath $Executable -ArgumentList @("--config", $quotedConfig) -PassThru -WindowStyle Normal
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 20
        $process.Refresh()
        if ($process.HasExited) {
            throw "LightLaunch exited before its main window appeared."
        }
        $mainWindow = [LightLaunchCaptureNative]::FindWindowForProcess(
            "LightLaunch.MainWindow", $process.Id
        )
        if ($mainWindow -ne [IntPtr]::Zero) { break }
    }
    if ($mainWindow -eq [IntPtr]::Zero) {
        throw "LightLaunch resident window did not become ready."
    }

    [LightLaunchCaptureNative]::PostMessage(
        $mainWindow, $activateMessage, [IntPtr]::Zero, [IntPtr]::Zero
    ) | Out-Null
    $showDeadline = [DateTime]::UtcNow.AddSeconds(2)
    while (-not [LightLaunchCaptureNative]::IsWindowVisible($mainWindow) -and
           [DateTime]::UtcNow -lt $showDeadline) {
        Start-Sleep -Milliseconds 20
    }
    if (-not [LightLaunchCaptureNative]::IsWindowVisible($mainWindow)) {
        throw "LightLaunch edge panel did not become visible."
    }

    $workingArea = [Windows.Forms.Screen]::FromHandle($mainWindow).WorkingArea
    $backdrop = [Windows.Forms.Form]::new()
    $backdrop.FormBorderStyle = [Windows.Forms.FormBorderStyle]::None
    $backdrop.StartPosition = [Windows.Forms.FormStartPosition]::Manual
    $backdrop.Bounds = $workingArea
    $backdrop.BackColor = [Drawing.Color]::FromArgb(232, 235, 240)
    $backdrop.ShowInTaskbar = $false
    $backdrop.TopMost = $true
    $backdrop.Show()
    [Windows.Forms.Application]::DoEvents()

    [LightLaunchCaptureNative]::PostMessage(
        $mainWindow, $activateMessage, [IntPtr]::Zero, [IntPtr]::Zero
    ) | Out-Null
    $pinButton = [LightLaunchCaptureNative]::GetDlgItem($mainWindow, 3001)
    $categoryList = [LightLaunchCaptureNative]::GetDlgItem($mainWindow, 1001)
    if ($pinButton -eq [IntPtr]::Zero -or $categoryList -eq [IntPtr]::Zero) {
        throw "LightLaunch rail controls were not found."
    }
    [LightLaunchCaptureNative]::SendMessage(
        $pinButton, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero
    ) | Out-Null
    [LightLaunchCaptureNative]::SendMessage(
        $categoryList, $LB_SETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero
    ) | Out-Null
    [LightLaunchCaptureNative]::SendMessage(
        $mainWindow, $WM_COMMAND, [IntPtr]$categorySelectionCommand, $categoryList
    ) | Out-Null

    $contentWindow = [IntPtr]::Zero
    $contentDeadline = [DateTime]::UtcNow.AddSeconds(2)
    do {
        $contentWindow = [LightLaunchCaptureNative]::FindWindowForProcess(
            "LightLaunch.ContentWindow", $process.Id
        )
        if ($contentWindow -ne [IntPtr]::Zero -and
            [LightLaunchCaptureNative]::IsWindowVisible($contentWindow)) {
            break
        }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $contentDeadline)
    if ($contentWindow -eq [IntPtr]::Zero -or
        -not [LightLaunchCaptureNative]::IsWindowVisible($contentWindow)) {
        throw "The centered content window did not become visible."
    }

    Start-Sleep -Milliseconds 500
    $bitmap = [Drawing.Bitmap]::new($workingArea.Width, $workingArea.Height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen(
            $workingArea.Location, [Drawing.Point]::Empty, $workingArea.Size
        )
        $bitmap.Save($Output, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }

    $railWindow = [LightLaunchCaptureNative]::FindWindowForProcess(
        "LightLaunch.RailInputWindow", $process.Id
    )
    $railRect = [LightLaunchCaptureNative+RECT]::new()
    if ($railWindow -eq [IntPtr]::Zero -or
        -not [LightLaunchCaptureNative]::GetWindowRect($railWindow, [ref]$railRect)) {
        throw "The visible rail bounds could not be resolved for the detail capture."
    }
    $railPadding = 28
    $railLeft = [math]::Max($workingArea.Left, $railRect.Left - $railPadding)
    $railTop = [math]::Max($workingArea.Top, $railRect.Top - $railPadding)
    $railRight = [math]::Min($workingArea.Right, $railRect.Right + $railPadding)
    $railBottom = [math]::Min($workingArea.Bottom, $railRect.Bottom + $railPadding)
    $railSize = [Drawing.Size]::new($railRight - $railLeft, $railBottom - $railTop)
    $railBitmap = [Drawing.Bitmap]::new($railSize.Width, $railSize.Height)
    $railGraphics = [Drawing.Graphics]::FromImage($railBitmap)
    try {
        $railGraphics.CopyFromScreen(
            [Drawing.Point]::new($railLeft, $railTop),
            [Drawing.Point]::Empty, $railSize
        )
        $railBitmap.Save($RailOutput, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $railGraphics.Dispose()
        $railBitmap.Dispose()
    }
    Write-Host "Captured: $Output"
    Write-Host "Rail detail: $RailOutput"
}
finally {
    if ($null -ne $backdrop) {
        $backdrop.Close()
        $backdrop.Dispose()
    }
    if ($null -ne $process -and -not $process.HasExited) {
        if ($mainWindow -ne [IntPtr]::Zero) {
            [LightLaunchCaptureNative]::PostMessage(
                $mainWindow, $WM_COMMAND,
                [IntPtr]$trayExitId, [IntPtr]::Zero
            ) | Out-Null
        }
        if (-not $process.WaitForExit(2000)) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit()
            throw "LightLaunch did not exit through the notification-area Exit command."
        }
    }

    if (Test-Path -LiteralPath $captureRoot) {
        $resolvedCaptureRoot = [IO.Path]::GetFullPath($captureRoot)
        if ($resolvedCaptureRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedCaptureRoot).StartsWith("LightLaunch-capture-")) {
            Remove-Item -LiteralPath $resolvedCaptureRoot -Recurse -Force
        }
    }
}
