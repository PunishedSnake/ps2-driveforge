#ifndef ReleaseRoot
  #error ReleaseRoot must be supplied with /DReleaseRoot=...
#endif
#ifndef OutputDir
  #error OutputDir must be supplied with /DOutputDir=...
#endif
#ifndef AppVersion
  #define AppVersion "0.5.0-rc4"
#endif

#define AppName "PS2 DriveForge"
#define AppPublisher "Hifu Himejima"
#define AppExeName "PS2-DriveForge.exe"
#define AppCodename "Emilia"

[Setup]
AppId={{70F721C1-06E5-4F75-90A6-DF51E568ED47}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion} {#AppCodename}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\PS2 DriveForge
DefaultGroupName=PS2 DriveForge
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#OutputDir}
OutputBaseFilename=PS2-DriveForge-{#AppVersion}-Emilia-Setup-x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppExeName}
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#ReleaseRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#ifdef DokanyMsi
Source: "{#DokanyMsi}"; DestDir: "{tmp}"; DestName: "Dokan_x64.msi"; Flags: deleteafterinstall; Check: NeedsDokany
#endif

[Icons]
Name: "{group}\PS2 DriveForge"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"
Name: "{group}\PS2 DriveForge — Legacy Win32"; Filename: "{app}\{#AppExeName}"; Parameters: "--legacy"; WorkingDir: "{app}"
Name: "{autodesktop}\PS2 DriveForge"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
#ifdef DokanyMsi
Filename: "{sys}\msiexec.exe"; Parameters: "/i ""{tmp}\Dokan_x64.msi"" /passive /norestart"; StatusMsg: "Installing the Dokany filesystem driver..."; Flags: waituntilterminated; Check: NeedsDokany
#endif
Filename: "{app}\{#AppExeName}"; Description: "Launch PS2 DriveForge"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
function NeedsDokany: Boolean;
begin
  { Dokany 2.x uses dokan2.sys. If a compatible 2.x driver already exists, do }
  { not attempt an in-place MSI replacement that could require an intermediate reboot. }
  Result := not FileExists(ExpandConstant('{sys}\drivers\dokan2.sys'));
end;
