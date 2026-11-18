# SPDX-License-Identifier: MIT
# Run only after correctness hashes and logs have been archived.
param([string[]]$Cases=@('w2-up64','w2-dfn64','address-enc128','address-dec96','address-dec96-confirm'),[string]$Prefix='cleanup-screen')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runs=Join-Path $workspace 'build/aco-runtime-trace/runs-next-optimization-2026-11-10'
$evidence=Join-Path $workspace 'docs/test-evidence/mesa-aco-next-optimization-2026-11-10'
$plan=@()
foreach($case in $Cases){
 $result=Get-Content -LiteralPath (Join-Path $evidence "$case/results.json") -Raw | ConvertFrom-Json
 $dir=(Resolve-Path -LiteralPath (Join-Path $runs $case)).Path
 if(-not $dir.StartsWith($runs+[IO.Path]::DirectorySeparatorChar)){throw 'Invalid experiment root'}
 foreach($file in Get-ChildItem -LiteralPath $dir -Recurse -File -Filter '*.bin'){
  $full=(Resolve-Path -LiteralPath $file.FullName).Path
  if(-not $full.StartsWith($dir+[IO.Path]::DirectorySeparatorChar)){throw 'Outside experiment'}
  if($full.Contains('LongCorrectness') -and $file.Name -match '^(baseline|candidate)-(steady-(131|279)|drs-11).bin$'){continue}
  $stage=$file.Directory.Parent.Name
  if($stage -in @('Correctness','LongCorrectness')){
   $hashes=@($result.$stage.rounds[0].output_hashes)
   if($file.Name -notmatch '^(baseline|candidate)-(steady|drs)-([0-9]+)[.]bin$'){throw 'Unexpected readback name'}
   $label=$Matches[1];$phase=$Matches[2];$index=[int]$Matches[3]
   $row=@($hashes | Where-Object {$_.phase -eq $phase -and $_.index -eq $index})
   if($row.Count -ne 1){throw 'Missing archived hash'}
   $hash=if($label -eq 'baseline'){$row[0].original}else{$row[0].candidate}
  }else{$hash=(Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash}
  if(-not $hash){throw 'Missing digest'}
  $plan+=[pscustomobject]@{path=$full;bytes=$file.Length;sha256=$hash;case=$case}
 }
}
$plan | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence "$Prefix-files.json") -Encoding utf8
foreach($file in $plan){Remove-Item -LiteralPath $file.path}
$summary=[pscustomobject]@{deleted_files=$plan.Count;reclaimed_bytes=($plan | Measure-Object bytes -Sum).Sum;free_bytes=(Get-PSDrive C).Free;kept='frame6 traces, long steady131/279 and DRS11; all logs and output hashes'}
$summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence "$Prefix-summary.json") -Encoding utf8
$summary | ConvertTo-Json
