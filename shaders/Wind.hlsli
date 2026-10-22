// Vegetation wind for meshes marked by RHWind.asi (atomic plugin 0xEDED, as made for SkyGfx Extended).
// Same displacement as SkyGfx Extended's wind vertex shader: value noise over the object's xy position plus a
// scroll that moves with the wind direction, weighted per vertex by 1 - prelight alpha.
#ifndef WIND_HLSLI
#define WIND_HLSLI

cbuffer WindBuffer : register( b12 )
{
    float4 vWind;       // xy = object position + scroll, z = intensity, w = 1 for meshes with wind (0 otherwise)
    float4 vAlphaPass;  // x = 1 in the forward (blended) pass of a dual-pass mesh, y = alpha cutoff (0 = dual pass off),
                        // zw = view depth where the cutoff starts rising / reaches 1 (w = 0: no distance fade)
    float4 vAlphaParams;// x = alpha boost per mip level, yz = view depth where distant dither starts / is complete
                        // (z <= y: off), w = frame number (changes the dither pattern every frame)
};

// Dual pass (RHWind.ini [DualPass]): the G-buffer pass keeps the pixels at or above the cutoff, opaque, and the
// forward pass of the same mesh blends only the pixels below it. Both passes must see the same alpha and cutoff.
// The cutoff and pass come from RHWind (b12) or from RenderHook itself (UseDualPassForAlphaObjects, PerMaterialBuffer).
float DualPassBaseCutoff()
{
    return max( vAlphaPass.y, fAlphaCutoff );
}

bool DualPassBlendedPass()
{
    return vAlphaPass.x > 0.5f || fAlphaBlendPass > 0.5f;
}

bool DualPassGBuffer()
{
    return DualPassBaseCutoff() > 0.0f;
}

// Lower mips average the leaves away; boosting alpha with the mip level keeps distant foliage's coverage.
float DualPassAlpha( float alpha, float mip )
{
    return alpha * ( 1.0f + max( mip, 0.0f ) * vAlphaParams.x );
}

// Far away the cutoff rises past 1, so distant foliage is drawn only by the (smooth) blended pass.
float DualPassCutoff( float viewDepth )
{
    if ( vAlphaPass.w <= vAlphaPass.z )
        return DualPassBaseCutoff();
    return lerp( DualPassBaseCutoff(), 1.01f, saturate( ( viewDepth - vAlphaPass.z ) / ( vAlphaPass.w - vAlphaPass.z ) ) );
}

// Distant dither (RHWind.ini [DualPass] DitherStart/DitherEnd): far foliage goes to the G-buffer as a screen-door
// pattern (like RenderHook's UseDitheringForAlphaObjects) and is not blended at all. The pattern moves every frame
// so TAA averages it into smooth coverage. Between start and end pixels switch over randomly, no visible line.
float DualPassDitherNoise( float2 pixel, float salt )
{
    return InterleavedGradientNoise( pixel + ( vAlphaParams.w + salt ) * 5.588238f );
}

bool DualPassDistant( float2 pixel, float viewDepth )
{
    if ( vAlphaParams.z <= vAlphaParams.y )
        return false;
    return DualPassDitherNoise( pixel, 17.0f ) < saturate( ( viewDepth - vAlphaParams.y ) / ( vAlphaParams.z - vAlphaParams.y ) );
}

// Fully opaque texels always stay in the G-buffer (that also keeps opaque meshes there at any distance).
bool DualPassKeepInGBuffer( float rawAlpha, float mip, float viewDepth, float2 pixel )
{
    if ( rawAlpha >= 0.999f )
        return true;
    if ( DualPassDistant( pixel, viewDepth ) )
        return DualPassAlpha( rawAlpha, mip ) > DualPassDitherNoise( pixel, 0.0f );
    return DualPassAlpha( rawAlpha, mip ) >= DualPassCutoff( viewDepth );
}

bool DualPassSkipForward( float rawAlpha, float mip, float viewDepth, float2 pixel )
{
    if ( !DualPassBlendedPass() )
        return false;
    return DualPassDistant( pixel, viewDepth ) || DualPassKeepInGBuffer( rawAlpha, mip, viewDepth, pixel );
}

float WindHash( float n )
{
    return frac( sin( n ) * 43758.5469f );
}

float WindNoise( float2 x )
{
    float2 p = floor( x );
    float2 f = frac( x );
    f = f * f * ( 3.0f - 2.0f * f );
    float n = p.x + p.y * 57.0f;
    return lerp( lerp( WindHash( n ), WindHash( n + 1.0f ), f.x ),
                 lerp( WindHash( n + 57.0f ), WindHash( n + 58.0f ), f.x ), f.y );
}

// pos: object-space position; prelitAlpha: alpha of the vertex's (day) prelight colour.
float3 ApplyWind( float3 pos, float prelitAlpha )
{
    if ( vWind.w < 0.5f )
        return pos;
    float w   = ( WindNoise( pos.xy + vWind.xy ) - 0.5f ) * vWind.z;
    float amt = 1.0f - prelitAlpha;
    pos.xy += w * amt;
    pos.z  += w * amt * 0.3f;
    return pos;
}

// On wind meshes the prelight alpha is the wind weight, not opacity (SkyGfx takes opacity from the night colour,
// which those models keep at 255), so RenderHook must not fade the leaves with it.
float4 WindVertexColor( float4 prelit )
{
    if ( vWind.w >= 0.5f )
        prelit.a = 1.0f;
    return prelit;
}

#endif
