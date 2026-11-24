@echo off

powershell -NoProfile -NonInteractive -Command ^
	"$ErrorActionPreference = 'Stop'; $version = '0.9.8.1';" ^
	"$cli = Join-Path $env:ProgramFiles 'USBip\usbip.exe';" ^
	"if (!(Test-Path -LiteralPath $cli -PathType Leaf) -or (& $cli --version) -ne $version -or $LASTEXITCODE -ne 0) {" ^
	"    $arch = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant();" ^
	"    $url = 'https://github.com/vadimgrn/usbip-win2/releases/download/v.{0}/USBip-{0}-{1}.exe' -f $version, $arch;" ^
	"    $installer = Join-Path $env:TEMP ('USBip-{0}.exe' -f [guid]::NewGuid());" ^
	"    try {" ^
	"        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $installer;" ^
	"        if ((Get-AuthenticodeSignature -LiteralPath $installer).Status -ne 'Valid') { throw 'Invalid USB/IP installer signature.' };" ^
	"        $setup = Start-Process -FilePath $installer -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-' -Wait -PassThru;" ^
	"        if ($setup.ExitCode -ne 0) { throw ('USB/IP installer exit code: ' + $setup.ExitCode) }" ^
	"    } finally { Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue }" ^
	"};" ^
	"if ((& $cli --version) -ne $version -or $LASTEXITCODE -ne 0) { throw ('Expected usbip-win2 ' + $version) };" ^
	"& $cli port > $null; if ($LASTEXITCODE -ne 0) { throw 'USB/IP controller is not ready.' }"
if errorlevel 1 exit /b %errorlevel%

rem Required to make cloudtest use our version of taef
setx /M CloudTestWorkerCustomTaefExe "%1\taef\te.exe"

mkdir %2\WexLogFileOutput
mklink /D %1\WexLogFileOutput %2\WexLogFileOutput

rem These need to run before the package is installed
powershell -NoProfile -Command "Add-MpPreference -ExclusionPath 'C:\Program Files\WSL'"
powershell -NoProfile -Command "Add-MpPreference -ExclusionPath \"%appdata%\Microsoft\Windows\Start Menu\""
powershell -NoProfile -Command "Add-MpPreference -ExclusionProcess @('wsl.exe', 'wslg.exe', 'wslconfig.exe', 'wslrelay.exe', 'wslhost.exe', 'msrdc.exe', 'wslservice.exe', 'msal.wsl.proxy.exe', 'te.processhost.exe')"