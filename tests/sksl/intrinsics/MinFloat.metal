#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;
struct Uniforms {
    float4 testInputs;
    float4 colorGreen;
    float4 colorRed;
};
struct Inputs {
};
struct Outputs {
    float4 sk_FragColor [[color(0)]];
};



fragment Outputs fragmentMain(Inputs _in [[stage_in]], constant Uniforms& _uniforms [[buffer(0)]], bool _frontFacing [[front_facing]], float4 _fragCoord [[position]]) {
    Outputs _out;
    (void)_out;
    _out.sk_FragColor = ((((((min(_uniforms.testInputs.x, 0.5) == -1.25 && all(min(_uniforms.testInputs.xy, 0.5) == float2(-1.25, 0.0))) && all(min(_uniforms.testInputs.xyz, 0.5) == float3(-1.25, 0.0, 0.5))) && all(min(_uniforms.testInputs, 0.5) == float4(-1.25, 0.0, 0.5, 0.5))) && min(_uniforms.testInputs.x, _uniforms.colorGreen.x) == -1.25) && all(min(_uniforms.testInputs.xy, _uniforms.colorGreen.xy) == float2(-1.25, 0.0))) && all(min(_uniforms.testInputs.xyz, _uniforms.colorGreen.xyz) == float3(-1.25, 0.0, 0.0))) && all(min(_uniforms.testInputs, _uniforms.colorGreen) == float4(-1.25, 0.0, 0.0, 1.0)) ? _uniforms.colorGreen : _uniforms.colorRed;
    return _out;
}
