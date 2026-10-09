# Independent Shell probe; only creates and removes its own diagnostic icon.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TrayProbe {
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] public struct Data {
  public uint cbSize; public IntPtr hwnd; public uint id, flags, message; public IntPtr icon;
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=128)] public string tip;
  public uint state,mask;
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)] public string info;
  public uint version;
  [MarshalAs(UnmanagedType.ByValTStr,SizeConst=64)] public string title;
  public uint infoFlags; public Guid guid; public IntPtr balloon;
 }
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern IntPtr CreateWindowExW(uint ex,string cls,string title,uint style,int x,int y,int w,int h,IntPtr parent,IntPtr menu,IntPtr inst,IntPtr param);
 [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr hwnd);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls,string title);
 [DllImport("user32.dll",SetLastError=true)] public static extern IntPtr SendMessageTimeoutW(IntPtr hwnd,uint msg,IntPtr wp,IntPtr lp,uint flags,uint timeout,out IntPtr result);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
 [DllImport("user32.dll")] public static extern IntPtr LoadIconW(IntPtr inst,IntPtr name);
 [DllImport("kernel32.dll")] public static extern void SetLastError(uint error);
 [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
 [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
 [DllImport("advapi32.dll")] public static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
 [DllImport("advapi32.dll")] public static extern bool GetTokenInformation(IntPtr token,int field,IntPtr buffer,int size,out int used);
 [StructLayout(LayoutKind.Sequential)] public struct Id { public uint size; public IntPtr hwnd; public uint id; public Guid guid; }
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left,top,right,bottom; }
 [DllImport("shell32.dll")] public static extern int Shell_NotifyIconGetRect(ref Id id,out Rect rect);
 [DllImport("shell32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool Shell_NotifyIconW(uint op,ref Data data);
 static int Integrity(IntPtr process) {
  IntPtr token; if(!OpenProcessToken(process,8,out token)) return -1;
  var buffer=Marshal.AllocHGlobal(512);
  try { int used; if(!GetTokenInformation(token,25,buffer,512,out used)) return -1;
   var sid=Marshal.ReadIntPtr(buffer); int count=Marshal.ReadByte(sid,1);
   return count>0 && count<=15 ? Marshal.ReadInt32(sid,8+(count-1)*4) : -1;
  } finally { Marshal.FreeHGlobal(buffer); CloseHandle(token); }
 }
 public static void Run() {
  var shell=FindWindowW("Shell_TrayWnd",null); uint pid; GetWindowThreadProcessId(shell,out pid);
  var process=OpenProcess(0x1000,false,pid);
  Console.WriteLine("Probe integrity RID="+Integrity(GetCurrentProcess())+" Explorer integrity RID="+(process!=IntPtr.Zero?Integrity(process):-1));
  if(process!=IntPtr.Zero) CloseHandle(process);
  SetLastError(0); IntPtr answer; var sent=SendMessageTimeoutW(shell,0,IntPtr.Zero,IntPtr.Zero,2,1000,out answer);
  Console.WriteLine("Shell_TrayWnd="+shell+" PID="+pid+" WM_NULL="+sent+" error="+Marshal.GetLastWin32Error());
  var hwnd=CreateWindowExW(0,"STATIC","ScreenShotPlus diagnostic",0x80000000,0,0,1,1,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero);
  Console.WriteLine("Own diagnostic HWND="+hwnd+" size="+Marshal.SizeOf(typeof(Data)));
  try { foreach(uint flags in new uint[]{7,135}) {
   var data=new Data{cbSize=(uint)Marshal.SizeOf(typeof(Data)),hwnd=hwnd,id=99,flags=flags,message=0x8001,icon=LoadIconW(IntPtr.Zero,new IntPtr(32512)),tip="ScreenShotPlus diagnostic",info="",title=""};
   SetLastError(0); bool ok=Shell_NotifyIconW(0,ref data); int error=Marshal.GetLastWin32Error();
   Console.WriteLine("ADD flags="+flags+" icon="+data.icon+" result="+ok+" lastError="+error);
   if(ok) { try {
    var id=new Id{size=(uint)Marshal.SizeOf(typeof(Id)),hwnd=hwnd,id=99}; Rect rect;
    Console.WriteLine("GetRect HRESULT="+Shell_NotifyIconGetRect(ref id,out rect)+" Rect="+rect.left+","+rect.top+","+rect.right+","+rect.bottom);
   } finally {Shell_NotifyIconW(2,ref data);} }
  }} finally {DestroyWindow(hwnd);}
 }
}
'@
[TrayProbe]::Run()
