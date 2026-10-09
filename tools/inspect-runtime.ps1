param([string]$ExecutablePath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'x64\Release\ScreenShotPlus.exe'))
$ErrorActionPreference = 'Stop'
# Read-only inspection. Never changes process state, ACLs or integrity labels.
if (!('SspRuntime' -as [type])) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class SspRuntime {
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
 [DllImport("advapi32.dll",SetLastError=true)] static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
 [DllImport("advapi32.dll",SetLastError=true)] static extern bool GetTokenInformation(IntPtr token,int field,IntPtr buffer,int size,out int used);
 public static string Integrity(uint pid) {
  var process=OpenProcess(0x1000,false,pid); if(process==IntPtr.Zero) return "Unknown (OpenProcess error "+Marshal.GetLastWin32Error()+")";
  IntPtr token=IntPtr.Zero, buffer=IntPtr.Zero;
  try {
   if(!OpenProcessToken(process,8,out token)) return "Unknown (OpenProcessToken error "+Marshal.GetLastWin32Error()+")";
   buffer=Marshal.AllocHGlobal(512); int used;
   if(!GetTokenInformation(token,25,buffer,512,out used)) return "Unknown (GetTokenInformation error "+Marshal.GetLastWin32Error()+")";
   var sid=Marshal.ReadIntPtr(buffer); int count=Marshal.ReadByte(sid,1);
   if(count<1 || count>15) return "Unknown (invalid integrity SID)";
   int rid=Marshal.ReadInt32(sid,8+(count-1)*4);
   return rid+" ("+(rid<8192?"Low or lower":rid<12288?"Medium":rid<16384?"High":"System or higher")+")";
  } finally {if(buffer!=IntPtr.Zero) Marshal.FreeHGlobal(buffer); if(token!=IntPtr.Zero) CloseHandle(token); CloseHandle(process);}
 }
 public delegate bool Callback(IntPtr hwnd,IntPtr param);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback callback,IntPtr param);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd,StringBuilder text,int size);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr GetPropW(IntPtr hwnd,string name);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
 [StructLayout(LayoutKind.Sequential)] public struct Id {public uint size; public IntPtr hwnd; public uint uid; public Guid guid;}
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int left,top,right,bottom;}
 [DllImport("shell32.dll")] public static extern int Shell_NotifyIconGetRect(ref Id id,out Rect rect);
}
'@
}
$taskExe = Get-Item -LiteralPath $ExecutablePath
Write-Output "Target=$($taskExe.FullName) Version=$($taskExe.VersionInfo.FileVersion)"
& icacls $taskExe.FullName
if ($LASTEXITCODE -ne 0) { throw '无法读取 EXE 权限。' }
$taskApps = @(Get-CimInstance Win32_Process -Filter "Name='ScreenShotPlus.exe'")
$taskExplorers = @(Get-CimInstance Win32_Process -Filter "Name='explorer.exe'")
foreach ($taskProcess in @($taskApps) + @($taskExplorers)) {
    Write-Output "PID=$($taskProcess.ProcessId) Parent=$($taskProcess.ParentProcessId) Session=$($taskProcess.SessionId) Integrity=$([SspRuntime]::Integrity($taskProcess.ProcessId)) EXE=$($taskProcess.ExecutablePath)"
}
if (!$taskApps.Count) { Write-Output 'No running ScreenShotPlus instance; runtime tray state not verified.' }
[SspRuntime]::EnumWindows({ param($taskWindow, $taskParam)
    $taskWindowPid = [uint32]0
    [SspRuntime]::GetWindowThreadProcessId($taskWindow, [ref]$taskWindowPid) | Out-Null
    if ($taskApps.ProcessId -contains $taskWindowPid) {
        $taskClass = [Text.StringBuilder]::new(128)
        [SspRuntime]::GetClassName($taskWindow, $taskClass, 128) | Out-Null
        if ($taskClass.ToString() -eq 'ScreenShotPlus.Panel') {
            $taskId = [SspRuntime+Id]::new(); $taskId.size = [Runtime.InteropServices.Marshal]::SizeOf($taskId)
            $taskId.hwnd = $taskWindow; $taskId.uid = 1
            $taskRect = [SspRuntime+Rect]::new()
            $taskResult = [SspRuntime]::Shell_NotifyIconGetRect([ref]$taskId, [ref]$taskRect)
            $taskRegistered = [SspRuntime]::GetPropW($taskWindow, 'ScreenShotPlus.TrayRegistered') -ne [IntPtr]::Zero
            Write-Host "Panel PID=$taskWindowPid HWND=$taskWindow Visible=$([SspRuntime]::IsWindowVisible($taskWindow)) Registered=$taskRegistered GetRect=0x$([Convert]::ToString($taskResult,16)) Rect=$($taskRect.left),$($taskRect.top),$($taskRect.right),$($taskRect.bottom)"
        }
    }
    return $true
}, [IntPtr]::Zero) | Out-Null
