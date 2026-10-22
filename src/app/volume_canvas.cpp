#include "app/volume_canvas.hpp"

#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QOffscreenSurface>
#include <QThread>
#include <QVarLengthArray>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <cstdio>

namespace cappedweather {
namespace {

// Phase 1 constants, copied from RadarMapCanvas::rebuildVolumeRender. They are
// duplicated rather than shared because the CPU march keeps them inside a
// worker lambda; if one of these changes the GPU image must change with it and
// tools/volume3d_probe is what shows the drift.
constexpr float kAmbient = 0.46F;
constexpr float kLightX = -0.5F;
constexpr float kLightY = -0.5F;
constexpr float kLightZ = 0.7071F;
constexpr float kDensityPerMeter = 1.2e-3F;
// The 3D view background painted under the CPU image (RadarMapCanvas::
// paintVolume3d). The shader composites over it, so GPU and CPU frames are
// comparable pixel for pixel instead of differing in the empty area.
constexpr float kBackground[3] = {8.0F / 255.0F, 13.0F / 255.0F, 16.0F / 255.0F};

// March geometry, identical to the CPU path: half the largest voxel span, over
// the box diagonal.
struct MarchPlan {
    float stepMeters = 1.0F;
    int maxSteps = 1;
    float spanX = 1.0F;
    float spanY = 1.0F;
    float spanZ = 1.0F;
};

MarchPlan marchPlanFor(const VolumeBrick& brick) {
    MarchPlan plan;
    plan.spanX = static_cast<float>(2.0 * brick.halfExtentMeters / std::max(1, brick.nx));
    plan.spanY = static_cast<float>(2.0 * brick.halfExtentMeters / std::max(1, brick.ny));
    plan.spanZ = static_cast<float>(brick.topMeters / std::max(1, brick.nz));
    plan.stepMeters = std::max({plan.spanX, plan.spanY, plan.spanZ}) * 0.5F;
    const double diagonal =
        std::sqrt(8.0 * brick.halfExtentMeters * brick.halfExtentMeters +
                  brick.topMeters * brick.topMeters);
    plan.maxSteps = static_cast<int>(diagonal / plan.stepMeters) + 2;
    return plan;
}

// --- shader -------------------------------------------------------------

// The shipped shader is GLSL in src/app/shaders/volume.{vert,frag}. Qt Shader
// Tools bakes each into a .qsb that carries SPIR-V (Vulkan), GLSL 150 (OpenGL),
// HLSL 5.0 (D3D11) and MSL 1.2 (Metal), and QRhi picks the flavour its backend
// asks for. That is what makes the GPU path cross-API (card t_9691d611).
//
// The HLSL below is the fallback for a kit built without Qt Shader Tools: no
// qsb, so D3D11 is the only backend that can start, and create() says so
// instead of pretending. It is the same math as the GLSL source, line for
// line; tools/volume3d_probe compares the two images.
const char* const kVolumeHlsl = R"HLSL(
cbuffer Params : register(b0)
{
    float4 gEye;       // xyz eye, w = focal
    float4 gForward;   // xyz forward, w = aspect
    float4 gRight;     // xyz right, w = step meters
    float4 gUp;        // xyz up, w = max steps
    float4 gBoxLo;     // xyz brick min corner, w = density per meter
    float4 gBoxHi;     // xyz brick max corner, w = ambient
    float4 gLight;     // xyz light direction, w = opacity scale
    float4 gTexel;     // xyz voxel size in meters, w = viewport width
    float4 gViewport;  // xy = pixel size
    float4 gBackground; // rgb = 3D view background
};

// r = value * present, g = present. Hardware trilinear on both channels gives
// sum(w * value) over the corners that carry a gate in r and sum(w) in g, so
// r/g is the renormalised mean the CPU samples and g is how much of the cell
// is known.
Texture3D<float2> gVolume : register(t1);
SamplerState gVolumeSampler : register(s1);
Texture2D<float4> gPalette : register(t2); // 256-entry ramp in a single row
SamplerState gPaletteSampler : register(s2);

struct VSOut { float4 pos : SV_Position; };

VSOut vsMain(uint id : SV_VertexID)
{
    // Full-screen triangle, no vertex buffer.
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float filteredValue(float3 uvw)
{
    float2 s = gVolume.SampleLevel(gVolumeSampler, uvw, 0);
    return s.y > 0.0 ? s.x / s.y : 0.0;
}

float4 psMain(float4 pos : SV_Position) : SV_Target
{
    // Same pixel-to-ray mapping as the CPU march: pixel centres, aspect on x,
    // y flipped, 45-degree vertical field.
    float2 ndc;
    ndc.x = (pos.x / gViewport.x * 2.0 - 1.0) * gForward.w;
    ndc.y = 1.0 - pos.y / gViewport.y * 2.0;
    float3 dir = gForward.xyz * gEye.w + gRight.xyz * ndc.x + gUp.xyz * ndc.y;
    dir = normalize(dir);

    float3 lo = gBoxLo.xyz;
    float3 hi = gBoxHi.xyz;
    float3 invDir = 1.0 / dir;
    float3 t0 = (lo - gEye.xyz) * invDir;
    float3 t1 = (hi - gEye.xyz) * invDir;
    float tEnter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), max(min(t0.z, t1.z), 0.0));
    float tExit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));

    float3 accum = float3(0.0, 0.0, 0.0);
    float alpha = 0.0;
    if (tExit > tEnter) {
        const float stepMeters = gRight.w;
        const int maxSteps = (int)gUp.w;
        const float3 span = hi - lo;
        const float3 h = gTexel.xyz;
        float t = tEnter;
        [loop]
        for (int s = 0; s < maxSteps; ++s) {
            if (t >= tExit || alpha >= 0.98) break;
            float3 uvw = (gEye.xyz + dir * t - lo) / span;
            float2 volSample = gVolume.SampleLevel(gVolumeSampler, uvw, 0);
            if (volSample.y > 0.0) {
                float value01 = saturate(volSample.x / volSample.y);
                // Texel centres of the 256-wide ramp.
                float4 color = gPalette.SampleLevel(
                    gPaletteSampler,
                    float2(value01 * (255.0 / 256.0) + (0.5 / 256.0), 0.5), 0);
                float sigma = color.a * volSample.y * gLight.w;
                // Beer-Lambert along the step, exactly like the CPU march: a
                // fixed per-sample alpha saturates into a fog shell.
                float gate = 1.0 - exp(-sigma * gBoxLo.w * stepMeters);
                if (gate > 0.002) {
                    // Gradient of the unnormalised field (value * present),
                    // the same fold brickSurfaceNormal() uses so a hole edge
                    // has its own falloff.
                    float gx = filteredValue(uvw + float3(h.x, 0.0, 0.0)) -
                               filteredValue(uvw - float3(h.x, 0.0, 0.0));
                    float gy = filteredValue(uvw + float3(0.0, h.y, 0.0)) -
                               filteredValue(uvw - float3(0.0, h.y, 0.0));
                    float gz = filteredValue(uvw + float3(0.0, 0.0, h.z)) -
                               filteredValue(uvw - float3(0.0, 0.0, h.z));
                    float3 normal = float3(-gx, -gy, -gz);
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
    float3 rgb = accum + (1.0 - alpha) * gBackground.rgb;
    return float4(rgb, 1.0);
}
)HLSL";

QShader hlslShader(const QShader::Stage stage, const char* entryPoint) {
    QShader shader;
    shader.setStage(stage);
    const QShaderCode code{QByteArray(kVolumeHlsl), QByteArray(entryPoint)};
    // QRhi's D3D11 backend asks for the HLSL profile that matches the device
    // feature level; both keys carry the same source.
    shader.setShader(QShaderKey(QShader::HlslShader, 50), code);
    shader.setShader(QShaderKey(QShader::HlslShader, 51), code);
    return shader;
}

// One pair of stages a backend can consume. `label` names the source in the
// status line ("qsb" or "HLSL") so a support report says what actually ran.
struct ShaderSet {
    QShader vertex;
    QShader fragment;
    QString label;
    QString error;
    bool ok = false;
};

#ifdef CAPPEDWEATHER_HAS_VOLUME_QSB
bool loadBakedShader(const QString& path, QShader* out) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray blob = file.readAll();
    if (blob.isEmpty()) return false;
    const QShader shader = QShader::fromSerialized(blob);
    if (!shader.isValid()) return false;
    *out = shader;
    return true;
}
#endif

// The baked set is preferred on every backend: D3D11 gets the HLSL variant of
// the same GLSL source, OpenGL gets GLSL 150, Vulkan SPIR-V, Metal MSL. Only a
// build without Shader Tools falls back to the embedded HLSL string, and that
// fallback is D3D11-only by construction.
ShaderSet volumeShaderSet(const QRhi::Implementation backend, const QString& backendName) {
    ShaderSet set;
    // Deliberate-failure hook for the fallback proof in
    // docs/gpu-volume-rendering.md ("Failure behaviour"): a shader the backend
    // cannot consume must give a one-line error and a CPU frame, never a black
    // panel. Read once, never set by the app itself. The blob is not a shader in
    // any language, so every backend refuses it in its own words.
    static const bool broken = qEnvironmentVariableIsSet("CAPPEDWEATHER_VOLUME_BREAK_SHADER");
    if (broken) {
        const auto invalid = [](const QShader::Stage stage) {
            QShader shader;
            shader.setStage(stage);
            shader.setShader(QShaderKey(QShader::SpirvShader, 100),
                             QShaderCode(QByteArrayLiteral("this is not a shader"),
                                         QByteArrayLiteral("main")));
            return shader;
        };
        set.vertex = invalid(QShader::VertexStage);
        set.fragment = invalid(QShader::FragmentStage);
        set.label = QStringLiteral("broken");
        set.ok = true; // create() fails; the caller reports the captured reason
        return set;
    }
#ifdef CAPPEDWEATHER_HAS_VOLUME_QSB
    if (loadBakedShader(QStringLiteral(":/shaders/volume.vert.qsb"), &set.vertex) &&
        loadBakedShader(QStringLiteral(":/shaders/volume.frag.qsb"), &set.fragment)) {
        set.label = QStringLiteral("qsb");
        set.ok = true;
        return set;
    }
    set.error = QStringLiteral("the baked volume shader is missing from this build's resources");
#endif
    if (backend == QRhi::D3D11) {
        // Kit without Qt Shader Tools: the embedded HLSL string is the only set
        // available, and only the D3D11 backend can compile it at runtime.
        set.label = QStringLiteral("HLSL");
        set.error.clear();
        set.vertex = hlslShader(QShader::VertexStage, "vsMain");
        set.fragment = hlslShader(QShader::FragmentStage, "psMain");
        set.ok = true;
        return set;
    }
    if (set.error.isEmpty())
        set.error = QStringLiteral("GPU 3D has no shader set for %1: this build has no Qt Shader Tools")
                        .arg(backendName);
    return set;
}

// --- backend choice ------------------------------------------------------
//
// CAPPEDWEATHER_VOLUME_RHI=opengl|vulkan|d3d11|d3d12|metal forces the QRhi
// backend for the volume widget, the resident render thread and the offscreen
// probe, so one binary can be measured on every API the machine has. Unset
// keeps the platform default: Direct3D 11 on Windows, OpenGL elsewhere.
QString volumeRhiOverride() {
    return qEnvironmentVariable("CAPPEDWEATHER_VOLUME_RHI").trimmed().toLower();
}

bool volumeRhiOverrideIs(const char* name) {
    const QString value = volumeRhiOverride();
    const QString wanted = QString::fromLatin1(name);
    if (value == wanted) return true;
    if (wanted == QLatin1String("d3d11")) return value == QLatin1String("direct3d11");
    if (wanted == QLatin1String("opengl"))
        return value == QLatin1String("gl") || value == QLatin1String("opengl32") ||
               value == QLatin1String("gles");
    return false;
}

// --- error text ---------------------------------------------------------

// QRhi reports shader compile and pipeline failures through qWarning. Swallow
// them (per thread) while creating resources so the widget can put the real
// reason in its status line instead of "GPU failed".
thread_local bool t_captureActive = false;
thread_local QStringList t_captured;

void captureHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (t_captureActive && type != QtDebugMsg && type != QtInfoMsg) {
        t_captured.append(message);
        return;
    }
    // Never swallow Qt's own logging: forward it while we are not collecting.
    std::fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
}

void beginCapture() {
    static std::once_flag once;
    std::call_once(once, [] { qInstallMessageHandler(captureHandler); });
    t_captured.clear();
    t_captureActive = true;
}

QString endCapture() {
    t_captureActive = false;
    QStringList messages = t_captured;
    t_captured.clear();
    if (messages.isEmpty()) return {};
    // One line: the first failure is the useful one, the rest is echo.
    QString first = messages.first().simplified();
    if (first.size() > 220) first = first.left(217) + QStringLiteral("...");
    return first;
}

// --- palette ------------------------------------------------------------

QByteArray buildLut(const std::vector<RgbaStop>& stops) {
    QByteArray bytes(256 * 4, '\0');
    auto* out = reinterpret_cast<unsigned char*>(bytes.data());
    for (int index = 0; index < 256; ++index) {
        const float value01 = static_cast<float>(index) / 255.0F;
        const Rgba color = evaluateTransfer(stops, value01);
        const auto byte = [](const float channel) {
            return static_cast<unsigned char>(
                std::clamp(static_cast<int>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F)),
                           0, 255));
        };
        out[index * 4 + 0] = byte(color.r);
        out[index * 4 + 1] = byte(color.g);
        out[index * 4 + 2] = byte(color.b);
        out[index * 4 + 3] = byte(color.a);
    }
    return bytes;
}

// r = value * present, g = present, one entry per depth slice (QRhi's D3D11
// backend uploads 3D textures slice by slice; a single whole-volume entry
// silently produced an empty texture here).
QByteArray buildVolumeBytes(const VolumeBrick& brick, int* sliceBytes) {
    const int nx = std::max(1, brick.nx);
    const int ny = std::max(1, brick.ny);
    const int nz = std::max(1, brick.nz);
    const qsizetype slice = static_cast<qsizetype>(nx) * ny * 2;
    *sliceBytes = static_cast<int>(slice);
    QByteArray bytes(slice * nz, '\0');
    auto* out = reinterpret_cast<unsigned char*>(bytes.data());
    const std::size_t voxels =
        static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) * static_cast<std::size_t>(nz);
    const bool havePresent = brick.present.size() >= voxels;
    const bool haveValue = brick.value.size() >= voxels;
    for (std::size_t index = 0; index < voxels && haveValue; ++index) {
        const bool present = havePresent ? brick.present[index] != 0 : true;
        out[index * 2 + 0] = present ? static_cast<unsigned char>(brick.value[index]) : 0;
        out[index * 2 + 1] = present ? 255 : 0;
    }
    return bytes;
}

// --- renderer -----------------------------------------------------------

class RhiVolumeRenderer {
public:
    ~RhiVolumeRenderer() { destroy(); }

    bool create(QRhi* rhi, QRhiRenderPassDescriptor* renderPass, QString* error) {
        rhi_ = rhi;
        const QRhi::Implementation impl = rhi_->backend();
        const QString backendName = QString::fromUtf8(rhi_->backendName());
        // Only the backends the baked .qsb set can feed: HLSL 5.0 for D3D11,
        // GLSL 150 for OpenGL, SPIR-V for Vulkan, MSL for Metal. D3D12 wants
        // HLSL 6.x/DXIL stages the bake does not produce, and Null has no
        // device. Everything else is refused with the backend named.
        if (impl != QRhi::D3D11 && impl != QRhi::OpenGLES2 && impl != QRhi::Vulkan &&
            impl != QRhi::Metal) {
            *error = QStringLiteral("GPU 3D has no shader set for the %1 backend").arg(backendName);
            rhi_ = nullptr;
            return false;
        }
        shaders_ = volumeShaderSet(impl, backendName);
        if (!shaders_.ok) {
            *error = shaders_.error;
            rhi_ = nullptr;
            return false;
        }
        renderPass_ = renderPass;
        return true;
    }

    void destroy() {
        if (pipeline_) {
            pipeline_->destroy();
            delete pipeline_;
            pipeline_ = nullptr;
        }
        if (srb_) {
            srb_->destroy();
            delete srb_;
            srb_ = nullptr;
        }
        if (lutSampler_) {
            lutSampler_->destroy();
            delete lutSampler_;
            lutSampler_ = nullptr;
        }
        if (lut_) {
            lut_->destroy();
            delete lut_;
            lut_ = nullptr;
        }
        if (volumeSampler_) {
            volumeSampler_->destroy();
            delete volumeSampler_;
            volumeSampler_ = nullptr;
        }
        if (volume_) {
            volume_->destroy();
            delete volume_;
            volume_ = nullptr;
            volumeGrid_ = QSize();
            volumeDepth_ = 0;
        }
        if (ubuf_) {
            ubuf_->destroy();
            delete ubuf_;
            ubuf_ = nullptr;
        }
        rhi_ = nullptr;
        renderPass_ = nullptr;
    }

    void setBrick(std::shared_ptr<const VolumeBrick> brick) {
        if (brick == brick_) return;
        brick_ = std::move(brick);
        volumeDirty_ = true;
        if (brick_) {
            const MarchPlan plan = marchPlanFor(*brick_);
            plan_ = plan;
        }
    }

    void setStops(std::vector<RgbaStop> stops) {
        stops_ = std::move(stops);
        lutDirty_ = true;
    }

    void setView(const VolumeCamera& camera, const QSize& pixelSize, int opacityPercent) {
        camera_ = camera;
        pixelSize_ = pixelSize;
        opacityPercent_ = opacityPercent;
    }

    // True once the brick's 3D texture exists, i.e. a frame can be drawn.
    bool ready() const { return pipeline_ != nullptr; }

    // "qsb" or "HLSL": which shader source this device resolved.
    QString shaderLabel() const { return shaders_.label; }

    bool render(QRhiCommandBuffer* cb, QString* error) {
        if (!rhi_ || !cb || !brick_ || pixelSize_.isEmpty()) return false;
        if (!renderPass_) return false;

        QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
        if (volumeDirty_) {
            int sliceBytes = 0;
            volumeBytes_ = buildVolumeBytes(*brick_, &sliceBytes);
            const int nz = std::max(1, brick_->nz);
            const QSize grid(std::max(1, brick_->nx), std::max(1, brick_->ny));
            // The 3D texture is sized by the brick. A brick with a new grid (a
            // different box side, or the first frame after a product change)
            // needs a new texture: uploading a byte array shaped for the new
            // grid into a texture still sized for the old one makes D3D11 walk
            // the old row pitch off the end of the buffer - an access violation
            // inside the QRhi batch, not an error the caller can catch.
            if (volume_ && (volumeGrid_ != grid || volumeDepth_ != nz)) {
                volume_->destroy();
                delete volume_;
                volume_ = nullptr;
                // The SRB binds the old texture by pointer and the pipeline binds
                // the SRB, so both are rebuilt with the replacement. Without this
                // the draw samples a deleted texture: a black frame at best.
                if (srb_) {
                    srb_->destroy();
                    delete srb_;
                    srb_ = nullptr;
                }
                if (pipeline_) {
                    pipeline_->destroy();
                    delete pipeline_;
                    pipeline_ = nullptr;
                }
            }
            if (!volume_) {
                volume_ = rhi_->newTexture(QRhiTexture::RG8, grid.width(), grid.height(), nz, 1,
                                           QRhiTexture::ThreeDimensional);
                if (!volume_ || !volume_->create()) {
                    *error = QStringLiteral("3D texture creation failed (%1x%2x%3)")
                                 .arg(grid.width())
                                 .arg(grid.height())
                                 .arg(nz);
                    volume_ = nullptr;
                    return false;
                }
                volumeGrid_ = grid;
                volumeDepth_ = nz;
            }
            QVarLengthArray<QRhiTextureUploadEntry, 160> entries;
            for (int slice = 0; slice < nz; ++slice) {
                entries.append(QRhiTextureUploadEntry(
                    slice, 0,
                    QRhiTextureSubresourceUploadDescription(
                        volumeBytes_.constData() + static_cast<qsizetype>(slice) * sliceBytes,
                        static_cast<quint32>(sliceBytes))));
            }
            QRhiTextureUploadDescription description;
            description.setEntries(entries.begin(), entries.end());
            updates->uploadTexture(volume_, description);
            if (!volumeSampler_) {
                volumeSampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                                  QRhiSampler::None, QRhiSampler::ClampToEdge,
                                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
                volumeSampler_->create();
            }
            volumeDirty_ = false;
        }
        if (lutDirty_ || !lut_) {
            lutBytes_ = buildLut(stops_);
            if (!lut_) {
                // A 1D LUT in a single-row 2D texture: QRhi's D3D11 backend
                // refuses QRhiTexture::OneDimensional here.
                lut_ = rhi_->newTexture(QRhiTexture::RGBA8, QSize(256, 1));
                if (!lut_->create()) {
                    *error = QStringLiteral("palette LUT texture creation failed");
                    return false;
                }
            }
            updates->uploadTexture(
                lut_, QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                          0, 0, QRhiTextureSubresourceUploadDescription(lutBytes_))));
            if (!lutSampler_) {
                lutSampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                               QRhiSampler::None, QRhiSampler::ClampToEdge,
                                               QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
                lutSampler_->create();
            }
            lutDirty_ = false;
        }
        if (!ubuf_) {
            ubuf_ = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(params_));
            if (!ubuf_->create()) {
                *error = QStringLiteral("uniform buffer creation failed");
                return false;
            }
        }
        fillParams();
        updates->updateDynamicBuffer(ubuf_, 0, sizeof(params_), params_);

        if (!srb_) {
            srb_ = rhi_->newShaderResourceBindings();
            srb_->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0, QRhiShaderResourceBinding::FragmentStage, ubuf_),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage, volume_, volumeSampler_),
                QRhiShaderResourceBinding::sampledTexture(
                    2, QRhiShaderResourceBinding::FragmentStage, lut_, lutSampler_),
            });
            if (!srb_->create()) {
                *error = QStringLiteral("shader resource bindings failed");
                return false;
            }
        }
        if (!pipeline_) {
            beginCapture();
            pipeline_ = rhi_->newGraphicsPipeline();
            pipeline_->setShaderStages(
                {{QRhiShaderStage::Vertex, shaders_.vertex},
                 {QRhiShaderStage::Fragment, shaders_.fragment}});
            pipeline_->setVertexInputLayout({});
            pipeline_->setShaderResourceBindings(srb_);
            pipeline_->setRenderPassDescriptor(renderPass_);
            pipeline_->setDepthTest(false);
            pipeline_->setCullMode(QRhiGraphicsPipeline::None);
            pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
            const bool created = pipeline_->create();
            const QString captured = endCapture();
            if (!created) {
                delete pipeline_;
                pipeline_ = nullptr;
                *error = captured.isEmpty() ? QStringLiteral("shader pipeline creation failed")
                                            : captured;
                return false;
            }
        }

        cb->resourceUpdate(updates);
        cb->setGraphicsPipeline(pipeline_);
        cb->setShaderResources(srb_);
        cb->setViewport(QRhiViewport(0.0F, 0.0F, static_cast<float>(pixelSize_.width()),
                                     static_cast<float>(pixelSize_.height())));
        cb->draw(3);
        return true;
    }

    void setRenderPass(QRhiRenderPassDescriptor* pass) {
        if (pass == renderPass_) return;
        renderPass_ = pass;
        if (pipeline_) {
            pipeline_->destroy();
            delete pipeline_;
            pipeline_ = nullptr;
        }
    }

private:
    void fillParams() {
        const VolumeBrick& brick = *brick_;
        const double aspect = static_cast<double>(pixelSize_.width()) /
                              std::max(1, pixelSize_.height());
        auto put = [this](const int slot, const float x, const float y, const float z, const float w) {
            params_[slot * 4 + 0] = x;
            params_[slot * 4 + 1] = y;
            params_[slot * 4 + 2] = z;
            params_[slot * 4 + 3] = w;
        };
        put(0, static_cast<float>(camera_.eye[0]), static_cast<float>(camera_.eye[1]),
            static_cast<float>(camera_.eye[2]), static_cast<float>(camera_.focal));
        put(1, static_cast<float>(camera_.forward[0]), static_cast<float>(camera_.forward[1]),
            static_cast<float>(camera_.forward[2]), static_cast<float>(aspect));
        put(2, static_cast<float>(camera_.right[0]), static_cast<float>(camera_.right[1]),
            static_cast<float>(camera_.right[2]), plan_.stepMeters);
        put(3, static_cast<float>(camera_.up[0]), static_cast<float>(camera_.up[1]),
            static_cast<float>(camera_.up[2]), static_cast<float>(plan_.maxSteps));
        put(4, static_cast<float>(brick.centerXMeters - brick.halfExtentMeters),
            static_cast<float>(brick.centerYMeters - brick.halfExtentMeters), 0.0F,
            kDensityPerMeter);
        put(5, static_cast<float>(brick.centerXMeters + brick.halfExtentMeters),
            static_cast<float>(brick.centerYMeters + brick.halfExtentMeters),
            static_cast<float>(brick.topMeters), kAmbient);
        put(6, kLightX, kLightY, kLightZ, static_cast<float>(opacityPercent_) / 100.0F);
        put(7, plan_.spanX, plan_.spanY, plan_.spanZ, static_cast<float>(pixelSize_.width()));
        put(8, static_cast<float>(pixelSize_.width()),
            static_cast<float>(pixelSize_.height()), 0.0F, 0.0F);
        put(9, kBackground[0], kBackground[1], kBackground[2], 1.0F);
    }

    QRhi* rhi_ = nullptr;
    QRhiRenderPassDescriptor* renderPass_ = nullptr;
    QRhiBuffer* ubuf_ = nullptr;
    QRhiTexture* volume_ = nullptr;
    // Grid the resident 3D texture was created for: a brick with a different
    // grid must replace the texture, not upload a differently-shaped buffer
    // into the old one (see render()).
    QSize volumeGrid_;
    int volumeDepth_ = 0;
    QRhiSampler* volumeSampler_ = nullptr;
    QRhiTexture* lut_ = nullptr;
    QRhiSampler* lutSampler_ = nullptr;
    QRhiShaderResourceBindings* srb_ = nullptr;
    QRhiGraphicsPipeline* pipeline_ = nullptr;
    // Resolved once per device in create(): the baked .qsb (or HLSL fallback)
    // pair this backend consumes.
    ShaderSet shaders_;
    std::shared_ptr<const VolumeBrick> brick_;
    std::vector<RgbaStop> stops_;
    VolumeCamera camera_;
    MarchPlan plan_;
    QSize pixelSize_;
    int opacityPercent_ = 80;
    bool volumeDirty_ = false;
    bool lutDirty_ = true;
    QByteArray volumeBytes_;
    QByteArray lutBytes_;
    float params_[40] = {};
};

// --- resident device -----------------------------------------------------
//
// Panel integration (card t_f71a0be0) needs a frame without a window and without
// paying device creation per frame: 91-132 ms for a D3D11 device, against 4-8 ms
// for the march itself. So one QRhi device, one offscreen render target and the
// uploaded 3D texture live for the process, owned by one thread. QRhi has thread
// affinity, so RhiVolumeScene is only ever touched inside GpuFrameThread::run().

class RhiVolumeScene {
public:
    RhiVolumeScene() = default;
    ~RhiVolumeScene() {
        releaseTarget();
        renderer_.reset();   // textures, buffers, pipeline: before the device
        rhi_.reset();
        delete glFallbackSurface_;   // the GL context is gone with rhi_
        glFallbackSurface_ = nullptr;
    }
    RhiVolumeScene(const RhiVolumeScene&) = delete;
    RhiVolumeScene& operator=(const RhiVolumeScene&) = delete;

    qint64 frameCount() const { return frameCount_; }
    qint64 uploadCount() const { return uploadCount_; }

    // One frame with the device (and normally the volume texture) already
    // resident. frameMs is the march + readback + the wait for the GPU; the
    // caller's firstFrameMs is the whole call, which is where a cold device and
    // a fresh 16.8 MB upload show up.
    VolumeGpuFrame renderFrame(const std::shared_ptr<const VolumeBrick>& brick,
                               const VolumeCamera& camera, const std::vector<RgbaStop>& stops,
                               const int opacityPercent, const QSize& pixelSize) {
        VolumeGpuFrame frame;
        if (pixelSize.isEmpty()) {
            frame.error = QStringLiteral("GPU 3D offscreen render needs a non-empty size");
            return frame;
        }
        if (!brick || brick->value.empty()) {
            frame.error = QStringLiteral("GPU 3D has no brick to march");
            return frame;
        }
        QElapsedTimer call;
        call.start();
        if (!ensureDevice(&frame.error)) return frame;
        frame.backend = backend_;
        frame.device = device_;
        if (!ensureTarget(pixelSize, &frame.error)) return frame;
        frame.shader = renderer_ ? renderer_->shaderLabel() : QString();

        renderer_->setView(camera, pixelSize, opacityPercent);
        renderer_->setStops(stops);
        if (brick != uploadedBrick_) {
            // RhiVolumeRenderer::setBrick() skips an identical pointer, so this
            // counts what actually reaches the GPU. Resizes keep it resident.
            uploadedBrick_ = brick;
            ++uploadCount_;
        }
        renderer_->setBrick(brick);

        QRhiCommandBuffer* cb = nullptr;
        if (rhi_->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb) {
            frame.error = QStringLiteral("offscreen frame begin failed");
            return frame;
        }
        QElapsedTimer timer;
        timer.start();
        QRhiReadbackResult readback;
        // The readback has to be requested inside the pass, after the draw.
        cb->beginPass(rt_, QColor(0, 0, 0, 255), {1.0f, 0});
        QString renderError;
        const bool rendered = renderer_->render(cb, &renderError);
        if (rendered) {
            QRhiResourceUpdateBatch* readbackBatch = rhi_->nextResourceUpdateBatch();
            readbackBatch->readBackTexture(QRhiReadbackDescription(target_), &readback);
            cb->endPass(readbackBatch);
        } else {
            cb->endPass();
        }
        rhi_->endOffscreenFrame();
        rhi_->finish();
        frame.frameMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
        frame.firstFrameMs = static_cast<double>(call.nsecsElapsed()) / 1.0e6;
        if (!rendered) {
            frame.error =
                renderError.isEmpty() ? QStringLiteral("GPU 3D frame failed") : renderError;
            return frame;
        }
        if (readback.data.isEmpty()) {
            frame.error = QStringLiteral("GPU 3D readback returned no pixels");
            return frame;
        }
        const QImage view(reinterpret_cast<const uchar*>(readback.data.constData()),
                          pixelSize.width(), pixelSize.height(), QImage::Format_RGBA8888);
        // Same as the spike's offscreen path: the readback is bottom-up and the
        // shader already composited over the 3D view background, so the frame is
        // opaque RGB32 with nothing for QPainter to premultiply twice.
        frame.image = view.mirrored(false, true).convertToFormat(QImage::Format_RGB32);
        frame.ok = true;
        ++frameCount_;
        return frame;
    }

private:
    bool ensureDevice(QString* error) {
        if (rhi_) return true;
        const QString requested = volumeRhiOverride();
        // The QRhiWidget can present Vulkan (Qt owns the VkInstance), but this
        // offscreen/render-thread device cannot: QRhiVulkanInitParams needs
        // <vulkan/vulkan.h> at build time and this kit does not ship it, so the
        // honest answer is a named refusal rather than a silent D3D11 device.
        if (requested == QLatin1String("vulkan")) {
            *error = QStringLiteral(
                "GPU 3D offscreen has no Vulkan device in this build (no vulkan/vulkan.h); "
                "the widget path still accepts CAPPEDWEATHER_VOLUME_RHI=vulkan");
            return false;
        }
        if (!requested.isEmpty() && !volumeRhiOverrideIs("opengl") &&
            !volumeRhiOverrideIs("d3d11")) {
            *error = QStringLiteral("GPU 3D offscreen has no device for CAPPEDWEATHER_VOLUME_RHI=%1")
                         .arg(requested);
            return false;
        }
        const bool wantGl = volumeRhiOverrideIs("opengl");
#if defined(Q_OS_WIN)
        const bool useGl = wantGl;
#else
        const bool useGl = requested.isEmpty() || wantGl;
#endif
        if (useGl) {
            glFallbackSurface_ = QRhiGles2InitParams::newFallbackSurface();
            QRhiGles2InitParams params;
            params.fallbackSurface = glFallbackSurface_;
            rhi_.reset(QRhi::create(QRhi::OpenGLES2, &params));
        } else {
            QRhiD3D11InitParams params;
            rhi_.reset(QRhi::create(QRhi::D3D11, &params));
        }
        if (!rhi_) {
            *error = useGl ? QStringLiteral("OpenGL device creation failed")
                           : QStringLiteral("Direct3D 11 device creation failed");
            return false;
        }
        backend_ = QString::fromUtf8(rhi_->backendName());
        device_ = rhi_->driverInfo().deviceName;
        return true;
    }

    bool ensureTarget(const QSize& size, QString* error) {
        if (rt_ && targetSize_ == size) return true;
        releaseTarget();
        target_ = rhi_->newTexture(QRhiTexture::RGBA8, size, 1,
                                   QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
        if (!target_ || !target_->create()) {
            *error = QStringLiteral("offscreen render target creation failed");
            releaseTarget();
            return false;
        }
        rt_ = rhi_->newTextureRenderTarget({target_});
        pass_ = rt_->newCompatibleRenderPassDescriptor();
        rt_->setRenderPassDescriptor(pass_);
        if (!rt_->create()) {
            *error = QStringLiteral("offscreen render target setup failed");
            releaseTarget();
            return false;
        }
        if (!renderer_) {
            renderer_ = std::make_unique<RhiVolumeRenderer>();
            if (!renderer_->create(rhi_.get(), pass_, error)) {
                renderer_.reset();
                releaseTarget();
                return false;
            }
        } else {
            // A resize keeps the volume texture and the palette LUT; only the
            // pipeline is bound to the render pass, and setRenderPass drops it.
            renderer_->setRenderPass(pass_);
        }
        targetSize_ = size;
        return true;
    }

    void releaseTarget() {
        if (renderer_) renderer_->setRenderPass(nullptr);
        if (rt_) {
            rt_->destroy();
            delete rt_;
            rt_ = nullptr;
        }
        if (pass_) {
            pass_->destroy();
            delete pass_;
            pass_ = nullptr;
        }
        if (target_) {
            target_->destroy();
            delete target_;
            target_ = nullptr;
        }
        targetSize_ = QSize();
    }

    std::unique_ptr<QRhi> rhi_;
    // Only set when the OpenGL backend is used: QRhi's GL device draws through
    // this surface's context, so it must outlive rhi_.
    QOffscreenSurface* glFallbackSurface_ = nullptr;
    std::unique_ptr<RhiVolumeRenderer> renderer_;
    QRhiTexture* target_ = nullptr;
    QRhiTextureRenderTarget* rt_ = nullptr;
    QRhiRenderPassDescriptor* pass_ = nullptr;
    QSize targetSize_;
    std::shared_ptr<const VolumeBrick> uploadedBrick_;
    QString backend_;
    QString device_;
    qint64 frameCount_ = 0;
    qint64 uploadCount_ = 0;
};

// One request at a time, one frame at a time. A caller blocks for its frame
// because the panel's pool thread has nothing else to do while the GPU marches;
// a frame that does not come back inside kFrameTimeoutMs is reported as an error
// so the panel falls back to the CPU instead of hanging on a wedged driver.
constexpr int kFrameTimeoutMs = 4000;

class GpuFrameThread final : public QThread {
public:
    static GpuFrameThread& shared() {
        // Deliberately leaked, and `run()` never returns: the device belongs to
        // the process, and tearing a D3D11 device down during static destruction
        // (after Qt is gone) is a hang waiting to happen. Process exit takes it
        // down with the address space.
        static GpuFrameThread* thread = new GpuFrameThread();
        // Started once, on first use: a thread that is never started answers
        // every submit() with "not running" and the panel silently lives on the
        // CPU march. call_once keeps the start race-free for the first two 3D
        // panels toggled in the same event loop turn.
        static std::once_flag started;
        std::call_once(started, [] { thread->start(); });
        return *thread;
    }

    std::atomic<qint64> frames{0};
    std::atomic<qint64> uploads{0};

    VolumeGpuFrame submit(const std::shared_ptr<const VolumeBrick>& brick,
                          const VolumeCamera& camera, const std::vector<RgbaStop>& stops,
                          const int opacityPercent, const QSize& pixelSize) {
        QMutexLocker lock(&mutex_);
        // Several 3D panels share this device: queue behind whichever frame is
        // in flight rather than overwriting its request.
        while (inFlight_ && isRunning()) {
            if (!wake_.wait(&mutex_, kFrameTimeoutMs)) return timeoutFrame();
        }
        if (!isRunning()) {
            VolumeGpuFrame frame;
            frame.error = QStringLiteral("GPU 3D thread is not running");
            return frame;
        }
        request_ = Request{brick, camera, stops, opacityPercent, pixelSize};
        haveRequest_ = true;
        inFlight_ = true;
        pending_ = true;
        wake_.wakeAll();
        while (pending_) {
            if (!wake_.wait(&mutex_, kFrameTimeoutMs)) {
                // Leave inFlight_ set: the next caller must not pile onto a
                // request the driver never finished.
                return timeoutFrame();
            }
        }
        VolumeGpuFrame frame = reply_;
        inFlight_ = false;
        wake_.wakeAll();
        return frame;
    }

protected:
    void run() override {
        RhiVolumeScene scene;
        for (;;) {
            Request request;
            {
                QMutexLocker lock(&mutex_);
                while (!haveRequest_) wake_.wait(&mutex_);
                request = request_;
                haveRequest_ = false;
            }
            VolumeGpuFrame frame =
                scene.renderFrame(request.brick, request.camera, request.stops,
                                  request.opacityPercent, request.pixelSize);
            frames.store(scene.frameCount(), std::memory_order_relaxed);
            uploads.store(scene.uploadCount(), std::memory_order_relaxed);
            {
                QMutexLocker lock(&mutex_);
                reply_ = std::move(frame);
                pending_ = false;
                // The caller that timed out still owns the request on paper; the
                // frame it asked for is done here. Clear the in-flight flag so a
                // later caller queues behind the GPU thread's actual work rather
                // than waiting out kFrameTimeoutMs for a reply nobody will take.
                inFlight_ = false;
                wake_.wakeAll();
            }
        }
    }

private:
    struct Request {
        std::shared_ptr<const VolumeBrick> brick;
        VolumeCamera camera;
        std::vector<RgbaStop> stops;
        int opacityPercent = 80;
        QSize pixelSize;
    };

    static VolumeGpuFrame timeoutFrame() {
        VolumeGpuFrame frame;
        frame.error = QStringLiteral("GPU 3D frame timed out after %1 ms").arg(kFrameTimeoutMs);
        return frame;
    }

    QMutex mutex_;
    QWaitCondition wake_;
    bool haveRequest_ = false;
    bool inFlight_ = false;
    bool pending_ = false;
    Request request_;
    VolumeGpuFrame reply_;
};

} // namespace

// VolumeCanvas::Impl lives in the .cpp so the header does not drag QRhi in.

struct VolumeCanvas::Impl {
    std::unique_ptr<RhiVolumeRenderer> renderer;
    std::shared_ptr<const VolumeBrick> brick;
    std::vector<RgbaStop> stops;
    VolumeCamera camera;
    bool haveOrbit = false;
    double azimuth = 180.0;
    double elevation = 40.0;
    double distance = 420000.0;
    double target[3] = {0.0, 0.0, 7000.0};
    int opacityPercent = 80;
    QString error;
    QString backend;
    QString device;
    double lastFrameMs = 0.0;
    qint64 frames = 0;
    bool failedEmitted = false;
};

VolumeCanvas::VolumeCanvas(QWidget* parent) : QRhiWidget(parent), impl_(std::make_unique<Impl>()) {
    // CAPPEDWEATHER_VOLUME_RHI forces the widget's API so the probe can present
    // the same volume on every backend this machine has (card t_9691d611).
    // Unset keeps the platform default, which on Windows means Direct3D 11.
    if (volumeRhiOverrideIs("opengl")) {
        setApi(QRhiWidget::Api::OpenGL);
    } else if (volumeRhiOverrideIs("vulkan")) {
        setApi(QRhiWidget::Api::Vulkan);
    } else if (volumeRhiOverrideIs("d3d11")) {
        setApi(QRhiWidget::Api::Direct3D11);
    } else if (volumeRhiOverride() == QLatin1String("d3d12")) {
        setApi(QRhiWidget::Api::Direct3D12);
    } else if (volumeRhiOverride() == QLatin1String("metal")) {
        setApi(QRhiWidget::Api::Metal);
    } else {
#if defined(Q_OS_WIN)
        setApi(QRhiWidget::Api::Direct3D11);
#endif
    }
    // A backend that cannot start never reaches initialize(), so the host would
    // sit on "initialising" forever and never fall back to the CPU march. Qt
    // emits renderFailed() in that case; turn it into the same one-line
    // gpuFailed() the paint-side failures use.
    connect(this, &QRhiWidget::renderFailed, this, [this] {
        if (impl_->failedEmitted) return;
        if (impl_->error.isEmpty())
            impl_->error = QStringLiteral("GPU 3D: no RHI device came up%1")
                               .arg(impl_->backend.isEmpty()
                                        ? QString()
                                        : QStringLiteral(" (%1)").arg(impl_->backend));
        impl_->failedEmitted = true;
        emit gpuFailed(impl_->error);
    });
}

VolumeCanvas::~VolumeCanvas() = default;

void VolumeCanvas::setVolume(std::shared_ptr<const VolumeBrick> brick) {
    impl_->brick = std::move(brick);
    if (impl_->renderer) impl_->renderer->setBrick(impl_->brick);
    // The camera aims at the brick centre in x/y, so a new brick moves it.
    if (impl_->brick) {
        const VolumeBrick& volume = *impl_->brick;
        const double frameTarget[3] = {volume.centerXMeters, volume.centerYMeters,
                                       impl_->target[2]};
        impl_->camera = volumeCameraFor(volume, impl_->azimuth, impl_->elevation, impl_->distance,
                                        frameTarget);
    }
    update();
}

void VolumeCanvas::setTransferStops(std::vector<RgbaStop> stops) {
    impl_->stops = std::move(stops);
    if (impl_->renderer) impl_->renderer->setStops(impl_->stops);
    update();
}

void VolumeCanvas::setOrbit(const double azimuthDeg, const double elevationDeg,
                            const double distanceMeters, const double target[3]) {
    impl_->azimuth = azimuthDeg;
    impl_->elevation = elevationDeg;
    impl_->distance = distanceMeters;
    for (int axis = 0; axis < 3; ++axis) impl_->target[axis] = target[axis];
    impl_->haveOrbit = true;
    if (impl_->brick) {
        const VolumeBrick& brick = *impl_->brick;
        const double frameTarget[3] = {brick.centerXMeters, brick.centerYMeters,
                                       impl_->target[2]};
        impl_->camera = volumeCameraFor(brick, impl_->azimuth, impl_->elevation, impl_->distance,
                                       frameTarget);
    }
    update();
}

void VolumeCanvas::setOpacityPercent(const int percent) {
    impl_->opacityPercent = percent;
    update();
}

bool VolumeCanvas::gpuReady() const { return impl_->renderer != nullptr && impl_->renderer->ready(); }

QString VolumeCanvas::statusLine() const {
    if (!impl_->error.isEmpty()) return impl_->error;
    if (!gpuReady()) return QStringLiteral("GPU 3D initialising");
    // Submit time, not GPU time: QRhiWidget owns the RHI flags and timestamps
    // are not enabled, so the completed-frame number comes from
    // renderVolumeOffscreen() (see docs/gpu-volume-rendering.md).
    return QStringLiteral("GPU 3D %1  submit %2 ms")
        .arg(impl_->backend)
        .arg(impl_->lastFrameMs, 0, 'f', 2);
}

QString VolumeCanvas::backendName() const {
    if (!impl_->backend.isEmpty()) return impl_->backend;
    return rhi() ? QString::fromUtf8(rhi()->backendName()) : QString();
}

QString VolumeCanvas::shaderSource() const {
    return impl_->renderer ? impl_->renderer->shaderLabel() : QString();
}

QString VolumeCanvas::lastError() const { return impl_->error; }

double VolumeCanvas::lastFrameMs() const { return impl_->lastFrameMs; }

qint64 VolumeCanvas::frameCount() const { return impl_->frames; }

QSize VolumeCanvas::renderSize() const {
    return renderTarget() ? renderTarget()->pixelSize() : QSize();
}

void VolumeCanvas::initialize(QRhiCommandBuffer*) {
    impl_->error.clear();
    impl_->renderer.reset();
    if (!rhi() || !renderTarget()) {
        impl_->error = QStringLiteral("GPU 3D unavailable: no RHI render target");
        return;
    }
    impl_->backend = QString::fromUtf8(rhi()->backendName());
    impl_->device = rhi()->driverInfo().deviceName;
    auto renderer = std::make_unique<RhiVolumeRenderer>();
    QString error;
    if (!renderer->create(rhi(), renderTarget()->renderPassDescriptor(), &error)) {
        impl_->error = error;
        qWarning().noquote() << "3D: GPU raymarch unavailable -" << error;
        if (!impl_->failedEmitted) {
            impl_->failedEmitted = true;
            emit gpuFailed(error);
        }
        return;
    }
    renderer->setBrick(impl_->brick);
    renderer->setStops(impl_->stops);
    impl_->renderer = std::move(renderer);
    qInfo().noquote() << "3D: GPU raymarch backend" << impl_->backend << impl_->device;
}

void VolumeCanvas::render(QRhiCommandBuffer* cb) {
    QRhiRenderTarget* target = renderTarget();
    if (!impl_->renderer || !impl_->brick || !target) return;
    impl_->renderer->setRenderPass(target->renderPassDescriptor());
    impl_->renderer->setView(impl_->camera, target->pixelSize(), impl_->opacityPercent);
    QElapsedTimer timer;
    timer.start();
    QString error;
    // QRhiWidget hands over the command buffer with no pass open: the widget is
    // responsible for beginPass/endPass on its own render target. Drawing
    // without this crashes inside QRhiCommandBuffer::draw() (segfault, no
    // warning) on Qt 6.8.3 / D3D11.
    cb->beginPass(target, QColor(8, 13, 16, 255), {1.0f, 0});
    const bool rendered = impl_->renderer->render(cb, &error);
    cb->endPass();
    if (!rendered) {
        impl_->error = error.isEmpty() ? QStringLiteral("GPU 3D frame failed") : error;
        if (!impl_->failedEmitted) {
            impl_->failedEmitted = true;
            emit gpuFailed(impl_->error);
        }
        return;
    }
    impl_->lastFrameMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
    ++impl_->frames;
}

void VolumeCanvas::releaseResources() { impl_->renderer.reset(); }

// --- offscreen ----------------------------------------------------------

VolumeGpuFrame renderVolumeOffscreen(const VolumeBrick& brick, const VolumeCamera& camera,
                                     const std::vector<RgbaStop>& stops, const int opacityPercent,
                                     const QSize& pixelSize, const int frames) {
    VolumeGpuFrame frame;
    if (pixelSize.isEmpty()) {
        frame.error = QStringLiteral("GPU 3D offscreen render needs a non-empty size");
        return frame;
    }
    if (brick.value.empty()) {
        frame.error = QStringLiteral("GPU 3D has no brick to march");
        return frame;
    }
    // A private device: the probe must not queue behind (or warm) the panel's.
    RhiVolumeScene scene;
    const auto brickNow = std::make_shared<const VolumeBrick>(brick);
    const int frameCount = std::max(1, frames);
    for (int index = 0; index < frameCount; ++index) {
        const VolumeGpuFrame one =
            scene.renderFrame(brickNow, camera, stops, opacityPercent, pixelSize);
        if (index == 0) frame.firstFrameMs = one.firstFrameMs;
        frame.frameMs = one.frameMs; // last completed frame, resources resident
        frame.backend = one.backend;
        frame.device = one.device;
        frame.shader = one.shader;
        if (!one.ok) {
            frame.error = one.error;
            frame.ok = false;
            return frame;
        }
        frame.image = one.image;
        frame.ok = true;
    }
    return frame;
}

VolumeGpuFrame renderVolumeResident(std::shared_ptr<const VolumeBrick> brick,
                                    const VolumeCamera& camera,
                                    const std::vector<RgbaStop>& stops, const int opacityPercent,
                                    const QSize& pixelSize) {
    if (!brick || brick->value.empty()) {
        VolumeGpuFrame frame;
        frame.error = QStringLiteral("GPU 3D has no brick to march");
        return frame;
    }
    return GpuFrameThread::shared().submit(brick, camera, stops, opacityPercent, pixelSize);
}

qint64 volumeResidentFrames() { return GpuFrameThread::shared().frames.load(); }

qint64 volumeResidentUploads() { return GpuFrameThread::shared().uploads.load(); }

} // namespace cappedweather
