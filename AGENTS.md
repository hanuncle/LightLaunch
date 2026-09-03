# LightLaunch 的 Codex 操作指南

这份文件用于让 Codex 在收到“新建分组”或“向分组添加内容”的请求时，快速确定目标、操作入口和安全边界。

## 用户可以这样下达请求

```text
在 LightLaunch 中新建分组“游戏开发”。
把 D:\Apps\Unity Hub\Unity Hub.exe 添加到“游戏开发”，显示名称为“Unity Hub”。
把 D:\Games 和 E:\Tools\VSCode.lnk 批量添加到“常用工具”。
```

用户最好提供分组名称和目标的完整路径；显示名称、启动参数和工作目录可以省略。

## 应用内操作

1. 启动 `LightLaunch.exe`，或单击托盘图标恢复已经运行的实例。
2. 将鼠标移到当前屏幕最右缘，显示分组 Dock。
3. 新建分组：右键 Dock 空白区域，选择“新建分组”，输入名称。
4. 打开分组：左键单击对应的 2×2 分组图标。
5. 添加文件：右键围栏内容区空白位置，选择添加文件；也可以从资源管理器拖入一个或多个文件。
6. 添加文件夹：右键围栏内容区空白位置，选择添加文件夹；也可以直接拖入文件夹。
7. 添加完成后只检查名称、图标和目标是否出现在正确分组；除非用户明确要求，不要双击启动目标。
8. 调整顺序：按住 Dock 中的分组图标拖到目标位置；在围栏中按住内容图标拖到另一个图标位置。松开后顺序会立即保存。

## Codex 批量修改配置时的规则

只有用户明确要求 Codex 代为修改 LightLaunch 数据时，才可以直接更新配置。默认配置文件为：

```text
%LOCALAPPDATA%\LightLaunch\config.ini
```

如果用户通过 `LightLaunch.exe --config <路径>` 使用独立配置，应操作用户指定的配置，不能同时修改默认配置。

执行顺序：

1. 使用只读命令确认每个目标路径存在，并解析为绝对路径；不要扫描用户未指定的磁盘或目录。
2. 检查 `LightLaunch.exe` 进程和 `<config>.lock`。程序仍在运行时不要编辑配置，也不要强制结束进程；请用户先从托盘选择“退出”。
3. 只读取完成本次操作所需的分组和项目字段，不在回复或日志中输出无关路径。
4. 修改前在同一目录创建带时间戳的备份，例如 `config.ini.codex-backup-20260903-120000`。
5. 写入同目录临时文件，完整校验后再原子替换正式配置；失败时保留原配置和备份。
6. 重新启动 LightLaunch，只验证分组数量、项目数量、显示名称和目标归属，不自动启动新加入的目标。

任何“删除分组”“移除项目”“覆盖同名分组”都不属于添加操作，必须得到用户单独、明确的授权。LightLaunch 配置只保存目标引用；不要移动、重命名或删除用户的原始应用、文件、快捷方式和文件夹。

## 配置格式速查

- 文件编码：带 BOM 的 UTF-16 LE。
- 当前格式：`SchemaVersion=2`。
- Dock 外观保存在 `[General]` 的 `DockBackgroundColor`、`DockTransparency` 和 `DockBorderColor` 字段中。
- 分组数量上限：256；每个分组的项目数量上限：4096。
- 分组节连续编号：`[Category.0]`、`[Category.1]`……
- 项目节连续编号：`[Category.0.Item.0]`、`[Category.0.Item.1]`……
- `Name`、`Target`、`Arguments`、`WorkingDirectory` 和 `BackgroundImage` 使用 `b64:` 加 UTF-16 LE 字节的 Base64。
- 同一分组内，目标路径按不区分大小写的方式去重。

PowerShell 编码函数：

```powershell
function ConvertTo-LightLaunchValue([string]$Value) {
    $bytes = [Text.Encoding]::Unicode.GetBytes($Value)
    return "b64:" + [Convert]::ToBase64String($bytes)
}
```

Dock 默认外观字段：

```ini
DockBackgroundColor=16054004
DockTransparency=53
DockBorderColor=14343640
```

新分组的默认字段：

```ini
[Category.N]
Name=b64:<UTF-16LE-Base64>
BackgroundImage=b64:
BackgroundMode=0
BackgroundOpacity=45
BackgroundCropX=5000
BackgroundCropY=5000
BackgroundColor=15198950
FenceTransparency=31
FenceBorderColor=16777215
ItemCount=0
```

添加分组时，将 `N` 设为修改前的 `CategoryCount`，添加上述新节，然后将 `CategoryCount` 加一。除非用户要求切换当前分组，否则保留 `SelectedCategory`。

新项目字段：

```ini
[Category.N.Item.M]
Name=b64:<UTF-16LE-Base64>
Target=b64:<UTF-16LE-Base64>
Arguments=b64:
WorkingDirectory=b64:<UTF-16LE-Base64>
```

添加项目时，先按分组名称找到 `N`，将 `M` 设为该分组修改前的 `ItemCount`，添加新项目节，然后将该分组的 `ItemCount` 加一。默认显示名称使用文件名或文件夹名；默认工作目录使用目标所在目录，文件夹目标则使用该文件夹自身；默认参数为空。

## 完成后的最小报告

Codex 完成操作后只需报告：

- 修改的是默认配置还是独立配置；
- 新增的分组名称；
- 每个分组新增了多少个项目；
- 被跳过的无效路径或重复项目；
- 备份文件的位置；
- 是否已重新启动并完成只读核对。
