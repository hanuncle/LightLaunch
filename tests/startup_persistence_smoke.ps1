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
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class LightLaunchPersistenceNative
{
    public delegate bool EnumChildProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr window, int controlId);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumChildProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumChildProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern int GetDlgCtrlID(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder className, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr window, StringBuilder text, int maximumCount);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    public static extern IntPtr SendMessageText(IntPtr window, uint message, IntPtr wParam, string text);

    [DllImport("user32.dll", EntryPoint = "SendMessageW")]
    public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);

    public static IntPtr FindDescendantById(IntPtr parent, int controlId)
    {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, delegate(IntPtr window, IntPtr parameter)
        {
            if (GetDlgCtrlID(window) == controlId)
            {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static IntPtr FindFirstButton(IntPtr parent)
    {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, delegate(IntPtr window, IntPtr parameter)
        {
            StringBuilder className = new StringBuilder(32);
            GetClassName(window, className, className.Capacity);
            if (String.Equals(className.ToString(), "Button", StringComparison.Ordinal))
            {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static string DescribeDescendants(IntPtr parent)
    {
        List<string> descriptions = new List<string>();
        EnumChildWindows(parent, delegate(IntPtr window, IntPtr parameter)
        {
            StringBuilder className = new StringBuilder(128);
            StringBuilder text = new StringBuilder(256);
            GetClassName(window, className, className.Capacity);
            GetWindowText(window, text, text.Capacity);
            descriptions.Add(String.Format("id={0},class={1},text={2}",
                GetDlgCtrlID(window), className, text));
            return true;
        }, IntPtr.Zero);
        return String.Join(" | ", descriptions.ToArray());
    }

    public static string DescribeTopWindows(uint ownerProcessId)
    {
        List<string> descriptions = new List<string>();
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId == ownerProcessId)
            {
                StringBuilder className = new StringBuilder(128);
                StringBuilder text = new StringBuilder(256);
                GetClassName(window, className, className.Capacity);
                GetWindowText(window, text, text.Capacity);
                descriptions.Add(String.Format("class={0},text={1}", className, text));
            }
            return true;
        }, IntPtr.Zero);
        return String.Join(" | ", descriptions.ToArray());
    }

    public static IntPtr FindWindowForProcess(string expectedClass, string expectedTitle,
                                              uint ownerProcessId)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId != ownerProcessId)
            {
                return true;
            }
            StringBuilder className = new StringBuilder(128);
            StringBuilder title = new StringBuilder(256);
            GetClassName(window, className, className.Capacity);
            GetWindowText(window, title, title.Capacity);
            if (String.Equals(className.ToString(), expectedClass, StringComparison.Ordinal) &&
                (String.IsNullOrEmpty(expectedTitle) ||
                 String.Equals(title.ToString(), expectedTitle, StringComparison.Ordinal)))
            {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$WM_SETTEXT = 0x000C
$BM_CLICK = 0x00F5
$LB_GETCOUNT = 0x018B
$addCategoryId = 1002
$trayExitId = 5002
$IDOK = 1
$IDYES = 6
$IDNO = 7

function Chars([int[]]$CodePoints) {
    return -join @($CodePoints | ForEach-Object { [char]$_ })
}

$createConfigTitle = Chars @(0x521B, 0x5EFA, 0x65B0, 0x914D, 0x7F6E)
$readFailureTitle = Chars @(0x914D, 0x7F6E, 0x8BFB, 0x53D6, 0x5931, 0x8D25)
$backupRecoveryTitle = Chars @(0x5DF2, 0x4ECE, 0x914D, 0x7F6E, 0x5907, 0x4EFD, 0x6062, 0x590D)
$newCategoryTitle = Chars @(0x65B0, 0x5EFA, 0x5206, 0x7EC4)
$confirmRecoveryTitle = Chars @(0x786E, 0x8BA4, 0x6062, 0x590D, 0x914D, 0x7F6E)
$recoveryCompleteTitle = Chars @(0x914D, 0x7F6E, 0x6062, 0x590D, 0x5B8C, 0x6210)

function Wait-ForWindow(
    [string]$ClassName,
    [string]$Title,
    [Diagnostics.Process]$OwnerProcess,
    [int]$TimeoutMs = 5000
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        $OwnerProcess.Refresh()
        if ($OwnerProcess.HasExited) {
            throw "LightLaunch exited before window appeared: $ClassName / $Title"
        }
        $window = [LightLaunchPersistenceNative]::FindWindowForProcess(
            $ClassName, $Title, [uint32]$OwnerProcess.Id
        )
        if ($window -ne [IntPtr]::Zero) {
            return $window
        }
        Start-Sleep -Milliseconds 20
    }
    $windows = [LightLaunchPersistenceNative]::DescribeTopWindows(
        [uint32]$OwnerProcess.Id
    )
    throw "Window did not appear: $ClassName / $Title. Owned windows: $windows"
}

function Wait-ForExit(
    [Diagnostics.Process]$Process,
    [int]$ExpectedExitCode,
    [int]$TimeoutMs = 5000
) {
    if (-not $Process.WaitForExit($TimeoutMs)) {
        throw "LightLaunch did not exit within ${TimeoutMs}ms."
    }
    if ($Process.ExitCode -ne $ExpectedExitCode) {
        throw "LightLaunch exit code was $($Process.ExitCode), expected $ExpectedExitCode."
    }
}

function Wait-ForWindowGone(
    [string]$ClassName,
    [string]$Title,
    [Diagnostics.Process]$OwnerProcess,
    [int]$TimeoutMs = 3000
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        $window = [LightLaunchPersistenceNative]::FindWindowForProcess(
            $ClassName, $Title, [uint32]$OwnerProcess.Id
        )
        if ($window -eq [IntPtr]::Zero) {
            return
        }
        Start-Sleep -Milliseconds 20
    }
    throw "Window remained open: $ClassName / $Title"
}

function Wait-ForWindowHidden([IntPtr]$Window, [int]$TimeoutMs = 3000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        if (-not [LightLaunchPersistenceNative]::IsWindowVisible($Window)) {
            return
        }
        Start-Sleep -Milliseconds 20
    }
    throw "Window remained visible after WM_CLOSE."
}

function Click-DialogButton([IntPtr]$Dialog, [int]$ControlId, [string]$Description) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt 3000) {
        $button = [LightLaunchPersistenceNative]::FindDescendantById($Dialog, $ControlId)
        if ($button -ne [IntPtr]::Zero) {
            [void][LightLaunchPersistenceNative]::SendMessage(
                $button, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero
            )
            return
        }
        Start-Sleep -Milliseconds 20
    }
    $children = [LightLaunchPersistenceNative]::DescribeDescendants($Dialog)
    throw "$Description button was not found. Children: $children"
}

function Click-OnlyDialogButton([IntPtr]$Dialog, [string]$Description) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt 3000) {
        $button = [LightLaunchPersistenceNative]::FindFirstButton($Dialog)
        if ($button -ne [IntPtr]::Zero) {
            [void][LightLaunchPersistenceNative]::SendMessage(
                $button, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero
            )
            return
        }
        Start-Sleep -Milliseconds 20
    }
    throw "$Description button was not found."
}

function Wait-ForDialogControl(
    [IntPtr]$Dialog,
    [int]$ControlId,
    [string]$Description,
    [int]$TimeoutMs = 3000
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        $control = [LightLaunchPersistenceNative]::FindDescendantById(
            $Dialog, $ControlId
        )
        if ($control -ne [IntPtr]::Zero) {
            return $control
        }
        Start-Sleep -Milliseconds 20
    }
    $children = [LightLaunchPersistenceNative]::DescribeDescendants($Dialog)
    throw "$Description control was not found. Children: $children"
}

function File-Fingerprint([string]$Path) {
    return [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path))
}

function ConvertTo-ConfigValue([string]$Value) {
    return "b64:" + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Value))
}

function Start-WithConfig([string]$ConfigPath) {
    $quotedConfig = '"' + $ConfigPath + '"'
    return Start-Process -FilePath $Executable `
        -ArgumentList @("--config", $quotedConfig) -PassThru -WindowStyle Normal
}

function Assert-NoConfigWrites(
    [string]$ConfigPath,
    [string]$MainFingerprint,
    [string]$BackupFingerprint,
    [string[]]$AllowedExtraNames = @()
) {
    if ((File-Fingerprint $ConfigPath) -ne $MainFingerprint -or
        (File-Fingerprint ($ConfigPath + ".bak")) -ne $BackupFingerprint -or
        (Test-Path -LiteralPath ($ConfigPath + ".tmp")) -or
        (Test-Path -LiteralPath ($ConfigPath + ".bak.tmp"))) {
        throw "A guarded startup path modified configuration data."
    }
    $configName = Split-Path -Leaf $ConfigPath
    $allowedNames = @($configName, ($configName + ".bak")) +
                    @($AllowedExtraNames)
    $unexpected = @(
        Get-ChildItem -LiteralPath (Split-Path -Parent $ConfigPath) -File |
            Where-Object { $_.Name -notin $allowedNames }
    )
    if ($unexpected.Count -ne 0) {
        throw "A guarded startup path created unexpected files: $($unexpected.Name -join ', ')"
    }
}

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = [IO.Path]::GetFullPath(
    (Join-Path $temporaryRoot ("LightLaunch-persistence-smoke-" + [Guid]::NewGuid().ToString("N")))
)
if (-not $testRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to create a persistence-test directory outside the system temp directory."
}

New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$activeProcess = $null
try {
    $missingDirectory = Join-Path $testRoot "missing"
    New-Item -ItemType Directory -Path $missingDirectory | Out-Null
    $missingConfig = Join-Path $missingDirectory "config.ini"
    $activeProcess = Start-WithConfig $missingConfig
    $dialog = Wait-ForWindow "#32770" $createConfigTitle $activeProcess
    Click-DialogButton $dialog $IDNO "Create-config refusal"
    Wait-ForExit $activeProcess 0
    $activeProcess = $null
    if ((Test-Path -LiteralPath $missingConfig) -or
        (Test-Path -LiteralPath ($missingConfig + ".bak")) -or
        (Test-Path -LiteralPath ($missingConfig + ".tmp"))) {
        throw "Refusing new configuration still created a config file."
    }

    $invalidDirectory = Join-Path $testRoot "invalid"
    New-Item -ItemType Directory -Path $invalidDirectory | Out-Null
    $invalidConfig = Join-Path $invalidDirectory "config.ini"
    $invalidText = "[General]`r`nSchemaVersion=2`r`nCategoryCount=1`r`nSelectedCategory=0`r`n"
    [IO.File]::WriteAllText($invalidConfig, $invalidText, [Text.Encoding]::Unicode)
    [IO.File]::WriteAllText($invalidConfig + ".bak", $invalidText, [Text.Encoding]::Unicode)
    $invalidMainFingerprint = File-Fingerprint $invalidConfig
    $invalidBackupFingerprint = File-Fingerprint ($invalidConfig + ".bak")
    $activeProcess = Start-WithConfig $invalidConfig
    $dialog = Wait-ForWindow "#32770" $readFailureTitle $activeProcess
    if ([LightLaunchPersistenceNative]::FindWindowForProcess(
            "LightLaunch.MainWindow", "", [uint32]$activeProcess.Id
        ) -ne [IntPtr]::Zero) {
        throw "Main window was created after both configuration files failed validation."
    }
    Click-OnlyDialogButton $dialog "Read-failure acknowledgement"
    Wait-ForExit $activeProcess 1
    $activeProcess = $null
    Assert-NoConfigWrites $invalidConfig $invalidMainFingerprint $invalidBackupFingerprint

    $recoveryDirectory = Join-Path $testRoot "recovery"
    New-Item -ItemType Directory -Path $recoveryDirectory | Out-Null
    $recoveryConfig = Join-Path $recoveryDirectory "config.ini"
    $validBackup = "[General]`r`n" +
                   "SchemaVersion=2`r`n" +
                   "CategoryCount=1`r`n" +
                   "SelectedCategory=0`r`n`r`n" +
                   "[Category.0]`r`n" +
                   "Name=$(ConvertTo-ConfigValue "Recovery Probe")`r`n" +
                   "ItemCount=0`r`n"
    [IO.File]::WriteAllText($recoveryConfig, $invalidText, [Text.Encoding]::Unicode)
    [IO.File]::WriteAllText($recoveryConfig + ".bak", $validBackup, [Text.Encoding]::Unicode)
    $recoveryMainFingerprint = File-Fingerprint $recoveryConfig
    $recoveryBackupFingerprint = File-Fingerprint ($recoveryConfig + ".bak")
    $activeProcess = Start-WithConfig $recoveryConfig
    $dialog = Wait-ForWindow "#32770" $backupRecoveryTitle $activeProcess
    Click-OnlyDialogButton $dialog "Backup-recovery acknowledgement"
    $mainWindow = Wait-ForWindow "LightLaunch.MainWindow" $null $activeProcess
    $categoryList = [LightLaunchPersistenceNative]::GetDlgItem($mainWindow, 1001)
    if ($categoryList -eq [IntPtr]::Zero -or
        [LightLaunchPersistenceNative]::SendMessage(
            $categoryList, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
        ).ToInt32() -ne 1) {
        throw "Backup state was not loaded into the group list."
    }

    [void][LightLaunchPersistenceNative]::PostMessage(
        $mainWindow, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero
    )
    Wait-ForWindowHidden $mainWindow
    Assert-NoConfigWrites $recoveryConfig $recoveryMainFingerprint $recoveryBackupFingerprint

    [void][LightLaunchPersistenceNative]::PostMessage(
        $mainWindow, $WM_COMMAND, [IntPtr]$addCategoryId, [IntPtr]::Zero
    )
    $inputDialog = Wait-ForWindow "LightLaunch.InputDialog" $newCategoryTitle $activeProcess
    $edit = Wait-ForDialogControl $inputDialog 100 "New-category edit"
    $okButton = Wait-ForDialogControl $inputDialog $IDOK "New-category OK"
    [void][LightLaunchPersistenceNative]::SendMessageText(
        $edit, $WM_SETTEXT, [IntPtr]::Zero, "Recovery Reject"
    )
    [void][LightLaunchPersistenceNative]::PostMessage(
        $okButton, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero
    )
    $dialog = Wait-ForWindow "#32770" $confirmRecoveryTitle $activeProcess
    Click-DialogButton $dialog $IDNO "Recovery-save refusal"
    Wait-ForWindowGone "#32770" $confirmRecoveryTitle $activeProcess
    if ([LightLaunchPersistenceNative]::SendMessage(
            $categoryList, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero
        ).ToInt32() -ne 1) {
        throw "Refusing recovered-state save did not roll back the UI change."
    }
    Assert-NoConfigWrites $recoveryConfig $recoveryMainFingerprint $recoveryBackupFingerprint

    [void][LightLaunchPersistenceNative]::PostMessage(
        $mainWindow, $WM_COMMAND, [IntPtr]$addCategoryId, [IntPtr]::Zero
    )
    $inputDialog = Wait-ForWindow "LightLaunch.InputDialog" $newCategoryTitle $activeProcess
    $edit = Wait-ForDialogControl $inputDialog 100 "Accepted new-category edit"
    $okButton = Wait-ForDialogControl $inputDialog $IDOK "Accepted new-category OK"
    [void][LightLaunchPersistenceNative]::SendMessageText(
        $edit, $WM_SETTEXT, [IntPtr]::Zero, "Recovery Accept"
    )
    [void][LightLaunchPersistenceNative]::PostMessage(
        $okButton, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero
    )
    $dialog = Wait-ForWindow "#32770" $confirmRecoveryTitle $activeProcess
    Click-DialogButton $dialog $IDYES "Recovery-save confirmation"
    $dialog = Wait-ForWindow "#32770" $recoveryCompleteTitle $activeProcess
    Click-OnlyDialogButton $dialog "Recovery completion acknowledgement"

    $recoveredText = [IO.File]::ReadAllText($recoveryConfig, [Text.Encoding]::Unicode)
    if (-not $recoveredText.Contains("CategoryCount=2") -or
        -not $recoveredText.Contains((ConvertTo-ConfigValue "Recovery Probe")) -or
        -not $recoveredText.Contains((ConvertTo-ConfigValue "Recovery Accept"))) {
        throw "Confirmed recovery did not persist the current state."
    }
    if ((File-Fingerprint ($recoveryConfig + ".bak")) -ne
        $recoveryBackupFingerprint) {
        throw "Confirmed recovery replaced the known-good backup."
    }
    $preservedPrimaries = @(
        Get-ChildItem -LiteralPath $recoveryDirectory -File |
            Where-Object { $_.Name -like "config.ini.pre-recovery-*.bak" }
    )
    if ($preservedPrimaries.Count -ne 1 -or
        (File-Fingerprint $preservedPrimaries[0].FullName) -ne
            $recoveryMainFingerprint) {
        throw "Confirmed recovery did not preserve the original main config."
    }
    $recoveredMainFingerprint = File-Fingerprint $recoveryConfig

    [void][LightLaunchPersistenceNative]::PostMessage(
        $mainWindow, $WM_COMMAND, [IntPtr]$trayExitId, [IntPtr]::Zero
    )
    Wait-ForExit $activeProcess 0
    $activeProcess = $null
    Assert-NoConfigWrites $recoveryConfig $recoveredMainFingerprint `
        $recoveryBackupFingerprint @($preservedPrimaries[0].Name)

    Write-Host "Startup persistence smoke test passed."
}
finally {
    if ($null -ne $activeProcess) {
        $activeProcess.Refresh()
        if (-not $activeProcess.HasExited) {
            Stop-Process -Id $activeProcess.Id -Force
            $activeProcess.WaitForExit()
        }
    }

    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
        if ($resolvedTestRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTestRoot).StartsWith("LightLaunch-persistence-smoke-")) {
            Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
        }
    }
}
