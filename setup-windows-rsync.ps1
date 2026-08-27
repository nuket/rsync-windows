#Requires -Version 5.1
<#
.SYNOPSIS
    Make a Windows box reachable over rsync-over-ssh, in both directions.

.DESCRIPTION
    One script, run once, on a fresh Windows 10 1809+ / Windows 11 / Server 2019+ box:

      1. OpenSSH Client capability  - the ssh.exe rsync shells out to when this box SENDS.
      2. ssh-agent service          - Automatic + started, so keys loaded with ssh-add
                                      survive and are visible to ssh.exe and git.
      3. OpenSSH Server (sshd)      - Automatic + started + inbound TCP 22 on ALL firewall
                                      profiles, so this box can RECEIVE.
      4. rsync.exe                  - the latest release of github.com/nuket/rsync-windows,
                                      SHA-256 verified, installed to C:\Tools\rsync and put
                                      on the MACHINE PATH.
      5. authorized_keys (optional) - -AuthorizedKey installs a public key with the ACLs
                                      Win32-OpenSSH insists on before it will honour it.

    Idempotent: re-running skips anything already in place, and only re-downloads rsync
    when the installed build is not the latest release.

    Elevates itself via UAC if not already running as Administrator.

.PARAMETER InstallDir
    Where rsync.exe lands. Default C:\Tools\rsync. Deliberately NOT under "Program Files":
    the remote end of an rsync is invoked as a bare command line, and a path with spaces
    in it makes the client-side --rsync-path escape hatch painful to quote.

.PARAMETER Tag
    Release to install, e.g. 'v3.5.0-g521ad8ad'. Default 'latest'.

.PARAMETER AuthorizedKey
    A public key to authorise for inbound ssh: either the key text itself
    ('ssh-ed25519 AAAA... you@host') or the path to a .pub file.

.PARAMETER ForUser
    Account that -AuthorizedKey is installed for. Defaults to the invoking user; the script
    passes this through when it elevates, so the key does not land in an admin's profile.

.PARAMETER SkipServer
    Configure the client side only (ssh client, ssh-agent, rsync). No sshd, no firewall rule.
    Use this on a box that only ever sends.

.EXAMPLE
    .\setup-windows-rsync.ps1

.EXAMPLE
    .\setup-windows-rsync.ps1 -AuthorizedKey $HOME\.ssh\id_ed25519.pub

.EXAMPLE
    .\setup-windows-rsync.ps1 -SkipServer -InstallDir D:\bin
#>

[CmdletBinding()]
param(
    [string] $InstallDir = 'C:\Tools\rsync',
    [string] $Tag        = 'latest',
    [string] $AuthorizedKey,
    [string] $ForUser    = $env:USERNAME,
    [switch] $SkipServer
)

$ErrorActionPreference = 'Stop'
$Repo = 'nuket/rsync-windows'

# TLS 1.2: Windows PowerShell 5.1 still defaults to SSL3/TLS1.0, which github.com refuses.
[Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

function Write-Step([string] $Msg) { Write-Host "`n==> $Msg" -ForegroundColor Cyan }
function Write-Ok  ([string] $Msg) { Write-Host "    $Msg" -ForegroundColor Green }
function Write-Info([string] $Msg) { Write-Host "    $Msg" }

# ---------------------------------------------------------------------------
# Elevate.
#
# Every step below writes machine state: Windows capabilities, service start
# types, a firewall rule, the HKLM PATH. -ForUser is passed explicitly so that
# -AuthorizedKey still targets the account that STARTED the script -- with UAC's
# split token the elevated process is normally the same user, but "Run as
# administrator" against a different admin account would otherwise silently
# authorise the wrong profile.
# ---------------------------------------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host 'Not elevated - relaunching via UAC ...' -ForegroundColor Yellow
    # Build ONE pre-quoted command line rather than handing Start-Process an
    # array. The array form joins its elements with spaces and does not quote
    # them, so anything containing a space (a key path under "Documents and
    # Settings", an -InstallDir under Program Files) silently splits into two
    # arguments. Quoting here is unambiguous under both 5.1 and 7.
    $q = { param($s) '"' + ($s -replace '"', '\"') + '"' }
    $argv = "-NoExit -NoProfile -ExecutionPolicy Bypass -File $(& $q $PSCommandPath)" +
            " -InstallDir $(& $q $InstallDir)" +
            " -Tag $(& $q $Tag)" +
            " -ForUser $(& $q $ForUser)"
    if ($AuthorizedKey) { $argv += " -AuthorizedKey $(& $q $AuthorizedKey)" }
    if ($SkipServer)    { $argv += ' -SkipServer' }
    # Pick the host to re-launch rather than reusing our own. A Store-installed
    # PowerShell 7 lives under C:\Program Files\WindowsApps, whose ACLs make
    # Start-Process -Verb RunAs against its full path unreliable. Everything
    # below is Windows PowerShell 5.1 compatible (see #Requires), so falling all
    # the way back to the in-box powershell.exe is always safe.
    $hostExe = @(
        (Join-Path $env:ProgramFiles 'PowerShell\7\pwsh.exe'),
        (Join-Path $PSHOME 'pwsh.exe'),
        (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe')
    ) | Where-Object { $_ -notlike '*\WindowsApps\*' -and (Test-Path $_) } | Select-Object -First 1

    # -NoExit (set above) so the elevated console stays open and its output is
    # readable; it is a new window either way, and it closing on the last line
    # is the classic way to never find out what went wrong.
    Start-Process -FilePath $hostExe -Verb RunAs -ArgumentList $argv | Out-Null
    exit 0
}

# ---------------------------------------------------------------------------
# 1. OpenSSH Client capability
#
# rsync does not speak ssh itself: it execs an ssh binary. On Windows that is
# %WINDIR%\System32\OpenSSH\ssh.exe, present by default on 1809+ but removable,
# and absent from some Server images. It also owns the ssh-agent service below.
# ---------------------------------------------------------------------------
function Install-SshCapability([string] $Pattern, [string] $Label) {
    $cap = Get-WindowsCapability -Online -Name $Pattern | Select-Object -First 1
    if (-not $cap) {
        Write-Warning "$Label is not offered by this Windows image - skipping."
        return $false
    }
    if ($cap.State -eq 'Installed') {
        Write-Ok "already installed: $($cap.Name)"
        return $true
    }
    Write-Info "installing $($cap.Name) ..."
    $r = Add-WindowsCapability -Online -Name $cap.Name
    if ($r.RestartNeeded) {
        Write-Host "    [reboot required to finish $Label]" -ForegroundColor Yellow
    }
    Write-Ok "installed: $($cap.Name)"
    return $true
}

Write-Step 'OpenSSH Client'
try {
    Install-SshCapability 'OpenSSH.Client*' 'OpenSSH Client' | Out-Null
} catch {
    Write-Warning "OpenSSH Client: $($_.Exception.Message)"
}

# ---------------------------------------------------------------------------
# 2. ssh-agent
#
# Ships Disabled, so the StartupType has to be set before Start-Service will do
# anything -- starting a disabled service is an error, not a no-op.
# ---------------------------------------------------------------------------
Write-Step 'ssh-agent service'
try {
    Set-Service -Name ssh-agent -StartupType Automatic
    if ((Get-Service ssh-agent).Status -ne 'Running') { Start-Service ssh-agent }
    Write-Ok 'ssh-agent: Automatic + running'
} catch {
    Write-Warning "ssh-agent: $($_.Exception.Message)"
}

# ---------------------------------------------------------------------------
# 3. OpenSSH Server (sshd) + firewall
# ---------------------------------------------------------------------------
if ($SkipServer) {
    Write-Step 'OpenSSH Server - skipped (-SkipServer)'
} else {
    Write-Step 'OpenSSH Server (sshd)'
    try {
        if (Install-SshCapability 'OpenSSH.Server*' 'OpenSSH Server') {
            Set-Service -Name sshd -StartupType Automatic
            if ((Get-Service sshd).Status -ne 'Running') { Start-Service sshd }
            Write-Ok 'sshd: Automatic + running'

            # Inbound 22 on ALL profiles. A VM's host-only or bridged adapter is
            # routinely classified Public, and the capability's own rule is
            # Private-only on some images -- which is the usual reason a plainly
            # running sshd is plainly unreachable.
            #
            # OpenSSH-Server-In-TCP is the name the capability itself uses, so
            # this WIDENS that rule rather than adding a second one beside it.
            # A rule of our own under a different name would leave the narrow one
            # in place and the box still unreachable; under the same name it
            # would collide. Adopt if present, create if not.
            $ruleName = 'OpenSSH-Server-In-TCP'
            if (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue) {
                Set-NetFirewallRule -Name $ruleName -Enabled True -Profile Any
                Write-Ok "firewall: widened $ruleName to all profiles"
            } else {
                New-NetFirewallRule -Name $ruleName `
                    -DisplayName 'OpenSSH SSH Server (sshd)' `
                    -Enabled True -Direction Inbound -Protocol TCP `
                    -Action Allow -LocalPort 22 -Profile Any | Out-Null
                Write-Ok "firewall: added $ruleName (TCP 22, all profiles)"
            }
        }
    } catch {
        Write-Warning "OpenSSH Server: $($_.Exception.Message)"
    }
}

# ---------------------------------------------------------------------------
# 4. rsync
#
# Windows ships the ssh transport but no rsync, so the remote end of an
# `rsync -e ssh` has nothing to run. Install the x64 build on a 64-bit OS and
# the x86 one otherwise, both under the name rsync.exe: it is what the sending
# side asks for over the wire, and renaming saves every caller a --rsync-path.
# ---------------------------------------------------------------------------
Write-Step "rsync for Windows ($Repo)"

# One zip per architecture, holding rsync.exe and the ssh.exe it runs under
# those names, plus the licence texts.
$asset     = if ([Environment]::Is64BitOperatingSystem) { 'rsync-windows-x64.zip' } else { 'rsync-windows-x86.zip' }
$target    = Join-Path $InstallDir 'rsync.exe'
$sshTarget = Join-Path $InstallDir 'ssh.exe'

try {
    # Resolve the release. The API gives us the tag, which is what makes the
    # "already current?" check below possible; without it we could only ever
    # re-download and compare afterwards.
    $api = if ($Tag -eq 'latest') {
        "https://api.github.com/repos/$Repo/releases/latest"
    } else {
        "https://api.github.com/repos/$Repo/releases/tags/$Tag"
    }
    $release  = $null
    $resolved = $Tag
    try {
        # A User-Agent is mandatory on the GitHub API; without one it 403s.
        $release  = Invoke-RestMethod -Uri $api -UseBasicParsing `
                        -Headers @{ 'User-Agent' = 'setup-windows-rsync' }
        $resolved = $release.tag_name
        Write-Info "release: $resolved"
    } catch {
        # Unauthenticated API calls are rate-limited to 60/hour per IP, which a
        # provisioning run behind a shared NAT can genuinely exhaust. The
        # /releases/latest/download/ redirect needs no API budget, so fall back
        # to it and accept that we lose the version check.
        Write-Warning "GitHub API unavailable ($($_.Exception.Message)); falling back to the redirect URL."
    }

    if ($release) {
        $exeUrl = ($release.assets | Where-Object { $_.name -eq $asset          }).browser_download_url
        $shaUrl = ($release.assets | Where-Object { $_.name -eq "$asset.sha256" }).browser_download_url
        if (-not $exeUrl) { throw "release $resolved has no asset named $asset" }
    } else {
        $base   = "https://github.com/$Repo/releases"
        $prefix = if ($Tag -eq 'latest') { "$base/latest/download" } else { "$base/download/$Tag" }
        $exeUrl = "$prefix/$asset"
        $shaUrl = "$prefix/$asset.sha256"
    }

    # The zip's ssh.exe is the release's own build of Microsoft's OpenSSH
    # client: the one Windows ships reads its stdin 3KB at a time, which holds
    # a transfer *from* this machine at ~17MB/s whatever the link. rsync.exe
    # prefers an ssh.exe in its own directory, so unpacking them together is
    # all it takes. Same ~/.ssh, same ssh-agent, same known_hosts as the
    # system one. It loads the libcrypto.dll that the OpenSSH Client component
    # (step 1) puts in System32 -- Windows' own LibreSSL, and the fast one --
    # built against the 3.8.2 that OpenSSH Client 9.5 carries, so an older
    # Windows gets a warning rather than a binary that will not start, and a
    # Windows without the component gets rsync alone.
    $sysCrypto = Join-Path $env:SystemRoot 'System32\libcrypto.dll'
    $wantSsh   = Test-Path $sysCrypto
    if (-not $wantSsh) {
        Write-Warning "$sysCrypto is missing -- the OpenSSH Client component is not installed. The release's ssh.exe needs it, so only rsync.exe is installed; rsync will use the ssh on PATH."
    } else {
        $v = (Get-Item $sysCrypto).VersionInfo.FileVersion
        if ($v -and ([version]($v -replace '[^0-9.]', '')) -lt [version]'3.8.2') {
            Write-Warning "$sysCrypto is LibreSSL $v; the release's ssh.exe is built against 3.8.2 (Windows OpenSSH Client 9.5). Update Windows, or expect ssh.exe not to start."
        }
    }

    # Skip the download when what is installed already IS this release. The
    # exe carries the version string the tag is built from -- tag v3.5.0-gABC
    # against ProductVersion 3.5.0-gABC -- so this compares builds, not
    # filenames; ssh.exe has to be there too, or the zip is worth unpacking.
    $installed = if (Test-Path $target) { (Get-Item $target).VersionInfo.ProductVersion } else { $null }
    if ($installed -and $resolved -eq "v$installed" -and ((Test-Path $sshTarget) -or -not $wantSsh)) {
        Write-Ok "already current: rsync $installed at $target"
    } else {
        if ($installed) { Write-Info "installed rsync $installed -> updating to $resolved" }
        New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

        # Download and unpack beside the targets, not over them: an interrupted
        # transfer must not leave a truncated rsync.exe sitting on the machine
        # PATH. The scratch name still has to END in .zip: Windows PowerShell
        # 5.1's Expand-Archive refuses any other extension outright ("*.download
        # is not a supported archive file format"), where PowerShell 7 just
        # reads the file -- and 5.1 is what the UAC relaunch above falls back to
        # on a box with no pwsh installed.
        $tmp = Join-Path $InstallDir "download-$asset"
        Invoke-WebRequest -Uri $exeUrl -OutFile $tmp -UseBasicParsing
        Write-Info "downloaded $asset ($([math]::Round((Get-Item $tmp).Length / 1MB, 2)) MB)"

        # Verify against the .sha256 published alongside it. Same origin, so this
        # is an integrity check on the transfer, not a defence against a hostile
        # release -- but a truncated or proxy-mangled download is the failure that
        # actually happens, and it fails here rather than mid-transfer later.
        if ($shaUrl) {
            # -OutFile, not .Content: GitHub serves the .sha256 as
            # application/octet-stream, and Invoke-WebRequest hands back a
            # byte[] rather than a string for any non-text content type. Reading
            # .Content directly parses the first BYTE as the hash and the
            # comparison then fails on every correct download.
            $shaTmp = "$tmp.sha256"
            Invoke-WebRequest -Uri $shaUrl -OutFile $shaTmp -UseBasicParsing
            $want = (((Get-Content $shaTmp -Raw) -split '\s+')[0]).Trim().ToLower()
            Remove-Item $shaTmp -Force -ErrorAction SilentlyContinue
            $got  = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
            if ($want -and $want -ne $got) {
                Remove-Item $tmp -Force
                throw "SHA-256 mismatch for ${asset}: expected $want, got $got"
            }
            Write-Ok "SHA-256 verified: $got"
        }

        $unpack = Join-Path $InstallDir '.unpack'
        if (Test-Path $unpack) { Remove-Item -Recurse -Force $unpack }
        Expand-Archive -Path $tmp -DestinationPath $unpack -Force
        Remove-Item $tmp -Force
        foreach ($f in 'rsync.exe', 'ssh.exe', 'COPYING.txt', 'NOTICE-ssh.txt') {
            $src = Join-Path $unpack $f
            if (-not (Test-Path $src)) { continue }
            if ($f -eq 'ssh.exe' -and -not $wantSsh) { continue }
            Move-Item -Path $src -Destination (Join-Path $InstallDir $f) -Force
        }
        Remove-Item -Recurse -Force $unpack
        Write-Ok "installed $target$(if ($wantSsh) { ' and ssh.exe' })"
    }

    # --- Machine PATH ---------------------------------------------------------
    # MACHINE, not user: the remote end of an rsync runs non-interactively with
    # no login shell, and Win32-OpenSSH builds that session's environment from
    # the registry rather than from a profile. A user-PATH entry is invisible to
    # it, and the symptom is the maddening one -- rsync works when you ssh in and
    # type it, and "command not found" when rsync itself is the caller.
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if (-not $machinePath) { $machinePath = '' }
    $entries = $machinePath -split ';' | Where-Object { $_ }
    if ($entries -notcontains $InstallDir) {
        $new = if ($machinePath.Trim()) { $machinePath.TrimEnd(';') + ';' + $InstallDir } else { $InstallDir }
        [Environment]::SetEnvironmentVariable('Path', $new, 'Machine')
        Write-Ok "added $InstallDir to the machine PATH"

        # sshd caches the environment it was started with, so a service already
        # running would not see the new PATH until it is restarted -- and that is
        # exactly the session an inbound rsync lands in.
        if ((Get-Service sshd -ErrorAction SilentlyContinue).Status -eq 'Running') {
            Restart-Service sshd
            Write-Ok 'restarted sshd so it inherits the updated machine PATH'
        }
    } else {
        Write-Ok "$InstallDir already on the machine PATH"
    }

    # Make it usable in THIS console too, without a restart.
    if (($env:Path -split ';') -notcontains $InstallDir) { $env:Path += ";$InstallDir" }

    Write-Info (& $target --version 2>&1 | Select-Object -First 1)
} catch {
    Write-Warning "rsync install failed: $($_.Exception.Message)"
    Write-Warning "Download $asset from https://github.com/$Repo/releases manually,"
    Write-Warning "save it as $target, and add $InstallDir to the machine PATH."
}

# ---------------------------------------------------------------------------
# 5. authorized_keys (optional)
#
# Win32-OpenSSH refuses a key file that is writable by anyone other than SYSTEM
# and the file's owner, and it fails CLOSED and near-silently: the client just
# falls back to password auth. Both the location and the ACL depend on whether
# the account is an administrator, which is the part that catches people out --
# an admin's ~/.ssh/authorized_keys is ignored outright by the default sshd_config.
# ---------------------------------------------------------------------------
if ($AuthorizedKey) {
    Write-Step "Authorising a key for $ForUser"
    try {
        $keyText = if (Test-Path -LiteralPath $AuthorizedKey) {
            (Get-Content -LiteralPath $AuthorizedKey -Raw).Trim()
        } else {
            $AuthorizedKey.Trim()
        }
        if ($keyText -notmatch '^(ssh-|ecdsa-|sk-)\S+\s+[A-Za-z0-9+/=]+') {
            throw 'that does not look like an OpenSSH public key (or a path to one)'
        }

        $sid      = (New-Object Security.Principal.NTAccount($ForUser)
                    ).Translate([Security.Principal.SecurityIdentifier])
        $systemSid = New-Object Security.Principal.SecurityIdentifier('S-1-5-18')
        $adminSid  = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')

        # Membership by SID, not by name: the Administrators group is localised,
        # and a domain account's member name is not $ForUser.
        $isUserAdmin = [bool] (Get-LocalGroupMember -SID $adminSid -ErrorAction SilentlyContinue |
            Where-Object { $_.SID.Value -eq $sid.Value })

        if ($isUserAdmin) {
            # The default sshd_config routes every member of Administrators to
            # this one shared file and ignores their per-user one entirely.
            $keyFile = Join-Path $env:ProgramData 'ssh\administrators_authorized_keys'
            $grants  = @($systemSid, $adminSid)
            Write-Info "$ForUser is an administrator -> $keyFile"
        } else {
            $profileDir = (Get-CimInstance Win32_UserProfile -Filter "SID = '$($sid.Value)'").LocalPath
            if (-not $profileDir) { throw "no local profile for $ForUser (log in as that user once first)" }
            $keyFile = Join-Path $profileDir '.ssh\authorized_keys'
            $grants  = @($systemSid, $sid)
            Write-Info "-> $keyFile"
        }

        New-Item -ItemType Directory -Force -Path (Split-Path $keyFile) | Out-Null
        $existing = if (Test-Path $keyFile) { Get-Content $keyFile } else { @() }
        if ($existing -contains $keyText) {
            Write-Ok 'key already authorised'
        } else {
            # ASCII, no BOM: sshd reads this file byte-wise and a UTF-8 BOM turns
            # the first line into an unparsable option, taking the key with it.
            @($existing + $keyText) | Where-Object { $_ } |
                Out-File -FilePath $keyFile -Encoding ascii -Force
            Write-Ok 'key appended'
        }

        # Break inheritance and rebuild the ACL from nothing: an inherited
        # "Users: Modify" from C:\ is precisely what sshd rejects, and merely
        # adding the right entries would leave it in place.
        $acl = Get-Acl $keyFile
        $acl.SetAccessRuleProtection($true, $false)
        # @() forces the rule collection into a materialised copy first.
        # Enumerating $acl.Access lazily while RemoveAccessRule mutates the
        # underlying collection skips entries, leaving some of the ACL we are
        # trying to strip in place.
        @($acl.Access) | ForEach-Object { $acl.RemoveAccessRule($_) | Out-Null }
        foreach ($g in $grants) {
            $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule(
                $g, 'FullControl', 'Allow')))
        }
        $acl.SetOwner($sid)
        Set-Acl -Path $keyFile -AclObject $acl
        Write-Ok 'ACL locked down (inheritance off; SYSTEM + owner only)'
    } catch {
        Write-Warning "authorized_keys setup failed: $($_.Exception.Message)"
    }
}

# ---------------------------------------------------------------------------
Write-Step 'Done'

# Address to advertise. Take it from the interface holding the default route,
# lowest metric first: a box with WSL or Hyper-V installed has several vEthernet
# adapters whose addresses sort ahead of the real NIC's, and printing one of
# those gives a "receive" command that the sending box cannot reach.
$ip = $null
try {
    $ifIndex = (Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop |
                Sort-Object RouteMetric, InterfaceMetric |
                Select-Object -First 1).InterfaceIndex
    if ($null -ne $ifIndex) {
        $ip = (Get-NetIPAddress -InterfaceIndex $ifIndex -AddressFamily IPv4 `
                    -ErrorAction Stop | Select-Object -First 1).IPAddress
    }
} catch {}
if (-not $ip) {
    $ip = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
           Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
           Select-Object -First 1).IPAddress
}
$host_ = if ($ip) { $ip } else { $env:COMPUTERNAME }

Write-Host ''
Write-Host "  host   : $env:COMPUTERNAME$(if ($ip) { " ($ip)" })"
Write-Host "  user   : $ForUser"
Write-Host "  rsync  : $target"
if (-not $SkipServer) {
    Write-Host "  sshd   : $((Get-Service sshd -ErrorAction SilentlyContinue).Status) on TCP 22"
}
Write-Host ''
Write-Host '  Receive - run this on the sending (Linux) box:' -ForegroundColor Cyan
Write-Host "      rsync -av ./data/ $ForUser@${host_}:data/"
Write-Host ''
Write-Host '  Send - run this here:' -ForegroundColor Cyan
Write-Host '      ssh-add $HOME\.ssh\id_ed25519      # once; ssh-agent keeps it across boots'
Write-Host '      rsync -av ./data/ user@linuxbox:/srv/data/'
Write-Host ''
if (-not $AuthorizedKey) {
    Write-Host '  No key is authorised for inbound ssh yet - password auth only.' -ForegroundColor Yellow
    Write-Host '  Re-run with the public key to fix that:' -ForegroundColor Yellow
    Write-Host "      .\setup-windows-rsync.ps1 -AuthorizedKey `$HOME\.ssh\id_ed25519.pub"
    Write-Host ''
}
Write-Host '  Open a NEW shell before using rsync elsewhere: the machine PATH change' -ForegroundColor DarkGray
Write-Host '  does not reach shells that were already running.' -ForegroundColor DarkGray
