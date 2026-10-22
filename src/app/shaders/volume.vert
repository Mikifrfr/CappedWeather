#version 450

// Full-screen triangle, no vertex buffer: (0,0) (2,0) (0,2) in uv space.
// The screen-space ndc is passed to the fragment shader as a varying instead
// of being derived from gl_FragCoord there: gl_FragCoord.y is bottom-up in
// GLSL but top-down once qsb translates the shader to HLSL for D3D11, and the
// same source has to shoot the same rays on every backend. gl_Position.y is
// +1 at the top row on all of them, and the rasterizer interpolates at pixel
// centres, so vNdc is exactly the pixel-centre ndc the old HLSL used
// (pos.x / width * 2 - 1, 1 - pos.y / height * 2).
layout(location = 0) out vec2 vNdc;

void main()
{
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
    vNdc = gl_Position.xy;
}
