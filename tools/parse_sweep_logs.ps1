$out = @()
Get-ChildItem -Path . -Filter 'sweep_run_*.log' | Sort-Object Name | ForEach-Object {
    $name = $_.Name
    $id = ($name -replace 'sweep_run_(\d+)\.log','$1')
    $txt = Get-Content -Raw -Path $_.FullName
    $m = [regex]::Match($txt,'wrote viterbi_vs_osmo_diff.txt \(diffs=(\d+)\)')
    if ($m.Success) { $diff = $m.Groups[1].Value }
    else {
        $m2 = [regex]::Match($txt,'# total_diffs=(\d+)')
        if ($m2.Success) { $diff = $m2.Groups[1].Value } else { $diff = 'N/A' }
    }
    $out += "punct_id=$id diffs=$diff"
}
$out | Set-Content sweep_summary.txt
Get-Content sweep_summary.txt
