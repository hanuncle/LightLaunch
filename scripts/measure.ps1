[CmdletBinding()]
param(
    [string]$Executable = "",
    [ValidateRange(1, 30)]
    [int]$Runs = 7,
    [ValidateRange(1, 20)]
    [int]$Categories = 20,
    [ValidateRange(0, 500)]
    [int]$Items = 500,
    [switch]$Tray,
    [switch]$OpenFirstGroup,
    [ValidateRange(250, 5000)]
    [int]$SampleMilliseconds = 1000
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

public static class LightLaunchMeasureNative
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder className,
                                          int maximumCount);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message,
                                          IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);

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

$WM_COMMAND = 0x0111
$LB_SETCURSEL = 0x0186
$categorySelectionCommand = (1 -shl 16) -bor 1001
$activateMessage = 0x8003
$trayExitId = 5002

function ConvertTo-ConfigValue([string]$Value) {
    return "b64:" + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Value))
}

function Write-MeasurementConfig([string]$Path, [int]$CategoryCount, [int]$ItemCount) {
    $builder = [Text.StringBuilder]::new()
    $emptyValue = ConvertTo-ConfigValue ""
    [void]$builder.AppendLine("[General]")
    [void]$builder.AppendLine("SchemaVersion=2")
    [void]$builder.AppendLine("CategoryCount=$CategoryCount")
    [void]$builder.AppendLine("SelectedCategory=0")

    for ($categoryIndex = 0; $categoryIndex -lt $CategoryCount; $categoryIndex++) {
        [void]$builder.AppendLine()
        [void]$builder.AppendLine("[Category.$categoryIndex]")
        [void]$builder.AppendLine("Name=$(ConvertTo-ConfigValue ("Benchmark " + ($categoryIndex + 1)))")
        $itemsInCategory = if ($categoryIndex -eq 0) { $ItemCount } else { 0 }
        [void]$builder.AppendLine("ItemCount=$itemsInCategory")

        for ($itemIndex = 0; $itemIndex -lt $itemsInCategory; $itemIndex++) {
            $target = if ($itemIndex -eq 0) {
                "\\LightLaunch-Disconnected\offline\probe.exe"
            }
            else {
                "C:\LightLaunch-Benchmark\App-$itemIndex.exe"
            }
            [void]$builder.AppendLine()
            [void]$builder.AppendLine("[Category.$categoryIndex.Item.$itemIndex]")
            [void]$builder.AppendLine("Name=$(ConvertTo-ConfigValue ("Benchmark App " + ($itemIndex + 1)))")
            [void]$builder.AppendLine("Target=$(ConvertTo-ConfigValue $target)")
            [void]$builder.AppendLine("Arguments=$emptyValue")
            [void]$builder.AppendLine("WorkingDirectory=$emptyValue")
        }
    }

    [IO.File]::WriteAllText($Path, $builder.ToString(), [Text.Encoding]::Unicode)
}

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$measureRoot = [IO.Path]::GetFullPath(
    (Join-Path $temporaryRoot ("LightLaunch-measure-" + [Guid]::NewGuid().ToString("N")))
)
if (-not $measureRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to create a measurement directory outside the system temporary directory."
}
New-Item -ItemType Directory -Path $measureRoot -Force | Out-Null
$measureConfig = Join-Path $measureRoot "config.ini"
$quotedConfig = '"' + $measureConfig + '"'
Write-MeasurementConfig $measureConfig $Categories $Items

$samples = @()
try {
    for ($index = 1; $index -le $Runs; $index++) {
        $process = $null
        $mainWindow = [IntPtr]::Zero
        try {
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $process = Start-Process -FilePath $Executable -ArgumentList @("--config", $quotedConfig) -PassThru -WindowStyle Normal
            while ($watch.ElapsedMilliseconds -lt 3000) {
                Start-Sleep -Milliseconds 10
                $process.Refresh()
                if ($process.HasExited) {
                    throw "LightLaunch exited before its resident window was ready."
                }
                $mainWindow = [LightLaunchMeasureNative]::FindWindowForProcess(
                    "LightLaunch.MainWindow", $process.Id
                )
                if ($mainWindow -ne [IntPtr]::Zero) {
                    break
                }
            }
            if ($mainWindow -eq [IntPtr]::Zero) {
                throw "LightLaunch resident window was not ready within 3000 ms."
            }

            if (-not $Tray) {
                [LightLaunchMeasureNative]::PostMessage(
                    $mainWindow, $activateMessage, [IntPtr]::Zero, [IntPtr]::Zero
                ) | Out-Null
                $showDeadline = [DateTime]::UtcNow.AddSeconds(2)
                while (-not [LightLaunchMeasureNative]::IsWindowVisible($mainWindow) -and
                       [DateTime]::UtcNow -lt $showDeadline) {
                    Start-Sleep -Milliseconds 10
                }
                if (-not [LightLaunchMeasureNative]::IsWindowVisible($mainWindow)) {
                    throw "LightLaunch panel did not become visible."
                }
                if ($OpenFirstGroup) {
                    $categoryList = [LightLaunchMeasureNative]::GetDlgItem($mainWindow, 1001)
                    if ($categoryList -eq [IntPtr]::Zero) {
                        throw "LightLaunch category control was not found."
                    }
                    [LightLaunchMeasureNative]::SendMessage(
                        $categoryList, $LB_SETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero
                    ) | Out-Null
                    [LightLaunchMeasureNative]::SendMessage(
                        $mainWindow, $WM_COMMAND,
                        [IntPtr]$categorySelectionCommand, $categoryList
                    ) | Out-Null
                    $contentDeadline = [DateTime]::UtcNow.AddSeconds(2)
                    do {
                        $contentWindow = [LightLaunchMeasureNative]::FindWindowForProcess(
                            "LightLaunch.ContentWindow", $process.Id
                        )
                        if ($contentWindow -ne [IntPtr]::Zero -and
                            [LightLaunchMeasureNative]::IsWindowVisible($contentWindow)) {
                            break
                        }
                        Start-Sleep -Milliseconds 10
                    } while ([DateTime]::UtcNow -lt $contentDeadline)
                    if ($contentWindow -eq [IntPtr]::Zero -or
                        -not [LightLaunchMeasureNative]::IsWindowVisible($contentWindow)) {
                        throw "LightLaunch centered content window did not become visible."
                    }
                }
            }
            $readyMilliseconds = $watch.ElapsedMilliseconds
            $process.Refresh()
            $processorStartMilliseconds = $process.TotalProcessorTime.TotalMilliseconds
            Start-Sleep -Milliseconds $SampleMilliseconds
            $process.Refresh()
            $processorDeltaMilliseconds =
                $process.TotalProcessorTime.TotalMilliseconds - $processorStartMilliseconds
            $normalizedCpuPercent = [math]::Round(
                ($processorDeltaMilliseconds /
                    ($SampleMilliseconds * [Environment]::ProcessorCount)) * 100,
                3
            )
            if ($process.HasExited) {
                throw "LightLaunch exited before the memory sample was collected."
            }
            $samples += [PSCustomObject]@{
                Run = $index
                ReadyMs = $readyMilliseconds
                WorkingSetMiB = [math]::Round($process.WorkingSet64 / 1MB, 2)
                PrivateMemoryMiB = [math]::Round($process.PrivateMemorySize64 / 1MB, 2)
                NormalizedCpuPercent = $normalizedCpuPercent
                Handles = $process.HandleCount
            }
        }
        finally {
            if ($null -ne $process -and -not $process.HasExited) {
                if ($mainWindow -ne [IntPtr]::Zero) {
                    [LightLaunchMeasureNative]::PostMessage(
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
        }
    }
}
finally {
    if (Test-Path -LiteralPath $measureRoot) {
        $resolvedMeasureRoot = [IO.Path]::GetFullPath($measureRoot)
        if ($resolvedMeasureRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedMeasureRoot).StartsWith("LightLaunch-measure-")) {
            Remove-Item -LiteralPath $resolvedMeasureRoot -Recurse -Force
        }
    }
}

$ordered = @($samples.ReadyMs | Sort-Object)
if (($ordered.Count % 2) -eq 1) {
    $median = $ordered[[math]::Floor($ordered.Count / 2)]
}
else {
    $upperIndex = $ordered.Count / 2
    $median = [math]::Round(($ordered[$upperIndex - 1] + $ordered[$upperIndex]) / 2, 1)
}
$p95Index = [math]::Min($ordered.Count - 1, [math]::Ceiling($ordered.Count * 0.95) - 1)
Write-Host ($samples | Format-Table -AutoSize | Out-String)
Write-Host "Median ready time : $median ms"
Write-Host "P95 ready time    : $($ordered[$p95Index]) ms"
Write-Host "Max sampled WS    : $([math]::Round(($samples.WorkingSetMiB | Measure-Object -Maximum).Maximum, 2)) MiB"
Write-Host "Max sampled private: $([math]::Round(($samples.PrivateMemoryMiB | Measure-Object -Maximum).Maximum, 2)) MiB"
Write-Host "Max normalized CPU : $([math]::Round(($samples.NormalizedCpuPercent | Measure-Object -Maximum).Maximum, 3)) % over ${SampleMilliseconds}ms"
Write-Host "Fixture           : $Categories categories, $Items items (all items in selected category)"
Write-Host "Sample mode       : $(if ($Tray) { 'resident background' } elseif ($OpenFirstGroup) { 'transparent rail with centered group window' } else { 'visible transparent group rail' })"
