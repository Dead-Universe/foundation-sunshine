#requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$InspectOnly,
    [switch]$AllowVendorUI,
    [string]$Archive=(Join-Path $PSScriptRoot 'VBCABLE_Driver_Pack45.zip'),
    [string]$ResultPath=(Join-Path $env:TEMP 'sunshine-vbcable-result.json')
)
$ErrorActionPreference='Stop'
$expectedVersion=[version]'3.3.1.7'
$expectedArchive='B950E39F01AF1D04EA623C8F6D8EB9B6EA5C477C637295FABF20631C85116BFB'
$result=[ordered]@{At=[DateTime]::UtcNow.ToString('o');Status='checking';BundledDriverVersion=$expectedVersion.ToString();ChangesMade=$false;ActiveRenderCount=0;RestartRequired=$false;VendorExitCode=$null;Error=$null}
$code=102;$temporary=$null
function Get-CableState {
    $drivers=@(Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceClass='MEDIA'"|Where-Object{$_.DeviceName -eq 'VB-Audio Virtual Cable' -and $_.DriverProviderName -eq 'VB-Audio Software'})
    $active=@(Get-ChildItem -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render' -ErrorAction SilentlyContinue|Where-Object{
        $state=Get-ItemProperty -LiteralPath $_.PSPath
        $properties=Get-ItemProperty -LiteralPath ($_.PSPath+'\Properties') -ErrorAction SilentlyContinue
        $state.DeviceState -eq 1 -and $properties.'{a45c254e-df1c-4efd-8020-67d146a850e0},2' -eq 'CABLE Input'
    })
    [pscustomobject]@{Drivers=$drivers;ActiveRenderCount=$active.Count}
}
function Check-FileTrust([string]$Path,[string]$Hash,[string]$Signer){
    if((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Hash){throw 'The extracted official driver file SHA256 does not match.'}
    $signature=Get-AuthenticodeSignature -LiteralPath $Path
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $Signer){throw ('Windows could not validate the expected publisher signature: '+[IO.Path]::GetFileName($Path))}
}
try{
    if((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $expectedArchive){throw 'The complete official standard Pack45 archive is missing or has a different SHA256.'}
    $result['ArchiveSHA256']=$expectedArchive
    $before=Get-CableState
    $result.ActiveRenderCount=$before.ActiveRenderCount
    if($before.Drivers.Count){
        if($before.Drivers.Count -ne 1 -or -not $before.Drivers[0].IsSigned){throw 'Existing standard VB-CABLE driver state is ambiguous or unsigned; retain it and repair explicitly.'}
        $installed=[version]$before.Drivers[0].DriverVersion
        $result['InstalledDriverVersion']=$installed.ToString()
        if($before.ActiveRenderCount -lt 1){throw 'VB-CABLE is already registered but its CABLE Input playback endpoint is not active. No reinstall was attempted.'}
        if($installed -lt $expectedVersion){$result.Status='existing-older-retained-update-required';$result.Error='An older driver was retained. Use the official installer and restart workflow to update explicitly.';$code=103}
        else{$result.Status=if($installed -eq $expectedVersion){'existing-current-retained'}else{'existing-newer-retained'};$code=0}
    }elseif($InspectOnly){$result.Status='not-installed';$code=104}
    else{
        if(Test-Path -LiteralPath 'HKLM:\SOFTWARE\VB-Audio\Cable'){throw 'A previous VB-CABLE installation is still registered. Repair it explicitly with the original installer; no automatic reinstall was attempted.'}
        if(-not $AllowVendorUI -or -not [Environment]::UserInteractive){throw 'VB-CABLE is absent. Run the Sunshine installer interactively or use the included original installer; silent installation will not open or wait for an invisible vendor window.'}
        $principal=New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())
        if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Administrator privileges are required to install the driver.'}
        if(-not [Environment]::Is64BitOperatingSystem){throw 'This Sunshine package requires 64-bit Windows.'}
        $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        $temporary=Join-Path $tempRoot ('Sunshine-VBCable-'+[guid]::NewGuid().ToString('N'))
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip=[IO.Compression.ZipFile]::OpenRead($Archive)
        try{foreach($entry in $zip.Entries){if(-not ([IO.Path]::GetFullPath((Join-Path $temporary $entry.FullName))).StartsWith($temporary+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected archive extraction path.'}}}finally{$zip.Dispose()}
        [IO.Compression.ZipFile]::ExtractToDirectory($Archive,$temporary)
        $setup=Join-Path $temporary 'VBCABLE_Setup_x64.exe'
        Check-FileTrust $setup '734C35DFA6D98F48782A451633CEB471166EC70D60482FD89A1123D0EE3C4F41' 'A77952D93229D0EC36E2543081EEA7D125732B9C'
        Check-FileTrust (Join-Path $temporary 'vbaudio_cable64_win10.cat') '0A921EBADBE39CF3FA7C14FF37D1CA22565A9651244BD3AC177DC810BC99072E' '9A4A6CFEE73C6B65945FCAA083DFBCC1E9077E7E'
        Write-Host 'Complete the original VB-Audio installer window. Sunshine will not click its controls or restart Windows.'
        $vendor=Start-Process -FilePath $setup -WorkingDirectory $temporary -WindowStyle Normal -PassThru -Wait
        $result.VendorExitCode=$vendor.ExitCode
        $after=$null
        for($attempt=0;$attempt -lt 15;$attempt++){
            $after=Get-CableState
            if($after.Drivers.Count -eq 1 -and $after.Drivers[0].IsSigned -and [version]$after.Drivers[0].DriverVersion -ge $expectedVersion){break}
            Start-Sleep -Seconds 2
        }
        if($after.Drivers.Count -ne 1 -or -not $after.Drivers[0].IsSigned -or [version]$after.Drivers[0].DriverVersion -lt $expectedVersion){throw 'The vendor window exited, but the signed expected driver was not found. Installation was cancelled or failed; its exit code alone is not treated as success.'}
        $result['InstalledDriverVersion']=$after.Drivers[0].DriverVersion
        $result.ActiveRenderCount=$after.ActiveRenderCount
        $result.Status=if($after.ActiveRenderCount){'installed-restart-required'}else{'driver-staged-restart-required'}
        $result.ChangesMade=$true;$result.RestartRequired=$true;$code=3010
    }
}catch{$result.Status='failed';$result.Error=$_.Exception.Message;$code=102}
finally{
    if($temporary -and (Test-Path -LiteralPath $temporary)){
        $resolved=[IO.Path]::GetFullPath($temporary)
        if(-not $resolved.StartsWith($tempRoot.TrimEnd('\')+'\Sunshine-VBCable-',[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing cleanup outside this helper temporary directory.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    $result['ExitCode']=$code
    $result|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $ResultPath -Encoding UTF8
}
$result|ConvertTo-Json -Depth 4
exit $code
