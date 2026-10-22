#requires -Version 7.2
# The installable package of the build: UnchartedLauncher_<version>_x64.appx, signed with a self-signed test key for
# the manifest's publisher CN=uncharted_xbox (made once in .local/signing, never committed; the Xbox in Developer Mode
# installs such packages), the VCLibs framework package it needs in Dependencies, and both in
# UnchartedLauncher-<version>.zip. The version is the time in UTC, so every package installs over the previous one
# and keeps the app's data. The zip is checked for game files and paths of this machine (tools/leak_check.py).
[CmdletBinding()]
# -LocalTest: a package for this developer's own console. The leak check still runs and shows what it finds, but
# only the .appx is kept, for xbox-install.ps1; no release zip is made.
param([string]$Out='',[switch]$LocalTest)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$local=Join-Path $root '.local'
$layout=Join-Path $local 'build/layout'
& (Join-Path $PSScriptRoot 'check-imports.ps1') -Folder $layout
& (Join-Path $PSScriptRoot 'assets.ps1')
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
& "$vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$sdk=Join-Path $env:WindowsSdkVerBinPath 'x64'
$vclibs="${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx"
if(!(Test-Path -LiteralPath $vclibs)) { throw "Missing framework package: $vclibs" }
if(!$Out) { $Out=Join-Path $local ('packages/'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')) }
$stage=Join-Path $Out 'layout'
New-Item -ItemType Directory -Force $stage | Out-Null
foreach($name in @('AppxManifest.xml','uncharted_uwp.exe','libwinpthread-1.dll','Assets','ThirdParty','CREDITS.md','LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $layout $name) -Destination $stage -Recurse
}
foreach($name in @('vulkan_dzn.dll','dxil.dll')) {
    if(Test-Path -LiteralPath (Join-Path $layout $name)) {
        Copy-Item -LiteralPath (Join-Path $layout $name) -Destination $stage
    }
}
$now=[DateTime]::UtcNow
$version='1.0.{0}.{1}' -f ($now.Year%100*1000+$now.DayOfYear),([int]$now.ToString('HHmm'))
$manifest=Join-Path $stage 'AppxManifest.xml'
$content=(Get-Content -LiteralPath $manifest -Raw).Replace('Version="1.0.0.0"',"Version=`"$version`"")
[IO.File]::WriteAllText($manifest,$content,[Text.UTF8Encoding]::new($false))
$package=Join-Path $Out "UnchartedLauncher_$version`_x64.appx"
& "$sdk/makeappx.exe" pack /d $stage /p $package /o *> (Join-Path $Out 'pack.log')
if($LASTEXITCODE) { Get-Content (Join-Path $Out 'pack.log') -Tail 12; throw 'Package creation failed' }
$key=Join-Path $local 'signing/uncharted_xbox-test.pfx'
if(!(Test-Path -LiteralPath $key)) {
    # A code signing test key without a password, made with .NET: nothing goes into the Windows certificate store.
    $rsa=[Security.Cryptography.RSA]::Create(2048)
    $request=[Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=uncharted_xbox',$rsa,
        [Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false,$false,0,$false))
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
        [Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature,$false))
    $usages=[Security.Cryptography.OidCollection]::new(); [void]$usages.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($usages,$false))
    $certificate=$request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddDays(-1),[DateTimeOffset]::UtcNow.AddYears(10))
    New-Item -ItemType Directory -Force (Split-Path $key) | Out-Null
    [IO.File]::WriteAllBytes($key,$certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx))
}
& "$sdk/signtool.exe" sign /q /fd SHA256 /f $key $package
if($LASTEXITCODE) { throw 'Signing failed' }
Remove-Item -LiteralPath $stage -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $Out 'Dependencies') | Out-Null
Copy-Item -LiteralPath $vclibs -Destination (Join-Path $Out 'Dependencies')
$zip=Join-Path $Out "UnchartedLauncher-$version.zip"
Compress-Archive -Path $package,(Join-Path $Out 'Dependencies') -DestinationPath $zip
$settings=Join-Path $local 'settings.json'
$python=if(Test-Path -LiteralPath $settings) { (Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json).python } else { 'python' }
& $python (Join-Path (Split-Path $root -Parent) 'tools/leak_check.py') package $zip
if($LASTEXITCODE -and !$LocalTest) { Remove-Item -LiteralPath $zip,$package -Force; throw 'Package refused by the leak check (above); nothing was kept' }
if($LocalTest) {
    Remove-Item -LiteralPath $zip -Force
    $zip=''
    Write-Output 'Local test package: no release zip; install the .appx on your own console only.'
}
New-Item -ItemType Directory -Force $local | Out-Null
[IO.File]::WriteAllText((Join-Path $local 'latest-package.txt'),$package,[Text.UTF8Encoding]::new($false))
Write-Output "Signed package created: $version."
Write-Output "version=$version"
Write-Output "zip=$zip"
