; fanyi 本地翻译 — 一键安装包脚本（Inno Setup 6）
; 编译: ISCC.exe fanyi-installer.iss  →  dist\fanyi-setup-<版本>.exe
; CI 注入版本: ISCC.exe /DMyAppVersion=1.1.0 fanyi-installer.iss
; 说明: llama-cuda\*.dll 与 fanyi-firefox-signed.xpi 缺失时自动跳过（CI/本地均可编译）

#ifndef MyAppVersion
#define MyAppVersion "1.1.0"
#endif

#define MyAppName "fanyi 本地翻译"
#define MyAppPublisher "fanyi"
#define MyAppURL "http://127.0.0.1:8765"

[Setup]
AppId={{6F4A9E1C-2D8B-4A57-9C3E-51F0B7A2D9E4}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\fanyi
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\fanyi-server.exe
OutputDir=dist
OutputBaseFilename=fanyi-setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
CloseApplications=no
DisableProgramGroupPage=yes

[Languages]
Name: "chinese"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "autostart"; Description: "开机后台自启 fanyi-server（无窗口，仅托盘图标，推荐）"; \
    GroupDescription: "启动选项:"
Name: "desktopicon"; Description: "创建桌面快捷方式（设置页）"; \
    GroupDescription: "附加图标:"
Name: "modeldl"; Description: "立即下载翻译模型 Q8_0（1.9 GB，需要网络，约 2 分钟～1 小时视网速）"; \
    GroupDescription: "翻译模型:"

[Files]
Source: "fanyi-server.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "fanyi-cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "onnxruntime.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "settings.html"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "USAGE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "BUILD.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "安装运行库.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "fanyi-server-background.vbs"; DestDir: "{app}"; Flags: ignoreversion
Source: "fanyi-firefox-signed.xpi"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist restartreplace
Source: "llama-cpu\*.dll"; DestDir: "{app}\llama-cpu"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "llama-cuda\*.dll"; DestDir: "{app}\llama-cuda"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "extension\*"; DestDir: "{app}\extension"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
Name: "{app}\models"

[Icons]
Name: "{group}\fanyi 设置页"; Filename: "{#MyAppURL}\settings"
Name: "{group}\安装或切换 CPU-GPU 运行库"; Filename: "{app}\安装运行库.bat"
Name: "{group}\卸载 fanyi"; Filename: "{uninstallexe}"
Name: "{autodesktop}\fanyi 设置页"; Filename: "{#MyAppURL}\settings"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; \
    ValueName: "fanyi"; ValueData: """wscript.exe"" ""{app}\fanyi-server-background.vbs"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "wscript.exe"; Parameters: """{app}\fanyi-server-background.vbs"""; Description: "后台启动 fanyi-server（托盘图标）"; \
    Flags: nowait postinstall skipifsilent
Filename: "{cmd}"; Parameters: "/c curl.exe -L --retry 5 -C - -o ""{app}\models\Hy-MT2-1.8B-Q8_0.gguf"" ""https://hf-mirror.com/tencent/Hy-MT2-1.8B-GGUF/resolve/main/Hy-MT2-1.8B-Q8_0.gguf"" & echo. & echo 模型下载完成！按任意键关闭窗口 & pause >nul"; \
    Description: "下载翻译模型 Q8_0（1.9 GB）"; \
    Flags: postinstall runasoriginaluser skipifsilent

[Code]
var
  BrowserPage: TInputOptionWizardPage;

function FirefoxExe(): String;
var p: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\firefox.exe', '', p) then
    if FileExists(p) then Result := p;
  if (Result = '') and FileExists(ExpandConstant('{pf}\Mozilla Firefox\firefox.exe')) then
    Result := ExpandConstant('{pf}\Mozilla Firefox\firefox.exe');
  if (Result = '') and FileExists(ExpandConstant('{pf32}\Mozilla Firefox\firefox.exe')) then
    Result := ExpandConstant('{pf32}\Mozilla Firefox\firefox.exe');
end;

function BrowserExe(Name: String): String;
var p: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\' + Name, '', p) then
    if FileExists(p) then Result := p;
end;

function HasNVIDIA(): Boolean;
begin
  Result := RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\nvlddmkm') or
            RegKeyExists(HKLM, 'SOFTWARE\NVIDIA Corporation\Global');
end;

procedure CopyRuntimeDlls();
var
  SrcDir: String;
  FindRec: TFindRec;
  src, dst: String;
begin
  if HasNVIDIA() and DirExists(ExpandConstant('{app}\llama-cuda')) then begin
    SrcDir := ExpandConstant('{app}\llama-cuda');
    Log('GPU: NVIDIA detected, using CUDA runtime');
  end else begin
    SrcDir := ExpandConstant('{app}\llama-cpu');
    Log('GPU: using CPU runtime');
  end;
  if FindFirst(SrcDir + '\*.dll', FindRec) then begin
    try
      repeat
        src := SrcDir + '\' + FindRec.Name;
        dst := ExpandConstant('{app}\') + FindRec.Name;
        FileCopy(src, dst, False);
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure InitializeWizard();
begin
  BrowserPage := CreateInputOptionPage(wpSelectTasks,
    '浏览器扩展安装', '选择要安装 fanyi 扩展的浏览器',
    '勾选要自动安装扩展的浏览器（可多选）。' #13#10 #13#10
    'Firefox：使用 Mozilla 官方签名包，浏览器会弹出确认框，点「添加」即完成，永久生效。' #13#10
    'Chrome / Edge：受应用商店限制，无法静默安装；将自动打开扩展管理页，' #13#10
    '请开启右上角「开发者模式」→「加载已解压的扩展程序」→ 选择安装目录下的 extension 文件夹。',
    False, False);
  BrowserPage.Add('Firefox');
  BrowserPage.Add('Chrome');
  BrowserPage.Add('Edge');
  BrowserPage.Values[0] := FirefoxExe() <> '';
  BrowserPage.Values[1] := BrowserExe('chrome.exe') <> '';
  BrowserPage.Values[2] := BrowserExe('msedge.exe') <> '';
end;

procedure InstallFirefoxExtension();
var
  ff: String;
  R: Integer;
begin
  ff := FirefoxExe();
  if (ff = '') or not FileExists(ExpandConstant('{app}\fanyi-firefox-signed.xpi')) then begin
    if not WizardSilent() then
      MsgBox('未找到 Firefox 或签名扩展包，跳过扩展安装。', mbInformation, MB_OK);
    exit;
  end;
  if not WizardSilent() then
    MsgBox('即将打开 Firefox 并弹出「fanyi 本地翻译」的安装确认。' #13#10 '请在弹窗中点击「添加」完成安装。', mbInformation, MB_OK);
  Exec(ff, '"' + ExpandConstant('{app}') + '\fanyi-firefox-signed.xpi"', '', SW_SHOW, ewNoWait, R);
end;

procedure OpenChromiumExtensions(ExePath: String; Url: String; BrowserName: String);
var
  R: Integer;
begin
  if not WizardSilent() then
    MsgBox('即将打开 ' + BrowserName + ' 的扩展管理页。' #13#10 #13#10
      '由于 Chrome 系应用商店的限制，请按以下步骤手动加载：' #13#10
      '1. 打开页面右上角「开发者模式」开关' #13#10
      '2. 点击「加载已解压的扩展程序」' #13#10
      '3. 选择文件夹：' + ExpandConstant('{app}') + '\extension',
      mbInformation, MB_OK);
  Exec(ExePath, Url, '', SW_SHOW, ewNoWait, R);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  chrome, edge: String;
  R: Integer;
begin
  if CurStep = ssPostInstall then begin
    CopyRuntimeDlls();
    if BrowserPage.Values[0] then
      InstallFirefoxExtension();
    if BrowserPage.Values[1] then begin
      chrome := BrowserExe('chrome.exe');
      if chrome <> '' then
        OpenChromiumExtensions(chrome, 'chrome://extensions', 'Chrome')
      else
        MsgBox('未找到 Chrome，跳过。', mbInformation, MB_OK);
    end;
    if BrowserPage.Values[2] then begin
      edge := BrowserExe('msedge.exe');
      if edge <> '' then
        OpenChromiumExtensions(edge, 'edge://extensions', 'Edge')
      else
        MsgBox('未找到 Edge，跳过。', mbInformation, MB_OK);
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  R: Integer;
begin
  Result := '';
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im fanyi-server.exe', '', SW_HIDE, ewWaitUntilTerminated, R);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  R: Integer;
begin
  if CurUninstallStep = usUninstall then
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im fanyi-server.exe', '', SW_HIDE, ewWaitUntilTerminated, R);
end;
