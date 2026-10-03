' fanyi-server background launcher: no console window, tray icon only
Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
dir = fso.GetParentFolderName(WScript.ScriptFullName)
sh.Run """" & dir & "\fanyi-server.exe""", 0, False
