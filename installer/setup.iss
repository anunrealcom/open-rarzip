; 压索 Yasuo Compressor 安装脚本 (Inno Setup 6)
; 运行 scripts\package.bat 即可自动完成 windeployqt 部署 + ISCC 编译；
; 也可手动先部署到 build\deploy\ 再单独编译本脚本。
; 采用用户级安装（PrivilegesRequired=lowest），因为右键菜单写入 HKCU 不需要管理员权限。

#define AppName "压索 Yasuo Compressor"
#define AppVersion "2.0.0"
#define AppExe "YasuoCompressor.exe"

[Setup]
AppId={{6D2A8E41-9F3C-4B7A-B0E2-1C5D7A9F4E33}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Yasuo Open Source
AppPublisherURL=https://github.com/
DefaultDirName={autopf}\YasuoCompressor
DefaultGroupName=压索
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=YasuoCompressor-2.0.0-Setup
Compression=lzma2
Solid=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#AppExe}
SetupIconFile=..\src\resources\app.ico
LicenseFile=..\LICENSE

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项"
Name: "shellmenu"; Description: "集成资源管理器右键菜单"; GroupDescription: "附加选项"; Flags: checkedonce

[Files]
Source: "..\build\deploy\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\deploy\*.dll"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs
Source: "..\build\deploy\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion
Source: "..\src\resources\logo.svg"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\压索"; Filename: "{app}\{#AppExe}"
Name: "{group}\卸载 压索"; Filename: "{uninstallexe}"
Name: "{autodesktop}\压索"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; 安装完成后静默注册右键菜单；程序本身也会在首次运行时兜底自动注册
Filename: "{app}\{#AppExe}"; Parameters: "--install-shell"; Flags: runhidden nowait; Tasks: shellmenu
Filename: "{app}\{#AppExe}"; Description: "立即启动 压索"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "--uninstall-shell"; Flags: runhidden; RunOnceId: "UnregShell"

[UninstallDelete]
Type: files; Name: "{app}\YasuoCompressor.exe"
