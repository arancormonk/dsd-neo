$idx=6
$mother = (Get-Content artifacts/punct_6/regression_frame_0_mother_code.txt -Raw).Trim()
$in = (Get-Content artifacts/punct_6/regression_frame_0_in_bits.txt -Raw).Trim()
$dec = (Get-Content artifacts/punct_6/regression_frame_0_decoded.txt -Raw).Trim()
$recon = Get-Content artifacts/punct_6/regression_frame_0_recon_costs.txt
$gs = 4*$idx
Write-Output ("input[{0}]={1} decoded[{0}]={2}" -f $idx, $in[$idx], $dec[$idx])
Write-Output ("mother bits around idx (0-based positions {0}..{1}): {2}" -f ([math]::Max(0,$gs-8)), ($gs+8), $mother.Substring([math]::Max(0,$gs-8), [math]::Min(17, $mother.Length-([math]::Max(0,$gs-8)))))
Write-Output ("mother group bits ({0}..{1}): {2}" -f $gs, $gs+3, $mother.Substring($gs, 4))
Write-Output ("type3 stream (first 120): " + (Get-Content artifacts/punct_6/regression_frame_0_type3.txt -Raw).Trim().Substring(0, [math]::Min(120, (Get-Content artifacts/punct_6/regression_frame_0_type3.txt -Raw).Trim().Length)))
Write-Output ("recon costs group: " + ($recon[$gs..($gs+3)] -join ','))
