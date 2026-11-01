; PrismArchive -- Inno Setup 6 script (the release packaging path).
;
; Two installers exist on purpose, and they must stay in step:
;   tools/install.ps1   per-user, no admin, 22 extensions, the canonical logic
;                       (idempotent, -DryRun, self-test + mutation controls).
;   Installer/prism.iss this file: a signable single .exe for releases, same
;                       HKCU-only layout, same ProgID/command/icon values.
; A value that differs between them is a bug: the association strings below are
; byte-identical to the ones install.ps1 writes.

#define AppName "PrismArchive"
#define AppVersion "0.1.0"
#define AppPublisher "PrismArchive contributors"
#define AppURL "https://github.com/xueweijian/prism-archive"
#define AppExe "PrismArchive.exe"
#ifndef PayloadDir
  #define PayloadDir "..\..\build\Src\PrismWin"
#endif

[Setup]
AppId={{9F3C2A54-6B1D-4E7A-9C21-0D5B8E7A1C33}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} setup
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=
DefaultDirName={localappdata}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
ChangesAssociations=yes
OutputDir=dist
OutputBaseFilename={#AppName}-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\Assets\Icon\prism.ico
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
LicenseFile=..\LICENSE
AllowNoIcons=yes

[Languages]
; English only. Inno 6 ships no Simplified Chinese .isl (it lives in the
; unofficial translations set), and shipping a translation we cannot compile is
; worse than not shipping one: ISCC aborts with "couldn't open include file".
; Add it back as: Name: "chinesesimplified"; MessagesFile: "Languages\ChineseSimplified.isl"
; after dropping the official istrans file into Installer/Languages/.
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "associate"; Description: "Associate archive types with {#AppName} (current user only)"; GroupDescription: "Integration:"
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#PayloadDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\PrismShellExt.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#PayloadDir}\PrismShellExtClassic.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\Assets\Shaders\*.hlsl"; DestDir: "{app}\Assets\Shaders"; Flags: ignoreversion
Source: "..\Assets\Icon\prism.ico"; DestDir: "{app}\Assets\Icon"; Flags: ignoreversion
Source: "..\tools\install.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\tools\uninstall.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\Assets"
Type: dirifempty; Name: "{app}"

[Registry]
; ---- SupportedTypes on the Application entry (same list install.ps1 writes) ----
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".7z"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".zip"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".rar"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".tar"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".gz"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".tgz"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".bz2"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".tbz2"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".xz"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".txz"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".zst"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".lz"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".cab"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".iso"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".wim"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".arj"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".lzh"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".cpio"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".rpm"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".deb"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".001"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".zipx"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate

; ---- Per-type ProgIDs. Every value here is byte-identical to the one
;      tools/install.ps1 writes; the PowerShell self-test asserts those. ----
; .7z
Root: HKA; Subkey: "Software\Classes\.7z"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.7z"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.7z\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.7z"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.7z"; ValueType: string; ValueName: ""; ValueData: "7z Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.7z\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.7z\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.7z\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.7z\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .zip
Root: HKA; Subkey: "Software\Classes\.zip"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.zip"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.zip\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.zip"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zip"; ValueType: string; ValueName: ""; ValueData: "ZIP Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zip\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zip\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zip\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zip\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .rar
Root: HKA; Subkey: "Software\Classes\.rar"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.rar"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.rar\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.rar"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rar"; ValueType: string; ValueName: ""; ValueData: "RAR Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rar\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rar\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rar\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rar\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .tar
Root: HKA; Subkey: "Software\Classes\.tar"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.tar"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.tar\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.tar"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tar"; ValueType: string; ValueName: ""; ValueData: "TAR Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tar\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tar\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tar\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tar\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .gz
Root: HKA; Subkey: "Software\Classes\.gz"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.gz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.gz\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.gz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.gz"; ValueType: string; ValueName: ""; ValueData: "Gzip Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.gz\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.gz\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.gz\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.gz\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .tgz
Root: HKA; Subkey: "Software\Classes\.tgz"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.tgz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.tgz\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.tgz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tgz"; ValueType: string; ValueName: ""; ValueData: "Gzip Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tgz\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tgz\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tgz\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tgz\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .bz2
Root: HKA; Subkey: "Software\Classes\.bz2"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.bz2"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.bz2\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.bz2"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.bz2"; ValueType: string; ValueName: ""; ValueData: "Bzip2 Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.bz2\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.bz2\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.bz2\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.bz2\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .tbz2
Root: HKA; Subkey: "Software\Classes\.tbz2"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.tbz2"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.tbz2\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.tbz2"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tbz2"; ValueType: string; ValueName: ""; ValueData: "Bzip2 Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tbz2\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tbz2\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tbz2\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.tbz2\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .xz
Root: HKA; Subkey: "Software\Classes\.xz"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.xz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.xz\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.xz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.xz"; ValueType: string; ValueName: ""; ValueData: "XZ Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.xz\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.xz\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.xz\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.xz\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .txz
Root: HKA; Subkey: "Software\Classes\.txz"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.txz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.txz\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.txz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.txz"; ValueType: string; ValueName: ""; ValueData: "XZ Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.txz\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.txz\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.txz\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.txz\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .zst
Root: HKA; Subkey: "Software\Classes\.zst"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.zst"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.zst\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.zst"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zst"; ValueType: string; ValueName: ""; ValueData: "Zstandard Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zst\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zst\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zst\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zst\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .lz
Root: HKA; Subkey: "Software\Classes\.lz"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.lz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.lz\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.lz"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lz"; ValueType: string; ValueName: ""; ValueData: "Lzip Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lz\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lz\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lz\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lz\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .cab
Root: HKA; Subkey: "Software\Classes\.cab"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.cab"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.cab\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.cab"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cab"; ValueType: string; ValueName: ""; ValueData: "Cabinet Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cab\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cab\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cab\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cab\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .iso
Root: HKA; Subkey: "Software\Classes\.iso"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.iso"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.iso\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.iso"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.iso"; ValueType: string; ValueName: ""; ValueData: "Disc Image"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.iso\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.iso\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.iso\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.iso\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .wim
Root: HKA; Subkey: "Software\Classes\.wim"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.wim"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.wim\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.wim"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.wim"; ValueType: string; ValueName: ""; ValueData: "Windows Imaging Format"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.wim\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.wim\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.wim\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.wim\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .arj
Root: HKA; Subkey: "Software\Classes\.arj"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.arj"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.arj\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.arj"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.arj"; ValueType: string; ValueName: ""; ValueData: "ARJ Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.arj\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.arj\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.arj\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.arj\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .lzh
Root: HKA; Subkey: "Software\Classes\.lzh"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.lzh"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.lzh\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.lzh"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lzh"; ValueType: string; ValueName: ""; ValueData: "LHA Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lzh\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lzh\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lzh\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.lzh\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .cpio
Root: HKA; Subkey: "Software\Classes\.cpio"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.cpio"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.cpio\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.cpio"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cpio"; ValueType: string; ValueName: ""; ValueData: "CPIO Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cpio\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cpio\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cpio\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.cpio\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .rpm
Root: HKA; Subkey: "Software\Classes\.rpm"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.rpm"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.rpm\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.rpm"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rpm"; ValueType: string; ValueName: ""; ValueData: "RPM Package"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rpm\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rpm\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rpm\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.rpm\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .deb
Root: HKA; Subkey: "Software\Classes\.deb"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.deb"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.deb\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.deb"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.deb"; ValueType: string; ValueName: ""; ValueData: "Debian Package"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.deb\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.deb\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.deb\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.deb\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .001
Root: HKA; Subkey: "Software\Classes\.001"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.001"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.001\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.001"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.001"; ValueType: string; ValueName: ""; ValueData: "Split Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.001\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.001\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.001\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.001\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate
; .zipx
Root: HKA; Subkey: "Software\Classes\.zipx"; ValueType: string; ValueName: ""; ValueData: "PrismArchive.zipx"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.zipx\OpenWithProgids"; ValueType: none; ValueName: "PrismArchive.zipx"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zipx"; ValueType: string; ValueName: ""; ValueData: "ZIPX Archive"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zipx\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zipx\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zipx\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\PrismArchive.zipx\shell\open"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: associate

; ---- Engine discovery: PRISM_7Z_DLL for the current user ----
; Only written when 7-Zip is actually found (Check: HasEngine); uninsdeletevalue
; puts the user's environment back to unset on uninstall. The app also falls
; back to the standard install paths, so a missing value is not fatal.
Root: HKA; Subkey: "Environment"; ValueType: string; ValueName: "PRISM_7Z_DLL"; ValueData: "{code:GetEngineDll}"; Flags: uninsdeletevalue; Check: HasEngine

[Code]
var
  EngineDll: String;

function FindEngine(): String;
var
  Candidate: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM64, 'SOFTWARE\7-Zip', 'Path64', Candidate) and
     FileExists(AddBackslash(Candidate) + '7z.dll') then
  begin
    Result := AddBackslash(Candidate) + '7z.dll';
    exit;
  end;
  if RegQueryStringValue(HKLM64, 'SOFTWARE\7-Zip', 'Path', Candidate) and
     FileExists(AddBackslash(Candidate) + '7z.dll') then
  begin
    Result := AddBackslash(Candidate) + '7z.dll';
    exit;
  end;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\7-Zip', 'Path', Candidate) and
     FileExists(AddBackslash(Candidate) + '7z.dll') then
  begin
    Result := AddBackslash(Candidate) + '7z.dll';
    exit;
  end;
  if FileExists(ExpandConstant('{pf}\7-Zip\7z.dll')) then
  begin
    Result := ExpandConstant('{pf}\7-Zip\7z.dll');
    exit;
  end;
  if FileExists(ExpandConstant('{pf32}\7-Zip\7z.dll')) then
  begin
    Result := ExpandConstant('{pf32}\7-Zip\7z.dll');
    exit;
  end;
end;

function HasEngine(): Boolean;
begin
  Result := EngineDll <> '';
end;

function GetEngineDll(Param: String): String;
begin
  Result := EngineDll;
end;

function InitializeSetup(): Boolean;
begin
  EngineDll := FindEngine();
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and (EngineDll = '') then
    MsgBox('7-Zip was not found, so PrismArchive has no engine yet.' + #13#10 +
           'Install 7-Zip (or set PRISM_7Z_DLL) and re-run this setup.', mbInformation, MB_OK);
end;
