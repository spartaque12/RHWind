# Adds RHWind to RenderHook's building shaders (shaders\SACustomBuilding.hlsl and SACustomBuildingDN.hlsl).
# Run from the game folder, or pass it: powershell -ExecutionPolicy Bypass -File install-shaders.ps1 "C:\Games\GTA SA"
# Originals are kept as *.before_rhwind. Running it again on patched shaders does nothing.
param([string]$GameDir = $PSScriptRoot)

$ErrorActionPreference = 'Stop'
$shaders = Join-Path $GameDir 'shaders'
if (-not (Test-Path (Join-Path $shaders 'SACustomBuildingDN.hlsl'))) {
    throw "No RenderHook shaders in '$shaders'. Pass the game folder as the first argument."
}
if (-not (Test-Path (Join-Path $shaders 'Wind.hlsli'))) {
    $src = Join-Path $PSScriptRoot 'shaders\Wind.hlsli'
    if (-not (Test-Path $src)) { throw "Wind.hlsli not found; copy it to '$shaders' first." }
    Copy-Item $src $shaders
}

# Lines of inserted code, joined with the file's own line break.
function Lines([string[]]$lines) { return $lines -join '{nl}' }

# Each edit: regex pattern, replacement. {color} = the shader's vertex colour input. Every pattern must match exactly once.
$vsEdits = @(
    @('(#include "Shadows\.hlsl"[^\r\n]*)', (Lines '$1', '#include "Wind.hlsli"')),
    @('float4\(\s*i\.vPosition\s*,\s*1\.0\s*\)(?=;\s*//\s*transform to screen space)', 'float4( ApplyWind( i.vPosition, i.{color}.a ),1.0)'),
    @('o\.vColor(\s*)=(\s*)i\.{color};', 'o.vColor$1=$2WindVertexColor( i.{color} );')
)
$dnEdits = @(
    # forward (alpha-blended) pass: only the pixels the G-buffer pass did not take
    @('(albedoSample\s*\*=\s*txDiffuse\.Sample\(\s*samLinear\s*,\s*i\.vTexCoord\.xy\s*\);)', (Lines @(
        '$1',
        '    float mip = bHasTexture != 0 ? txDiffuse.CalculateLevelOfDetail( samLinear, i.vTexCoord.xy ) : 0.0f;',
        '    if ( DualPassSkipForward( albedoSample.a, mip, i.vNormalDepth.w, i.vPosition.xy ) )',
        '        discard;',
        '    if ( vAlphaPass.x > 0.5f )',
        '        albedoSample.a = saturate( DualPassAlpha( albedoSample.a, mip ) );   // same coverage boost as the G-buffer part'))),
    # shadow map: plain alpha test
    @('(void ShadowPS\(PS_DEFERRED_IN i\)\s*\{\s*#if HAS_TEXTURE == 1\s*float4 outColor = txDiffuse\.Sample\(samLinear, i\.vTexCoord\.xy\);)', (Lines @(
        '$1',
        '    if ( DualPassGBuffer() )',
        '    {',
        '        // plain alpha test: dithered shadow maps make foliage self-shadowing speckled',
        '        if ( outColor.a < vAlphaPass.y )',
        '            discard;',
        '        return;',
        '    }'))),
    # G-buffer pass: alpha test (near) / per-frame dither (far) instead of RenderHook's fixed dither
    @('[ \t]*baseColor\.a = baseColor\.a > 0\.95f \? baseColor\.a : InterleavedGradientNoise\(i\.vPosition\.xy\) \* baseColor\.a;', (Lines @(
        '    if ( DualPassGBuffer() )',
        '    {',
        '        // alpha test instead of dithering; the forward pass blends the rest',
        '        float mip = bHasTexture != 0 ? txDiffuse.CalculateLevelOfDetail( samLinear, i.vTexCoord.xy ) : 0.0f;',
        '        if ( !DualPassKeepInGBuffer( baseColor.a, mip, i.vNormalDepth.w, i.vPosition.xy ) )',
        '            discard;',
        '        baseColor.a = 1.0f;',
        '    }',
        '    else',
        '        baseColor.a = baseColor.a > 0.95f ? baseColor.a : InterleavedGradientNoise(i.vPosition.xy) * baseColor.a;')))
)

# Patch in memory first, write only when both shaders matched.
function Get-Patched([string]$name, [string]$color, $edits) {
    $path = Join-Path $shaders $name
    $text = [IO.File]::ReadAllText($path)
    if ($text.Contains('Wind.hlsli')) { Write-Host "$name already patched"; return $null }
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    foreach ($e in $edits) {
        $pattern = $e[0].Replace('{color}', $color)
        $count = [regex]::Matches($text, $pattern).Count
        if ($count -ne 1) { throw "$name`: expected 1 match, found $count for: $pattern`nThis shader differs from the tested ones (Serendipity 1.0); nothing was changed." }
        $text = [regex]::Replace($text, $pattern, $e[1].Replace('{color}', $color).Replace('{nl}', $nl))
    }
    return @{ Path = $path; Text = $text }
}

$patched = @(
    (Get-Patched 'SACustomBuilding.hlsl' 'cColor' $vsEdits),
    (Get-Patched 'SACustomBuildingDN.hlsl' 'vInColor' ($vsEdits + $dnEdits))
)
foreach ($p in $patched | Where-Object { $_ }) {
    $backup = "$($p.Path).before_rhwind"
    if (-not (Test-Path $backup)) { Copy-Item $p.Path $backup }
    [IO.File]::WriteAllText($p.Path, $p.Text)
    Write-Host "$(Split-Path $p.Path -Leaf) patched (original kept as $(Split-Path $backup -Leaf))"
}
