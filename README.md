# ScreenShotPlus

Windows 10/11 的区域截图小工具：截图自动保存 PNG，拖动浮动缩略图即可向支持文件拖入的程序分享。

## 使用

1. 运行 `ScreenShotPlus.exe`。关闭操作面板后，程序仍在系统托盘运行；左击或右击托盘图标可打开面板。
2. 按 `Ctrl+Shift+S` 或点击“开始区域截图”。左键拖动框选，松开保存；Esc、右键或切换应用取消。
3. 从浮动预览的图片区域拖入文件夹、QQ、微信或浏览器附件区。接收程序可以将 PNG 当作图片或附件；原始截图始终保留。
4. 点击缩略图右上角关闭预览；操作面板的“显示最新截图”可重新显示本次运行中最新的截图。
5. 要结束程序，请在操作面板点击“退出程序”。重复启动会打开现有实例。

截图默认保存到系统“图片”目录的 `ScreenShotPlus` 子目录，文件名包含日期、时间、毫秒及必要的冲突序号。程序不会覆盖已有截图，也不会自动清理文件。

## 设置

- 保存目录：通过系统文件夹选择器更改，支持中文和空格。
- 快捷键：点击快捷键行，然后按 Ctrl / Alt / Shift 与字母、数字或 F1–F24 的组合。Esc 取消录入。
- PrintScreen：默认关闭，可与组合键同时启用。如仍触发系统截图，请关闭 Windows 设置中“使用 Print Screen 键打开屏幕捕获”的选项；其他软件占用时会提示注册失败。
- 缩略图：默认保留到关闭或下一次截图。点击切换为 5、10、15、30、60 或 120 秒；悬停、按住图片和拖拽期间暂停倒计时。
- 配置保存在 `%LOCALAPPDATA%\ScreenShotPlus\config.json`。点击“保存设置”才写入；无效或损坏的配置会提示并使用默认值。

截图写入失败时，图像会保留在内存中。更换保存目录并保存设置会自动重试，也可点击“重试保存未保存的截图”。退出程序或完成另一张截图会丢弃尚未保存的旧图像。

## 构建

需要 VS 2022 的 C++ 桌面开发工具、MSVC v143、Windows SDK，以及 vcpkg。默认从 `%USERPROFILE%\vcpkg` 查找，也可以设置 `VCPKG_ROOT` 或传入 MSBuild 属性 `VcpkgRoot`。

打开 `ScreenShotPlus.sln`，选择 **Release / x64** 并构建。项目通过固定 baseline 的 vcpkg manifest 自动获取 `nlohmann-json`，使用 `x64-windows-static` 与静态 CRT；截图、GDI+ 和 OLE 拖放使用系统库，不依赖 WinUI 3 或 .NET。

也可在仓库目录运行：

```powershell
.\tools\build.ps1
.\tools\build.ps1 -Test
.\tools\build.ps1 -Configuration Debug
.\tools\build.ps1 -Package
```

Release 输出为 `x64\Release\ScreenShotPlus.exe`。打包命令生成 `dist\ScreenShotPlus-x64.zip`，包含 EXE、说明和第三方版权声明；EXE 运行仅依赖 Windows 系统 DLL。

## 验证范围

独立测试工程 `tests\ScreenShotPlus.Tests.vcxproj` 覆盖区域坐标、配置读写和损坏回退、PNG 像素与尺寸、中文路径、文件名唯一性、真实文件权限拒绝、保存失败、OLE 文件数据、实际桌面裁剪、取消、GDI 资源释放、悬停/拖拽计时暂停、真实热键冲突与事务回退、应用内自动保存、设置持久化、重复触发、单实例和退出。首版实际结果见 [VERIFICATION.md](VERIFICATION.md)。

测试会短暂显示本程序的测试窗口，在 `build\test-artifacts` 保存自己的界面图像和测试文件。检测到现有运行实例时会明确打印 `SKIP`，跳过窗口测试；也可直接向测试 EXE 传入 `--no-ui`。QQ、微信、资源管理器、浏览器真实附件区、不同权限进程之间的拖入，以及混合 DPI 多屏仍需人工验收，模块测试不代表这些客户端已通过。

首版不包含标注、录屏、滚动截图、历史管理和开机自启。截屏使用桌面 GDI 捕获，受保护的视频内容、HDR 色彩还原及安全桌面不在首版支持范围内。
