# Adds RHWind to RenderHook's building shaders (shaders\SACustomBuilding.hlsl and SACustomBuildingDN.hlsl), and names
# the two PerMaterialBuffer fields RenderHook's own dual pass uses (shaders\Globals.hlsl).
# Run from the game folder, or pass it: powershell -ExecutionPolicy Bypass -File install-shaders.ps1 "C:\Games\GTA SA"
# Originals are kept as *.before_rhwind. Running it again changes nothing; shaders patched by RHWind 1.2-1.4 are
# brought up to date.
param([string]$GameDir = $PSScriptRoot)

$ErrorActionPreference = 'Stop'
$shaders = Join-Path $GameDir 'shaders'
if (-not (Test-Path (Join-Path $shaders 'SACustomBuildingDN.hlsl'))) {
    throw "No RenderHook shaders in '$shaders'. Pass the game folder as the first argument."
}
$windSrc = Join-Path $PSScriptRoot 'shaders\Wind.hlsli'
if (-not (Test-Path $windSrc)) { throw "shaders\Wind.hlsli not found next to this script." }

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
        '    if ( DualPassBlendedPass() )',
        '        albedoSample.a = saturate( DualPassAlpha( albedoSample.a, mip ) );   // same coverage boost as the G-buffer part'))),
    # shadow map: plain alpha test
    @('(void ShadowPS\(PS_DEFERRED_IN i\)\s*\{\s*#if HAS_TEXTURE == 1\s*float4 outColor = txDiffuse\.Sample\(samLinear, i\.vTexCoord\.xy\);)', (Lines @(
        '$1',
        '    if ( DualPassGBuffer() )',
        '    {',
        '        // plain alpha test: dithered shadow maps make foliage self-shadowing speckled',
        '        if ( outColor.a < DualPassBaseCutoff() )',
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
# Shaders patched by RHWind 1.2-1.4 read the dual-pass values from b12 only; 1.5 also takes RenderHook's own.
$dnUpgrades = @(
    @('    if ( vAlphaPass.x > 0.5f )', '    if ( DualPassBlendedPass() )'),
    @('        if ( outColor.a < vAlphaPass.y )', '        if ( outColor.a < DualPassBaseCutoff() )')
)
# RenderHook's dual pass (UseDualPassForAlphaObjects) passes its cutoff in PerMaterialBuffer's padding; Wind.hlsli reads it.
$globalsEdit = @('float2\s+___padding\s*:\s*packoffset\(\s*c2\s*\)\s*;', (Lines @(
    'float  fAlphaCutoff : packoffset( c2.x );    // RenderHook dual-pass alpha objects: alpha test value, 0 = off',
    '    float  fAlphaBlendPass : packoffset( c2.y ); // 1 in the blended pass of a dual-pass mesh')))

function Edit-Text([string]$name, [string]$text, $edits, [string]$color) {
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    foreach ($e in $edits) {
        $pattern = $e[0].Replace('{color}', $color)
        $count = [regex]::Matches($text, $pattern).Count
        if ($count -ne 1) { throw "$name`: expected 1 match, found $count for: $pattern`nThis shader differs from the tested ones (Serendipity 1.0); nothing was changed." }
        $text = [regex]::Replace($text, $pattern, $e[1].Replace('{color}', $color).Replace('{nl}', $nl))
    }
    return $text
}

# Patch in memory first, write only when every shader matched. Returns $null when nothing changes.
function Get-Patched([string]$name, [string]$color, $edits, $upgrades) {
    $path = Join-Path $shaders $name
    $original = [IO.File]::ReadAllText($path)
    if ($original.Contains('Wind.hlsli')) {
        $text = $original
        foreach ($u in $upgrades) { $text = $text.Replace($u[0], $u[1]) }
    } else {
        $text = Edit-Text $name $original $edits $color
    }
    if ($text -eq $original) { Write-Host "$name already up to date"; return $null }
    return @{ Path = $path; Text = $text }
}

function Get-PatchedGlobals {
    $path = Join-Path $shaders 'Globals.hlsl'
    $original = [IO.File]::ReadAllText($path)
    if ($original.Contains('fAlphaCutoff')) { Write-Host 'Globals.hlsl already up to date'; return $null }
    return @{ Path = $path; Text = (Edit-Text 'Globals.hlsl' $original @(, $globalsEdit) '') }
}

$patched = @(
    (Get-PatchedGlobals),
    (Get-Patched 'SACustomBuilding.hlsl' 'cColor' $vsEdits @()),
    (Get-Patched 'SACustomBuildingDN.hlsl' 'vInColor' ($vsEdits + $dnEdits) $dnUpgrades)
)
foreach ($p in $patched | Where-Object { $_ }) {
    $backup = "$($p.Path).before_rhwind"
    if (-not (Test-Path $backup)) { Copy-Item $p.Path $backup }
    [IO.File]::WriteAllText($p.Path, $p.Text)
    Write-Host "$(Split-Path $p.Path -Leaf) patched (original kept as $(Split-Path $backup -Leaf))"
}
Copy-Item $windSrc $shaders -Force
Write-Host 'Wind.hlsli installed'
