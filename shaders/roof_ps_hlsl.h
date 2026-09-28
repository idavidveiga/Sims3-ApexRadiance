// Generated from roof_ps.hlsl
#pragma once
static const char* kRoofHlsl = R"RAW(
// Replacement for the game's roof pixel shader (shader_ids.h kRoofPs): identical lighting plus nearby lamps and 16-tap shadows.
float4 c0 : register(c0);   // sun colour
float4 c1 : register(c1);   // sun direction
float4 c2 : register(c2);   // shadow map size, 1/size, 2/size
float4 c3 : register(c3);   // albedo tint
float4 c4 : register(c4);   // specular mask tint
float4 c5 : register(c5);   // alpha ref
float4 c6 : register(c6);   // ambient scale (x)
float4 c7 : register(c7);   // fresnel bias (x)
float4 c8 : register(c8);   // fog mix (x)
float4 lampPos[16] : register(c20); // xyz = lamp head, w = radius
float4 lampCol[16] : register(c36); // rgb = colour x intensity x night fade
float4 lampParams : register(c52);  // x = strength, y = lamp count
samplerCUBE sEnv : register(s0);
samplerCUBE sAmb : register(s1);
sampler2D sAlbedo : register(s2);
sampler2D sSpecMask : register(s3);
sampler2D sTop : register(s4);
sampler2D sShadow : register(s5);
sampler2D sMask : register(s6);
struct PSIn {
    float3 n : TEXCOORD0;
    float4 shadowPos : TEXCOORD1;
    float fade : COLOR1;
    float4 fog : TEXCOORD3;
    float4 uv : TEXCOORD4;      // xy = texture uv, zw = world xz * 0.5
    float4 maskUv : TEXCOORD5;  // xy = mask uv, w = world y
    float3 view : TEXCOORD6;
    float2 topUv : COLOR0;
};
float4 main(PSIn i) : COLOR0 {
    float sum = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++)
            sum += tex2Dproj(sShadow, float4(i.shadowPos.xy + (float2(x, y) - 1.5) * c2.y, i.shadowPos.zw)).x;
    float sh = lerp(sum / 16, 1, i.fade);

    float3 n = normalize(i.n);
    float ndv = dot(n, i.view);
    float3 r = 2 * n * ndv - i.view;
    float ndl = dot(c1.xyz, n);
    float spec = pow(saturate(dot(r, c1.xyz)), 90) * saturate(ndl * 100);
    float f = 1 - ndv;
    float fres = saturate(f * f * f + c7.x);
    float3 refl = texCUBE(sEnv, r).rgb * fres + spec * c0.rgb * sh;
    float3 specMask = tex2D(sSpecMask, i.uv.xy).rgb * c4.rgb;

    float3 p = float3(i.uv.z * 2, i.maskUv.w, i.uv.w * 2);
    float3 lamps = 0;
    [unroll] for (int k = 0; k < 16; k++) {
        float3 l = lampPos[k].xyz - p;
        float d2 = dot(l, l);
        float rr = lampPos[k].w * lampPos[k].w + 1e-3;
        float w = saturate(1 - d2 / rr);
        float wrap = saturate((dot(n, l * rsqrt(d2 + 1e-4)) + 0.5) / 1.5);
        lamps += lampCol[k].rgb * (w * w * wrap);
    }
    lamps *= lampParams.x;

    float3 diffuse = (texCUBE(sAmb, n).rgb * c6.x + saturate(ndl) * c0.rgb * sh + lamps) * tex2D(sMask, i.maskUv.xy).x;
    float2 topUv = n.y >= 0 ? i.topUv : 0;
    float3 albedo = tex2D(sTop, topUv).rgb * tex2D(sAlbedo, i.uv.xy).rgb * c3.rgb;
    float3 col = albedo * diffuse + refl * specMask;
    float lum = dot(refl * specMask + diffuse, float3(0.21, 0.67, 0.12));
    col = lerp(col * c8.x, i.fog.rgb, i.fog.w);
    return float4(col, saturate(lum - c5.x));
}
)RAW";
