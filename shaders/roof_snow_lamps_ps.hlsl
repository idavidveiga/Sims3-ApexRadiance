// Extra pass over snowy roofs. The game's winter roof shader (roof_snow_ref.h, 4 noise loops for the snow relief) has no
// lamp light: sun/moon x shadow + ambient cube only. This pass is drawn right after it with additive blend (ONE / ONE)
// and adds nearby lamps on the same snowy albedo the game computes (its first ~20 instructions, rewritten here).
float4 c3 : register(c3);           // albedo tint
float4 c6 : register(c6);           // z = snow amount
float4 c8 : register(c8);           // x = output scale
float4 lampPos[16] : register(c20); // xyz = lamp head, w = radius
float4 lampCol[16] : register(c36); // rgb = colour x intensity x night fade
float4 lampParams : register(c52);  // x = strength
float4 extra : register(c53);       // x = VS c15.x (COLOR0 = world xz / c15.x)
sampler2D sSnow : register(s3);
sampler2D sAlbedo : register(s4);
sampler2D sTop : register(s6);
sampler2D sMask : register(s7);
struct PSIn {
    float4 n : TEXCOORD0;      // xyz = normal, w = 1 on faces that keep no snow
    float4 snowUv : TEXCOORD2; // zw = snow texture uv
    float4 fog : TEXCOORD3;    // w = fog amount
    float4 uv : TEXCOORD4;     // xy = albedo uv
    float4 maskUv : TEXCOORD5; // xy = mask uv, w = world y
    float2 topUv : COLOR0;     // world xz / VS c15.x (the game tiles the roof top texture with it)
};
float4 main(PSIn i) : COLOR0 {
    float snow = saturate(c6.z * 2);
    float cover = saturate(snow * 1.4);
    float3 N = normalize(i.n.xyz);
    float3 a = tex2D(sAlbedo, i.uv.xy).rgb * c3.rgb * tex2D(sTop, N.y >= 0 ? i.topUv : 0).rgb;
    float3 b = saturate((a.r * a.g * a.b * 500 + 0.2) * (snow + 1) + a);
    float3 s = tex2D(sSnow, i.snowUv.zw).rgb;
    float3 c = lerp(min(s, b), s, snow);
    float3 albedo = saturate((1 - i.n.w) * (c - a) + a);
    float mask = lerp(tex2D(sMask, i.maskUv.xy).x, 1, cover);

    // World position: xz from COLOR0 (world / c15.x), y from TEXCOORD5.w. (TEXCOORD4.zw = world xz * VS c19.x is
    // useless: c19 is 0 on some roofs.)
    float3 p = float3(i.topUv.x * extra.x, i.maskUv.w, i.topUv.y * extra.x);
    float3 lamps = 0;
    [unroll] for (int k = 0; k < 16; k++) {
        float3 l = lampPos[k].xyz - p;
        float d2 = dot(l, l);
        float rr = lampPos[k].w * lampPos[k].w + 1e-3;
        float w = saturate(1 - d2 / rr);
        float wrap = saturate((dot(N, l * rsqrt(d2 + 1e-4)) + 0.5) / 1.5);
        lamps += lampCol[k].rgb * (w * w * wrap);
    }
    float3 col = mask * albedo * lamps * lampParams.x * c8.x * (1 - saturate(i.fog.w));
    return float4(col, 0);
}
