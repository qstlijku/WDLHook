$mab = [System.IO.File]::ReadAllBytes('C:\Users\qstli\Downloads\UPC_ACHTool\WDLHook\f00-000-00_trained-idle_mplyr_000f_norm_nowep.mab')
$streamBase = 0x714  # byte offset in file where bitstream starts

$interp_scale = @(0, 0, 0.33333334, 0.14285715, 0.06666667, 0.032258064, 0.015873017,
                  0.0078740157, 0.0039215689, 0.0019569471, 0.00097751711, 0.00048851978,
                  0.00024420026, 0.00012208521, 0.000061038882, 0.000030518509, 0.000015259022)

function ReadBits([int]$bitPos, [int]$n) {
    $val = 0
    for ($i = 0; $i -lt $n; $i++) {
        $b = $streamBase + (($bitPos + $i) -shr 3)
        $bit = ($mab[$b] -shr (($bitPos + $i) -band 7)) -band 1
        $val = $val -bor ($bit -shl $i)
    }
    return $val
}

function DecodeComponent([int]$bitPos, [bool]$isConst, [int]$numInterp, [int]$numFrames) {
    if ($isConst) {
        $v7 = ReadBits $bitPos 16
        return $v7 * 0.000030518044 - 1.0
    } else {
        $v12 = ReadBits $bitPos 16
        $v13 = ($v12 % 256) * 0.0078740157 - 1.0
        $v14 = ($v12 -shr 8) * 0.0078431377 * $interp_scale[$numInterp]
        $frame0 = ReadBits ($bitPos + 16) $numInterp
        return $v13 + $v14 * $frame0
    }
}

# Each datum: [bitPos_data_start, interp_bits, constFlags, numFrames]
# bitPos is the position AFTER the 6-bit constFlags header
# Bone order from CommonBoneIndexes in pelvis_ref.skeleton
$datums = @(
    [pscustomobject]@{bone='Pelvis';      bitPos=6;    interp=12; cf=48; nf=8},
    [pscustomobject]@{bone='Spine';       bitPos=348;  interp=8;  cf=16; nf=8},
    [pscustomobject]@{bone='Spine1';      bitPos=594;  interp=8;  cf=48; nf=8},
    [pscustomobject]@{bone='Spine2';      bitPos=840;  interp=8;  cf=48; nf=8},
    [pscustomobject]@{bone='L_UpperArm';  bitPos=1086; interp=8;  cf=48; nf=8},
    [pscustomobject]@{bone='R_UpperArm';  bitPos=1332; interp=9;  cf=16; nf=8},
    [pscustomobject]@{bone='L_Forearm';   bitPos=1602; interp=9;  cf=48; nf=8},
    [pscustomobject]@{bone='R_Forearm';   bitPos=1872; interp=8;  cf=48; nf=8},
    [pscustomobject]@{bone='L_Hand';      bitPos=2118; interp=8;  cf=48; nf=8},
    [pscustomobject]@{bone='R_Hand';      bitPos=2364; interp=12; cf=48; nf=8},
    [pscustomobject]@{bone='Neck';        bitPos=2706; interp=12; cf=48; nf=8},
    [pscustomobject]@{bone='Head';        bitPos=3048; interp=12; cf=48; nf=8},
    [pscustomobject]@{bone='L_Thigh';     bitPos=3390; interp=12; cf=0;  nf=8},
    [pscustomobject]@{bone='R_Thigh';     bitPos=3732; interp=12; cf=48; nf=8},
    [pscustomobject]@{bone='L_Calf';      bitPos=4074; interp=12; cf=48; nf=8},
    [pscustomobject]@{bone='R_Calf';      bitPos=4416; interp=12; cf=48; nf=8},
    [pscustomobject]@{bone='L_Foot';      bitPos=4758; interp=7;  cf=48; nf=8},
    [pscustomobject]@{bone='R_Foot';      bitPos=4980; interp=7;  cf=48; nf=8},
    [pscustomobject]@{bone='L_Toe';       bitPos=5202; interp=12; cf=55; nf=8},  # all constant
    [pscustomobject]@{bone='R_Toe';       bitPos=5256; interp=12; cf=55; nf=8}   # all constant
)

foreach ($d in $datums) {
    $wInd = ($d.cf -shr 4) -band 3
    $q = @(0.0, 0.0, 0.0, 0.0)

    # Decode 3 stored components in order (skipping wInd slot)
    $bp = $d.bitPos
    $stride = if (($d.cf -band 1) -eq 1) { 16 } else { 16 + $d.interp * $d.nf }

    # flags for each call follow the pattern from the log (upper bits shift)
    # Determine which q[] indices are stored (all except wInd), in order 0,1,2,3 skipping wInd
    $stored = @(0,1,2,3) | Where-Object { $_ -ne $wInd }
    # Each call's flags bit0 tells us constant or not; since all calls within a datum share same constFlags,
    # we determine per-component constancy from the flags values in the log.
    # For cf=55 (0b110111): bits 0-3 = 0111, meaning components at indices 0,1,2 are constant (bit i = 1)
    # For cf=48 (0b110000): bits 0-3 = 0000, all interpolated
    # For cf=16 (0b010000): bits 0-3 = 0000, all interpolated
    # For cf=0  (0b000000): bits 0-3 = 0000, all interpolated
    # Constant flag per stored component: (cf >> compIdx) & 1 where compIdx is the stored index position

    for ($si = 0; $si -lt 3; $si++) {
        $qi = $stored[$si]
        # Per-component constant flag: bit (qi) of lower nibble of cf
        $isConst = (($d.cf -shr $qi) -band 1) -eq 1
        $compStride = if ($isConst) { 16 } else { 16 + $d.interp * $d.nf }
        $q[$qi] = DecodeComponent $bp $isConst $d.interp $d.nf
        $bp += $compStride
    }

    # Reconstruct W from the other three
    $sumSq = $q[0]*$q[0] + $q[1]*$q[1] + $q[2]*$q[2] + $q[3]*$q[3]
    $q[$wInd] = [Math]::Sqrt([Math]::Max(0.0, 1.0 - ($sumSq - $q[$wInd]*$q[$wInd])))

    $x = $q[0]; $y = $q[1]; $z = $q[2]; $w = $q[3]
    $mag = [Math]::Sqrt($x*$x + $y*$y + $z*$z + $w*$w)

    # Euler ZYX (roll=X, pitch=Y, yaw=Z) in degrees
    $sinp = 2.0*($w*$y - $z*$x)
    $sinp = [Math]::Max(-1.0, [Math]::Min(1.0, $sinp))
    $pitch = [Math]::Asin($sinp) * 180.0 / [Math]::PI

    $sinr = 2.0*($w*$x + $y*$z)
    $cosr = 1.0 - 2.0*($x*$x + $y*$y)
    $roll = [Math]::Atan2($sinr, $cosr) * 180.0 / [Math]::PI

    $siny = 2.0*($w*$z + $x*$y)
    $cosy = 1.0 - 2.0*($y*$y + $z*$z)
    $yaw = [Math]::Atan2($siny, $cosy) * 180.0 / [Math]::PI

    Write-Output ("{0,-12}  wInd={1}  q=({2:F4},{3:F4},{4:F4},{5:F4})  |q|={6:F4}  roll={7:F1} pitch={8:F1} yaw={9:F1}" `
        -f $d.bone, $wInd, $x, $y, $z, $w, $mag, $roll, $pitch, $yaw)
}
