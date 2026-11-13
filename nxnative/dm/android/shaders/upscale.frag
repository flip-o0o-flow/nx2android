#version 450
// The present's upscale (DM_VK_UPSCALE=gsr, gfx_vk_present.c sharpen_draw): Snapdragon Game Super Resolution 1, the edge-direction variant
// (dm/third_party/snapdragon-gsr/sgsr1_shader_mobile_edge_direction.frag, Copyright (c) 2025 Qualcomm Innovation Center, Inc., BSD-3-Clause), adapted: GLSL 450,
// the window image's size from textureSize, its sharpness (EdgeSharpness, 2.0 upstream) from the push constant, RGBA mode (the green channel as luma)
precision mediump float;
precision highp int;
layout(location = 0) in highp vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D ps0;
layout(push_constant) uniform P { vec4 src; float sharp; float flip; } pc;
#define OperationMode 1
#define EdgeThreshold (8.0 / 255.0)
float fastLanczos2(float x) { float wA = x - 4.0; float wB = x * wA - wA; wA *= wA; return wB * wA; }
vec2 weightY(float dx, float dy, float c, vec3 data) {
    float std = data.x; vec2 dir = data.yz;
    float edgeDis = ((dx * dir.y) + (dy * dir.x));
    float x = (((dx * dx) + (dy * dy)) + ((edgeDis * edgeDis) * ((clamp(((c * c) * std), 0.0, 1.0) * 0.7) + -1.0)));
    float w = fastLanczos2(x); return vec2(w, w * c);
}
vec2 edgeDirection(vec4 left, vec4 right) {
    vec2 dir; float RxLz = (right.x + (-left.z)); float RwLy = (right.w + (-left.y)); vec2 delta;
    delta.x = (RxLz + RwLy); delta.y = (RxLz + (-RwLy));
    float lengthInv = inversesqrt((delta.x * delta.x + 3.075740e-05) + (delta.y * delta.y));
    dir.x = (delta.x * lengthInv); dir.y = (delta.y * lengthInv); return dir;
}
void main() {
    highp vec4 ViewportInfo = vec4(1.0 / vec2(textureSize(ps0, 0)), vec2(textureSize(ps0, 0)));
    vec4 c; c.xyz = textureLod(ps0, uv, 0.0).xyz;
    highp vec2 imgCoord = ((uv * ViewportInfo.zw) + vec2(-0.5, 0.5));
    highp vec2 imgCoordPixel = floor(imgCoord);
    highp vec2 coord = (imgCoordPixel * ViewportInfo.xy);
    vec2 pl = (imgCoord + (-imgCoordPixel));
    vec4 left = textureGather(ps0, coord, OperationMode);
    float edgeVote = abs(left.z - left.y) + abs(c[OperationMode] - left.y) + abs(c[OperationMode] - left.z);
    if (edgeVote > EdgeThreshold) {
        coord.x += ViewportInfo.x;
        vec4 right = textureGather(ps0, coord + vec2(ViewportInfo.x, 0.0), OperationMode);
        vec4 upDown;
        upDown.xy = textureGather(ps0, coord + vec2(0.0, -ViewportInfo.y), OperationMode).wz;
        upDown.zw = textureGather(ps0, coord + vec2(0.0, ViewportInfo.y), OperationMode).yx;
        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean); right = right - vec4(mean); upDown = upDown - vec4(mean);
        c.w = c[OperationMode] - mean;
        float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) + (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) + (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
        float sumMean = 1.014185e+01 / sum; float std = (sumMean * sumMean);
        vec3 data = vec3(std, edgeDirection(left, right));
        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, data);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, data);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, data);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, data);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, data);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, data);
        aWY += weightY(pl.x, pl.y, left.z, data);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, data);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, data);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, data);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, data);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, data);
        float finalY = aWY.y / aWY.x;
        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        float deltaY = clamp(pc.sharp * finalY, minY, maxY) - c.w;
        deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);   // smooth high contrast input
        c.xyz = clamp(c.xyz + deltaY, 0.0, 1.0);
    }
    color = vec4(c.xyz, 1.0);
}
