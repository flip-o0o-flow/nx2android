#version 450
// Contrast-adaptive sharpening (after AMD FidelityFX CAS): the centre and its four neighbours; the sharpening weight shrinks where the neighbourhood is already near black or
// white, so edges do not ring. sharp 0..1 (DM_VK_SHARPEN / 100)
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform P { vec4 src; float sharp; float flip; } pc;
void main() {
    vec2 px = 1.0 / vec2(textureSize(tex, 0));
    vec3 e = texture(tex, uv).rgb;
    vec3 b = texture(tex, uv + vec2(0.0, -px.y)).rgb, d = texture(tex, uv + vec2(-px.x, 0.0)).rgb;
    vec3 f = texture(tex, uv + vec2(px.x, 0.0)).rgb, h = texture(tex, uv + vec2(0.0, px.y)).rgb;
    vec3 mn = min(e, min(min(b, d), min(f, h))), mx = max(e, max(max(b, d), max(f, h)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 w = amp * (-1.0 / mix(8.0, 5.0, pc.sharp));
    color = vec4(clamp((e + (b + d + f + h) * w) / (1.0 + 4.0 * w), 0.0, 1.0), 1.0);
}
