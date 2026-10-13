$in = (Get-Content artifacts/punct_6/regression_frame_0_in_bits.txt -Raw).Trim()
$dec = (Get-Content artifacts/punct_6/regression_frame_0_decoded.txt -Raw).Trim()
$costs = Get-Content artifacts/punct_6/regression_frame_0_recon_costs.txt
$len = [math]::Min($in.Length, $dec.Length)
$mismatches=0
for ($i=0; $i -lt $len; $i++) {
    if ($in[$i] -ne $dec[$i]) {
        $mismatches++
        $gs = 3*$i
        if ($gs+2 -ge $costs.Count) { $group = $costs[$gs..($costs.Count-1)] } else { $group = $costs[$gs..($gs+2)] }
        $hasNeutral = ($group -contains '32767')
        Write-Output ("mismatch idx={0} in={1} dec={2} groupNeutral={3} groupCosts={4}" -f $i, $in[$i], $dec[$i], $hasNeutral, ($group -join ','))
    }
}
Write-Output ("total_mismatches={0}" -f $mismatches)
