#version 450

// Volume raymarch, GLSL 450 source. Qt Shader Tools (qsb) bakes this one file
// into a .qsb that carries HLSL for D3D11, SPIR-V for Vulkan, GLSL 150 for
// OpenGL and MSL for Metal, so the GPU path is not D3D11-only (card
// t_9691d611). The uniform block and sampler bindings match the QRhi shader
// resource bindings in volume_canvas.cpp: 0 = Params (std140, ten vec4),
// 1 = the RG8 volume, 2 = the 256-entry palette row.
//
// The dynamic-bound loop below is why the bake targets are GLSL 150, HLSL 5.0
// and MSL 1.2 and not the Qt default GLSL 100 es/120: those require constant
// loop bounds, and maxSteps is a uniform.

layout(std140, binding = 0) uniform Params
{
    vec4 gEye;        // xyz eye, w = focal
    vec4 gForward;    // xyz forward, w = aspect
    vec4 gRight;      // xyz right, w = step meters
    vec4 gUp;         // xyz up, w = max steps
    vec4 gBoxLo;      // xyz brick min corner, w = density per meter
    vec4 gBoxHi;      // xyz brick max corner, w = ambient
    vec4 gLight;      // xyz light direction, w = opacity scale
    vec4 gTexel;      // xyz voxel size in meters, w = viewport width
    vec4 gViewport;   // xy = pixel size
    vec4 gBackground; // rgb = 3D view background
};

// r = value * present, g = present. Hardware trilinear on both channels gives
// sum(w * value) over the corners that carry a gate in r and sum(w) in g, so
// r/g is the renormalised mean the CPU samples and g is how much of the cell
// is known.
layout(binding = 1) uniform sampler3D gVolume;
layout(binding = 2) uniform sampler2D gPalette; // 256-entry ramp in a single row

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 fragColor;

float filteredValue(vec3 uvw)
{
    vec2 s = textureLod(gVolume, uvw, 0.0).rg;
    return s.y > 0.0 ? s.x / s.y : 0.0;
}

void main()
{
    // Pixel-centre ndc comes from the vertex shader (see volume.vert): same
    // pixel-to-ray mapping as the CPU march and the original HLSL -- y = +1 on
    // the top row -- without depending on each backend's gl_FragCoord origin.
    vec2 ndc = vec2(vNdc.x * gForward.w, vNdc.y);
    vec3 dir = normalize(gForward.xyz * gEye.w + gRight.xyz * ndc.x + gUp.xyz * ndc.y);

    vec3 lo = gBoxLo.xyz;
    vec3 hi = gBoxHi.xyz;
    vec3 invDir = 1.0 / dir;
    vec3 t0 = (lo - gEye.xyz) * invDir;
    vec3 t1 = (hi - gEye.xyz) * invDir;
    float tEnter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), max(min(t0.z, t1.z), 0.0));
    float tExit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));

    vec3 accum = vec3(0.0);
    float alpha = 0.0;
    if (tExit > tEnter) {
        float stepMeters = gRight.w;
        int maxSteps = int(gUp.w);
        vec3 span = hi - lo;
        vec3 h = gTexel.xyz;
        float t = tEnter;
        for (int s = 0; s < maxSteps; ++s) {
            if (t >= tExit || alpha >= 0.98) break;
            vec3 uvw = (gEye.xyz + dir * t - lo) / span;
            vec2 volSample = textureLod(gVolume, uvw, 0.0).rg;
            if (volSample.y > 0.0) {
                float value01 = clamp(volSample.x / volSample.y, 0.0, 1.0);
                // Texel centres of the 256-wide ramp.
                vec4 color = textureLod(gPalette,
                                        vec2(value01 * (255.0 / 256.0) + (0.5 / 256.0), 0.5),
                                        0.0);
                float sigma = color.a * volSample.y * gLight.w;
                // Beer-Lambert along the step, exactly like the CPU march: a
                // fixed per-sample alpha saturates into a fog shell.
                float gate = 1.0 - exp(-sigma * gBoxLo.w * stepMeters);
                if (gate > 0.002) {
                    // Gradient of the unnormalised field (value * present),
                    // the same fold brickSurfaceNormal() uses so a hole edge
                    // has its own falloff.
                    float gx = filteredValue(uvw + vec3(h.x, 0.0, 0.0)) -
                               filteredValue(uvw - vec3(h.x, 0.0, 0.0));
                    float gy = filteredValue(uvw + vec3(0.0, h.y, 0.0)) -
                               filteredValue(uvw - vec3(0.0, h.y, 0.0));
                    float gz = filteredValue(uvw + vec3(0.0, 0.0, h.z)) -
                               filteredValue(uvw - vec3(0.0, 0.0, h.z));
                    vec3 normal = vec3(-gx, -gy, -gz);
                    float len = length(normal);
                    float shade = gBoxHi.w;
                    if (len > 1e-9)
                        shade += (1.0 - gBoxHi.w) * max(0.0, dot(normal / len, gLight.xyz));
                    accum += (1.0 - alpha) * color.rgb * shade * gate;
                    alpha += (1.0 - alpha) * gate;
                }
            }
            t += stepMeters;
        }
    }
    vec3 rgb = accum + (1.0 - alpha) * gBackground.rgb;
    fragColor = vec4(rgb, 1.0);
}
