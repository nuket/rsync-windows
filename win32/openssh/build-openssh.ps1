<#
.SYNOPSIS
    Build the ssh.exe that ships beside rsync.exe.

.DESCRIPTION
    rsync on Windows runs the remote side through an ssh client, and the one
    Windows ships reads a pipe on its stdin 3KB at a time with a thread per
    read -- which held a push from a Windows machine at about 17MB/s however
    fast the link was (WINDOWS-PORT.md, "Moar Speed!") -- and, once the link
    is fast enough to show it, creates a thread for every write to stdout and
    a named pipe for every pass through its main loop.  This script builds
    the same client, Microsoft's Win32-OpenSSH, from the pinned copy in the
    openssh/ submodule with the patches in win32/openssh/patches applied
    (see BUILD-CMAKE.md for what each one does), and leaves ssh.exe in the
    rsync build directory, where rsync.exe prefers it over the one on PATH.

    Its libcrypto is the one Windows already has.  ssh.exe links against the
    LibreSSL SDK Microsoft publishes for Win32-OpenSSH (headers and import
    library, the same 3.8.2 that the Windows OpenSSH Client component
    installs as System32\libcrypto.dll), and loads that DLL at run time.
    Windows' build of it uses AES-NI -- 5GB/s of AES on this machine against
    140MB/s for a LibreSSL built from source with the vcpkg port -- and
    rsync.exe's PreferSystem32Images hardening, which its children inherit,
    means an ssh.exe started by rsync takes the System32 copy in any case.
    So no DLL ships, for either architecture: the 32-bit build is for 32-bit
    Windows, whose System32 holds a 32-bit libcrypto.dll.

    zlib comes from the same place, prebuilt, and is linked statically.

.PARAMETER Arch
    x64 (default), x86, or both.

.PARAMETER Configuration
    Release (default) or Debug.

.PARAMETER Work
    Where the SDK zips are downloaded and unpacked.  Default: build-openssh
    in the repository, which is ignored by git.

.PARAMETER Out
    Where the binary lands.  Default: build-x64\ssh.exe for x64, so the ssh
    transfer tests run with it, and build-x86\ssh-x86.exe for x86 -- under a
    name rsync-x86.exe does not pick up, because on the 64-bit machine that
    builds it there is no 32-bit libcrypto.dll for it to load.

.PARAMETER Clean
    Rebuild the OpenSSH projects from scratch.

.EXAMPLE
    .\win32\openssh\build-openssh.ps1
    .\win32\openssh\build-openssh.ps1 -Arch both
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'both')]
    [string] $Arch = 'x64',
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [string] $Work,
    [string] $Out,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$repo    = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$src     = Join-Path $repo 'openssh'
$sol     = Join-Path $src 'contrib\win32\openssh'
$patches = Join-Path $PSScriptRoot 'patches'
if (-not $Work) { $Work = Join-Path $repo 'build-openssh' }

# Microsoft's prebuilt dependencies for Win32-OpenSSH, pinned by content.
# LibreSSL 3.8.2 is what the Windows OpenSSH Client 9.5 component ships as
# System32\libcrypto.dll; building against the same SDK is what makes loading
# that DLL sound.
$sdks = @(
    @{ Name = 'LibreSSL'; Url = 'https://github.com/PowerShell/LibreSSL/releases/download/V3.8.2.0/LibreSSL.zip'
       Sha256 = '572177cb6fdcf00488e7b0dacfd550dd1d538a3c192b3375e42ecb50b657c565' },
    @{ Name = 'ZLib';     Url = 'https://github.com/PowerShell/zlib/releases/download/V1.3.1/ZLib.zip'
       Sha256 = 'cf6c5e752ade112ff2de981553db21c0b077123243e771cc9bc335e725eb5c75' }
)

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# ------------------------------------------------------------------ sources
if (-not (Test-Path (Join-Path $src 'ssh.c'))) {
    throw "openssh/ is empty. Run: git submodule update --init openssh"
}

# Apply each patch once.  A patch that already applies in reverse is in place;
# anything else is a real conflict with the pinned source and must be looked at.
# --ignore-whitespace: the patched files are CRLF upstream, and whether a
# checkout -- of the submodule or of the patches -- has CRLF or LF depends on
# core.autocrlf on the machine; line endings must not decide whether a
# context line matches.
Step "patches"
foreach ($p in (Get-ChildItem $patches -Filter '*.patch' | Sort-Object Name)) {
    & git -C $src apply --reverse --check --ignore-whitespace $p.FullName 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Host "    $($p.Name): already applied"; continue }
    & git -C $src apply --whitespace=nowarn --ignore-whitespace $p.FullName
    if ($LASTEXITCODE -ne 0) { throw "$($p.Name) does not apply to the pinned openssh source" }
    Write-Host "    $($p.Name): applied"
}

# -------------------------------------------------------------------- tools
Step "tools"
$vswhere = "${Env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio 2022 with the C++ tools was not found" }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
$sdk = Get-ChildItem "${Env:ProgramFiles(x86)}\Windows Kits\10\Include" -Directory |
       Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'um\Windows.h')) } |
       Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $sdk) { throw "no Windows 10/11 SDK found" }
Write-Host "    msbuild    : $msbuild"
Write-Host "    windows sdk: $($sdk.Name)"

# paths.targets carries a hard-coded SDK version; point it at the one present.
$pt = Join-Path $sol 'paths.targets'
$xml = [xml](Get-Content $pt)
if ($xml.Project.PropertyGroup.WindowsSDKVersion -ne $sdk.Name) {
    $xml.Project.PropertyGroup.WindowsSDKVersion = $sdk.Name
    $xml.Save($pt)
}

# --------------------------------------------------------------- dependencies
Step "dependencies"
$sdkDir = Join-Path $Work 'sdk'
New-Item -ItemType Directory -Force $sdkDir | Out-Null
foreach ($s in $sdks) {
    $zip = Join-Path $sdkDir "$($s.Name).zip"
    $ok  = (Test-Path $zip) -and ((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -eq $s.Sha256)
    if (-not $ok) {
        Write-Host "    downloading $($s.Url)"
        Invoke-WebRequest -Uri $s.Url -OutFile $zip -UseBasicParsing
        $got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
        if ($got -ne $s.Sha256) { Remove-Item $zip -Force; throw "$($s.Name).zip: SHA-256 $got, expected $($s.Sha256)" }
    }
    $dir = Join-Path $sdkDir $s.Name
    if (-not (Test-Path (Join-Path $dir $s.Name))) {
        Expand-Archive -Path $zip -DestinationPath $dir -Force
    }
    Write-Host "    $($s.Name): $zip"
}
$libressl = Join-Path $sdkDir 'LibreSSL\LibreSSL'
$zlib     = Join-Path $sdkDir 'ZLib\ZLib'

if ($Clean) {
    Step "clean"
    foreach ($d in (Join-Path $sol 'vcpkg_installed'), (Join-Path $src 'bin'), (Join-Path $sol 'lib')) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    }
}

# -------------------------------------------------------------------- build
$archs = if ($Arch -eq 'both') { 'x64', 'x86' } else { @($Arch) }
$built = @()
foreach ($a in $archs) {
    $triplet  = "$a-custom"
    $platform = if ($a -eq 'x86') { 'Win32' } else { 'x64' }
    $exeName  = if ($a -eq 'x86') { 'ssh-x86.exe' } else { 'ssh.exe' }
    $outDir   = if ($Out) { $Out } elseif ($a -eq 'x86') { Join-Path $repo 'build-x86' } else { Join-Path $repo 'build-x64' }

    # The projects look for headers and libraries where vcpkg's manifest mode
    # would have put them.  Nothing here uses vcpkg; the SDK is laid out in
    # that shape so the projects need no change.
    Step "${a}: SDK into the layout the projects expect"
    $dep = Join-Path $sol "vcpkg_installed\$triplet\$triplet"
    foreach ($d in 'include', 'lib') { New-Item -ItemType Directory -Force (Join-Path $dep $d) | Out-Null }
    Copy-Item (Join-Path $libressl 'sdk\include\*') (Join-Path $dep 'include') -Recurse -Force
    Copy-Item (Join-Path $zlib 'sdk\*.h') (Join-Path $dep 'include') -Force
    Copy-Item (Join-Path $libressl "bin\desktop\$a\libcrypto.lib") (Join-Path $dep 'lib\libcrypto.lib') -Force
    Copy-Item (Join-Path $zlib "bin\$a\zlib.lib") (Join-Path $dep 'lib\zs.lib') -Force

    Step "${a}: ssh.exe (msbuild, $platform $Configuration)"
    # In dependency order; the projects reference the dependency directory
    # explicitly, so vcpkg's MSBuild integration is not needed and manifest
    # mode is off.  /nodeReuse:false: otherwise MSBuild leaves worker
    # processes behind holding the tree open.
    foreach ($proj in 'config.vcxproj', 'win32iocompat.vcxproj', 'openbsd_compat.vcxproj', 'libssh.vcxproj', 'ssh.vcxproj') {
        & $msbuild (Join-Path $sol $proj) "/p:Platform=$platform" "/p:Configuration=$Configuration" `
            "/p:SolutionDir=$sol\" '/p:VcpkgEnableManifest=false' /m /nologo /v:m /nodeReuse:false
        if ($LASTEXITCODE -ne 0) { throw "msbuild failed: $proj ($a)" }
    }

    $exe = Join-Path $src "bin\$platform\$Configuration\ssh.exe"
    if (-not (Test-Path $exe)) { throw "$exe was not produced" }
    New-Item -ItemType Directory -Force $outDir | Out-Null
    Copy-Item $exe (Join-Path $outDir $exeName) -Force
    $built += (Join-Path $outDir $exeName)

    # The licences travel with the binary, in one file: OpenSSH and its
    # openbsd-compat parts (LICENCE), Microsoft's Win32 layer (BSD, in the
    # file headers rather than a file of its own), and zlib, which is linked
    # in.  LibreSSL is not distributed here -- the DLL is Windows' own --
    # only built against.
    $msft = @'
Portions Copyright (c) 2015-2017 Microsoft Corp.
All rights reserved

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
'@
    $sep = "`n" + ('=' * 76) + "`n"
    $text = @(
        "NOTICES for ssh.exe, the OpenSSH client shipped with rsync for Windows",
        "",
        "ssh.exe is Microsoft's Win32-OpenSSH (github.com/PowerShell/openssh-portable),",
        "built from the pinned openssh/ submodule of github.com/nuket/rsync-windows with",
        "the patches in win32/openssh/patches applied.  zlib is linked in.  Its",
        "libcrypto is not distributed with it: ssh.exe loads the libcrypto.dll that",
        "the Windows OpenSSH Client component installs in System32 -- Windows' own",
        "build of LibreSSL 3.8.2, which this ssh.exe was built against.",
        $sep, "OpenSSH", $sep,
        (Get-Content (Join-Path $src 'LICENCE') -Raw),
        $sep, "Win32-OpenSSH (contrib/win32): Microsoft Corp.", $sep,
        $msft,
        $sep, "zlib", $sep,
        (Get-Content (Join-Path $PSScriptRoot 'LICENSE-zlib.txt') -Raw)
    ) -join "`n"
    $notice = Join-Path $outDir 'NOTICE-ssh.txt'
    [IO.File]::WriteAllText($notice, $text.Replace("`r`n", "`n").Replace("`n", "`r`n"))
    $built += $notice
}

Step "done"
foreach ($b in $built) {
    Write-Host ("    {0}  {1:N0} bytes" -f $b, (Get-Item $b).Length)
}
