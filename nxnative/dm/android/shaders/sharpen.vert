#version 450
// The present's sharpening pass (gfx_vk_present.c sharpen_draw): one triangle over the viewport (the picture's rectangle), uv over the shown part of the game's window
layout(push_constant) uniform P { vec4 src; float sharp; float flip; } pc;   // src: the shown part's origin and size in uv; flip: the window image is stored bottom-up
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vec2 t = p; if (pc.flip > 0.5) t.y = 1.0 - t.y;
    uv = pc.src.xy + t * pc.src.zw;
}
