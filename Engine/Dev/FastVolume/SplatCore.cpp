#include "SplatCore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "webgpu.h"
#include "wgpu.h"

namespace SplatCore {

namespace {

// ------------------------------------------------------------------ shader
//
// One instanced quad per splat, 4 vertices as a triangle strip. Bindings:
//   0 uniforms, 1 position + opacity, 2 covariance (6 floats), 3 SH colour
//   coefficients (shStride floats, coefficient-major: c0.rgb, c1.rgb, ...),
//   4 draw order (far to near). Several scenes share the buffers, one after the
//   other; a splat finds its scene (and so its transform) from its id.
const char* kShader = R"(
struct U {
    view: mat4x4f,
    models: array<mat4x4f, 8>,       // per scene: file space -> world
    camPosLocal: array<vec4f, 8>,    // per scene: camera position in the scene's file space
    sceneEnd: array<vec4u, 2>,       // per scene: one past its last splat id
    focal: vec2f,
    viewport: vec2f,
    tanFov: vec2f,
    nearClip: f32,
    splatScale: f32,
    shDegree: u32,
    shStride: u32,
    lowpass: f32,
    maxPx: f32,
    opacityScale: f32,
    numScenes: u32,
    ndcOffset: vec2f,
};
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var<storage, read> posOp: array<vec4f>;
@group(0) @binding(2) var<storage, read> cov: array<f32>;
@group(0) @binding(3) var<storage, read> sh: array<f32>;
@group(0) @binding(4) var<storage, read> order: array<u32>;
@group(0) @binding(5) var<storage, read> nrm: array<f32>;     // 3 per splat: its thinnest axis (file space)

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) color: vec4f,     // rgb + opacity
    @location(1) uv: vec2f,        // position inside the splat, in standard deviations
    @location(2) depth: f32,       // distance along the view axis
    @location(3) normal: vec3f,    // world space, facing the camera
    @location(4) @interpolate(flat) scene: u32,
};

fn shc(id: u32, k: u32) -> vec3f {
    let b = id * u.shStride + k * 3u;
    return vec3f(sh[b], sh[b + 1u], sh[b + 2u]);
}

// Spherical-harmonic colour for direction d (unit, splat file space). Same
// constants and signs as the reference 3DGS implementation.
fn shColor(id: u32, d: vec3f) -> vec3f {
    var c = 0.28209479177387814 * shc(id, 0u);
    if (u.shDegree > 0u) {
        let x = d.x; let y = d.y; let z = d.z;
        c += 0.4886025119029199 * (-y * shc(id, 1u) + z * shc(id, 2u) - x * shc(id, 3u));
        if (u.shDegree > 1u) {
            let xx = x * x; let yy = y * y; let zz = z * z;
            let xy = x * y; let yz = y * z; let xz = x * z;
            c += 1.0925484305920792 * xy * shc(id, 4u)
               - 1.0925484305920792 * yz * shc(id, 5u)
               + 0.31539156525252005 * (2.0 * zz - xx - yy) * shc(id, 6u)
               - 1.0925484305920792 * xz * shc(id, 7u)
               + 0.5462742152960396 * (xx - yy) * shc(id, 8u);
            if (u.shDegree > 2u) {
                c += -0.5900435899266435 * y * (3.0 * xx - yy) * shc(id, 9u)
                   + 2.890611442640554 * xy * z * shc(id, 10u)
                   - 0.4570457994644658 * y * (4.0 * zz - xx - yy) * shc(id, 11u)
                   + 0.3731763325901154 * z * (2.0 * zz - 3.0 * xx - 3.0 * yy) * shc(id, 12u)
                   - 0.4570457994644658 * x * (4.0 * zz - xx - yy) * shc(id, 13u)
                   + 1.445305721320277 * z * (xx - yy) * shc(id, 14u)
                   - 0.5900435899266435 * x * (xx - 3.0 * yy) * shc(id, 15u);
            }
        }
    }
    return max(c + vec3f(0.5), vec3f(0.0));
}

@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VOut {
    var o: VOut;
    o.pos = vec4f(0.0, 0.0, 2.0, 1.0);     // outside the clip volume: culled
    o.color = vec4f(0.0);
    o.uv = vec2f(0.0);
    o.depth = 0.0;
    o.normal = vec3f(0.0);
    o.scene = 0u;

    let id = order[ii];
    let p = posOp[id];
    let opacity = p.w * u.opacityScale;
    if (opacity < 0.00392156862) { return o; }

    // Which scene this splat belongs to (ids are concatenated scene after scene).
    var sc = 0u;
    for (var k = 0u; k + 1u < u.numScenes; k++) {
        if (id >= u.sceneEnd[k >> 2u][k & 3u]) { sc = k + 1u; }
    }
    let mv = u.view * u.models[sc];
    let pc = mv * vec4f(p.xyz, 1.0);       // camera space: +x right, +y up, looking down -z
    let tz = -pc.z;
    if (tz < u.nearClip) { return o; }

    // 3D covariance: file space -> camera space (the model part carries the
    // node's rotation, uniform scale and mirror).
    let cb = id * 6u;
    let sig = mat3x3f(vec3f(cov[cb], cov[cb + 1u], cov[cb + 2u]),
                      vec3f(cov[cb + 1u], cov[cb + 3u], cov[cb + 4u]),
                      vec3f(cov[cb + 2u], cov[cb + 4u], cov[cb + 5u]));
    let w = mat3x3f(mv[0].xyz, mv[1].xyz, mv[2].xyz);
    let covC = w * sig * transpose(w) * (u.splatScale * u.splatScale);

    // Perspective Jacobian at the splat centre, in pixels. The centre used for
    // the Jacobian is kept within 1.3 x the field of view, as in the reference.
    let lim = 1.3 * u.tanFov;
    let tx = clamp(pc.x / tz, -lim.x, lim.x) * tz;
    let ty = clamp(pc.y / tz, -lim.y, lim.y) * tz;
    let j0 = vec3f(u.focal.x / tz, 0.0, u.focal.x * tx / (tz * tz));
    let j1 = vec3f(0.0, u.focal.y / tz, u.focal.y * ty / (tz * tz));
    let cj0 = covC * j0;
    let cj1 = covC * j1;
    let a = dot(j0, cj0) + u.lowpass;
    let b = dot(j0, cj1);
    let c = dot(j1, cj1) + u.lowpass;

    let det = a * c - b * b;
    if (det <= 0.0) { return o; }
    let mid = 0.5 * (a + c);
    let rad = sqrt(max(mid * mid - det, 0.0));
    let l1 = mid + rad;
    let l2 = max(mid - rad, 0.0001);
    var ax1 = vec2f(1.0, 0.0);
    if (abs(b) > 1e-7) {
        ax1 = normalize(vec2f(b, l1 - a));
    } else if (c > a) {
        ax1 = vec2f(0.0, 1.0);
    }
    let ax2 = vec2f(-ax1.y, ax1.x);

    // 3.33 standard deviations: where a fully opaque splat falls to 1/255.
    let s1 = sqrt(l1);
    let s2 = sqrt(l2);
    let r1 = min(3.33 * s1, u.maxPx);
    let r2 = min(3.33 * s2, u.maxPx);

    let corner = vec2f(f32(vi & 1u) * 2.0 - 1.0, f32((vi >> 1u) & 1u) * 2.0 - 1.0);
    let offPx = corner.x * r1 * ax1 + corner.y * r2 * ax2;
    let ndc = vec2f(u.focal.x * pc.x / tz, u.focal.y * pc.y / tz) / (0.5 * u.viewport) + u.ndcOffset;
    o.pos = vec4f(ndc + offPx / (0.5 * u.viewport), 0.5, 1.0);
    o.uv = vec2f(corner.x * r1 / s1, corner.y * r2 / s2);
    o.depth = tz;

    let dir = normalize(p.xyz - u.camPosLocal[sc].xyz);
    o.color = vec4f(shColor(id, dir), opacity);

    // Normal: the splat's thinnest axis, in camera space to pick the side that faces the
    // camera, sent out in world space.
    let nb = id * 3u;
    let nLocal = vec3f(nrm[nb], nrm[nb + 1u], nrm[nb + 2u]);
    let nCam = w * nLocal;
    let m3 = mat3x3f(u.models[sc][0].xyz, u.models[sc][1].xyz, u.models[sc][2].xyz);
    var nWorld = m3 * nLocal;
    if (dot(nCam, pc.xyz) > 0.0) { nWorld = -nWorld; }
    let nl = length(nWorld);
    if (nl > 1e-12) { nWorld = nWorld / nl; }
    o.normal = nWorld;
    o.scene = sc;
    return o;
}

struct FOut {
    @location(0) color: vec4f,
    @location(1) depth: vec4f,
};

@fragment
fn fs(i: VOut) -> FOut {
    let alpha = min(0.99, i.color.w * exp(-0.5 * dot(i.uv, i.uv)));
    if (alpha < 0.00392156862) { discard; }
    var o: FOut;
    o.color = vec4f(i.color.rgb * alpha, alpha);
    o.depth = vec4f(i.depth * alpha, 0.0, 0.0, alpha);
    return o;
}

// Second pass, same splats in the same order: layers that are not colour.
struct FAov {
    @location(0) normal: vec4f,     // normal x alpha, blended like the colour
    @location(1) matte: vec4f,      // alpha into the channel of the splat's scene (scenes 0..2)
    @location(2) nearest: vec4f,    // depth and 1 + scene of the last (= nearest) half-opaque splat
};

@fragment
fn fsAov(i: VOut) -> FAov {
    let alpha = min(0.99, i.color.w * exp(-0.5 * dot(i.uv, i.uv)));
    if (alpha < 0.00392156862) { discard; }
    var o: FAov;
    o.normal = vec4f(i.normal * alpha, alpha);
    var m = vec3f(0.0);
    if (i.scene == 0u) { m.x = alpha; } else if (i.scene == 1u) { m.y = alpha; } else if (i.scene == 2u) { m.z = alpha; }
    o.matte = vec4f(m, alpha);
    // Splats are drawn far to near, so with an all-or-nothing alpha the last one written
    // is the nearest one that is solid here.
    let solid = select(0.0, 1.0, alpha >= 0.5);
    o.nearest = vec4f(i.depth * solid, f32(i.scene + 1u) * solid, 0.0, solid);
    return o;
}
)";

struct Uniforms {
    float view[16];
    float models[kMaxScenes][16];
    float camPosLocal[kMaxScenes][4];
    uint32_t sceneEnd[8];
    float focal[2];
    float viewport[2];
    float tanFov[2];
    float nearClip;
    float splatScale;
    uint32_t shDegree;
    uint32_t shStride;
    float lowpass;
    float maxPx;
    float opacityScale;
    uint32_t numScenes;
    float ndcOffset[2];
};
static_assert(kMaxScenes == 8, "the WGSL arrays are hard-coded to 8 scenes");
static_assert(sizeof(Uniforms) == 800, "Uniforms must match the WGSL struct U");

WGPUStringView sv(const char* s) {
    WGPUStringView v;
    v.data = s;
    v.length = WGPU_STRLEN;
    return v;
}

void onAdapter(WGPURequestAdapterStatus, WGPUAdapter a, WGPUStringView, void* u, void*) {
    *(WGPUAdapter*)u = a;
}
void onDevice(WGPURequestDeviceStatus, WGPUDevice d, WGPUStringView, void* u, void*) {
    *(WGPUDevice*)u = d;
}
void onMap(WGPUMapAsyncStatus, WGPUStringView, void* u, void*) {
    *(bool*)u = true;
}

// column-major 4x4: out = a * b
void mul4(const float* a, const float* b, float* out) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    std::memcpy(out, r, sizeof r);
}

// Point `p` through the inverse of the affine column-major matrix m.
bool invAffinePoint(const float* m, const float* p, float* out) {
    const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-20) return false;
    const double id = 1.0 / det;
    const double x = p[0] - m[12], y = p[1] - m[13], z = p[2] - m[14];
    out[0] = (float)(((e * i - f * h) * x + (c * h - b * i) * y + (b * f - c * e) * z) * id);
    out[1] = (float)(((f * g - d * i) * x + (a * i - c * g) * y + (c * d - a * f) * z) * id);
    out[2] = (float)(((d * h - e * g) * x + (b * g - a * h) * y + (a * e - b * d) * z) * id);
    return true;
}

inline float halfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            exp = 1;
            while (!(man & 0x400u)) { man <<= 1; --exp; }
            man &= 0x3FFu;
            bits = sign | ((exp + 112u) << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Run f(thread, begin, end) over [0, n) split into contiguous ranges, one per
// thread. Small jobs run inline.
template <typename F>
void parallelFor(std::size_t n, int threads, F f) {
    if (threads <= 1 || n < (std::size_t)threads) {
        f(0, (std::size_t)0, n);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve((std::size_t)threads - 1);
    for (int t = 1; t < threads; ++t) {
        const std::size_t b = n * (std::size_t)t / (std::size_t)threads, e = n * (std::size_t)(t + 1) / (std::size_t)threads;
        pool.emplace_back([&f, t, b, e]() { f(t, b, e); });
    }
    f(0, (std::size_t)0, n / (std::size_t)threads);
    for (std::thread& th : pool) th.join();
}

int sortThreads(std::size_t n) {
    if (n < 200000) return 1;
    const unsigned hc = std::thread::hardware_concurrency();
    return (int)std::max(1u, std::min(16u, hc ? hc : 4u));
}

double msSince(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The GPU device, shared by every Renderer in the process. Each Renderer used to
// make its own: with four devices alive, one frame's GPU time went from 16 ms to
// 137 ms (measured), so a comp with several splat renders crawled for no reason.
// The device lives as long as any Renderer does. All use of it is serialised with
// `mutex` (the error text below is shared state).
struct Gpu {
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPULimits limits = {};
    std::string error;
    std::string gpuError;          // set by the uncaptured-error callback
    std::string adapterName;
    bool float32Blend = false;
    WGPUTextureFormat format = WGPUTextureFormat_RGBA16Float;
    WGPURenderPipeline pipeline = nullptr;
    WGPURenderPipeline pipelineAov = nullptr;    // second pass: normal, scene matte, nearest depth
    WGPUBindGroupLayout bindLayout = nullptr;    // one layout for both, so one bind group serves both
    // Recursive: releasing a shared scene (below) can happen while its owner holds the lock.
    std::recursive_mutex mutex;
    // Uploaded scenes by content ids, so several Renderers can share one copy.
    std::map<std::vector<unsigned long long>, std::weak_ptr<struct SceneGpu> > sceneCache;

    static void onError(WGPUDevice const*, WGPUErrorType, WGPUStringView msg, void* u, void*) {
        Gpu* self = (Gpu*)u;
        if (!self) return;
        const std::size_t n = msg.length == WGPU_STRLEN ? (msg.data ? std::strlen(msg.data) : 0) : msg.length;
        if (self->gpuError.empty() && msg.data) self->gpuError.assign(msg.data, n);
    }

    bool init() {
        instance = wgpuCreateInstance(nullptr);
        if (!instance) { error = "wgpuCreateInstance failed"; return false; }

        WGPURequestAdapterOptions opts = {};
        opts.powerPreference = WGPUPowerPreference_HighPerformance;
        WGPURequestAdapterCallbackInfo acb = {};
        acb.mode = WGPUCallbackMode_AllowProcessEvents;
        acb.callback = onAdapter;
        acb.userdata1 = &adapter;
        wgpuInstanceRequestAdapter(instance, &opts, acb);
        for (int i = 0; i < 10000 && !adapter; ++i) wgpuInstanceProcessEvents(instance);
        if (!adapter) { error = "no GPU adapter"; return false; }

        // Ask for everything the adapter offers: the default limits stop at
        // 256 MB per buffer and 128 MB per storage binding, far too little for a
        // large scan.
        wgpuAdapterGetLimits(adapter, &limits);
        limits.nextInChain = nullptr;
        float32Blend = wgpuAdapterHasFeature(adapter, WGPUFeatureName_Float32Blendable) != 0;
        const WGPUFeatureName feats[1] = {WGPUFeatureName_Float32Blendable};

        WGPUDeviceDescriptor dd = {};
        dd.requiredLimits = &limits;
        if (float32Blend) {
            dd.requiredFeatureCount = 1;
            dd.requiredFeatures = feats;
        }
        // Without a callback wgpu-native aborts the process on a validation or
        // out-of-memory error; with one, the error becomes a failed render.
        dd.uncapturedErrorCallbackInfo.callback = &Gpu::onError;
        dd.uncapturedErrorCallbackInfo.userdata1 = this;

        WGPURequestDeviceCallbackInfo dcb = {};
        dcb.mode = WGPUCallbackMode_AllowProcessEvents;
        dcb.callback = onDevice;
        dcb.userdata1 = &device;
        wgpuAdapterRequestDevice(adapter, &dd, dcb);
        for (int i = 0; i < 10000 && !device; ++i) wgpuInstanceProcessEvents(instance);
        if (!device) { error = "device request failed"; return false; }
        queue = wgpuDeviceGetQueue(device);
        wgpuDeviceGetLimits(device, &limits);
        format = float32Blend ? WGPUTextureFormat_RGBA32Float : WGPUTextureFormat_RGBA16Float;

        WGPUAdapterInfo info = {};
        wgpuAdapterGetInfo(adapter, &info);
        if (info.device.data) {
            adapterName.assign(info.device.data,
                               info.device.length == WGPU_STRLEN ? std::strlen(info.device.data) : info.device.length);
        }
        switch (info.backendType) {
            case WGPUBackendType_Vulkan:   adapterName += " (Vulkan)"; break;
            case WGPUBackendType_D3D12:    adapterName += " (Direct3D 12)"; break;
            case WGPUBackendType_D3D11:    adapterName += " (Direct3D 11)"; break;
            case WGPUBackendType_Metal:    adapterName += " (Metal)"; break;
            case WGPUBackendType_OpenGL:   adapterName += " (OpenGL)"; break;
            case WGPUBackendType_OpenGLES: adapterName += " (OpenGL ES)"; break;
            default: break;
        }

        return createPipeline();
    }

    bool createPipeline() {
        WGPUShaderSourceWGSL src = {};
        src.chain.sType = WGPUSType_ShaderSourceWGSL;
        src.code = sv(kShader);
        WGPUShaderModuleDescriptor smd = {};
        smd.nextInChain = &src.chain;
        WGPUShaderModule mod = wgpuDeviceCreateShaderModule(device, &smd);
        if (!mod || !gpuError.empty()) { error = "splat shader failed to compile: " + gpuError; return false; }

        // One explicit bind group layout for both pipelines (an automatic layout is private to
        // its pipeline, and the colour pass does not use every binding).
        WGPUBindGroupLayoutEntry le[6] = {};
        for (uint32_t b = 0; b < 6; ++b) {
            le[b].binding = b;
            le[b].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            le[b].buffer.type = b == 0 ? WGPUBufferBindingType_Uniform : WGPUBufferBindingType_ReadOnlyStorage;
        }
        WGPUBindGroupLayoutDescriptor bld = {};
        bld.entryCount = 6;
        bld.entries = le;
        bindLayout = wgpuDeviceCreateBindGroupLayout(device, &bld);
        WGPUPipelineLayoutDescriptor pld = {};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &bindLayout;
        WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(device, &pld);
        if (!bindLayout || !layout || !gpuError.empty()) { error = "splat pipeline layout failed: " + gpuError; return false; }

        WGPUBlendState blend = {};
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_One;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        // The alpha channel holds TRANSMITTANCE (what still shows through), not
        // coverage: it starts at 1 and each splat multiplies it by (1 - alpha).
        // A 16-bit float target cannot add the last thin layers to a coverage
        // near 1 (it stalled at 0.994), but it keeps a small transmittance exactly.
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_Zero;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        for (int which = 0; which < 2; ++which) {
            // 0: colour + depth, in the best blendable float format. 1: the three extra layers,
            // always 16-bit float (three 32-bit targets would pass the per-pixel attachment limit).
            const int nTargets = which == 0 ? 2 : 3;
            WGPUColorTargetState targets[3] = {};
            for (int i = 0; i < nTargets; ++i) {
                targets[i].format = which == 0 ? format : WGPUTextureFormat_RGBA16Float;
                targets[i].blend = &blend;
                targets[i].writeMask = WGPUColorWriteMask_All;
            }
            WGPUFragmentState fs = {};
            fs.module = mod;
            fs.entryPoint = sv(which == 0 ? "fs" : "fsAov");
            fs.targetCount = (size_t)nTargets;
            fs.targets = targets;

            WGPURenderPipelineDescriptor pd = {};
            pd.layout = layout;
            pd.vertex.module = mod;
            pd.vertex.entryPoint = sv("vs");
            pd.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
            pd.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
            pd.primitive.frontFace = WGPUFrontFace_CCW;
            pd.primitive.cullMode = WGPUCullMode_None;
            pd.multisample.count = 1;
            pd.multisample.mask = 0xFFFFFFFFu;
            pd.fragment = &fs;
            (which == 0 ? pipeline : pipelineAov) = wgpuDeviceCreateRenderPipeline(device, &pd);
        }
        wgpuPipelineLayoutRelease(layout);
        wgpuShaderModuleRelease(mod);
        if (!pipeline || !pipelineAov || !gpuError.empty()) { error = "splat pipeline creation failed: " + gpuError; return false; }
        return true;
    }

    ~Gpu() {
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (pipelineAov) wgpuRenderPipelineRelease(pipelineAov);
        if (bindLayout) wgpuBindGroupLayoutRelease(bindLayout);
        if (queue) wgpuQueueRelease(queue);
        if (device) wgpuDeviceRelease(device);
        if (adapter) wgpuAdapterRelease(adapter);
        if (instance) wgpuInstanceRelease(instance);
    }
};

// One uploaded set of scenes: the large buffers, which do not depend on the view.
struct SceneGpu {
    std::shared_ptr<Gpu> gpu;
    WGPUBuffer posBuf = nullptr, covBuf = nullptr, shBuf = nullptr, nrmBuf = nullptr;
    std::size_t count = 0;                      // total splats
    int numScenes = 0;
    std::size_t sceneStart[kMaxScenes + 1] = {0};
    int shDegree = 0;                           // highest degree among the scenes
    std::vector<float> radius;                  // per splat: largest scale (file units), for culling

    ~SceneGpu() {
        if (!gpu) return;
        std::lock_guard<std::recursive_mutex> lk(gpu->mutex);
        for (WGPUBuffer b : {posBuf, covBuf, shBuf, nrmBuf})
            if (b) { wgpuBufferDestroy(b); wgpuBufferRelease(b); }
    }
};

std::shared_ptr<Gpu> acquireGpu(std::string& err) {
    static std::mutex m;
    static std::weak_ptr<Gpu> shared;
    std::lock_guard<std::mutex> lk(m);
    std::shared_ptr<Gpu> g = shared.lock();
    if (!g) {
        g = std::make_shared<Gpu>();
        if (!g->init()) {
            err = g->error.empty() ? std::string("GPU initialisation failed") : g->error;
            return std::shared_ptr<Gpu>();
        }
        shared = g;
    }
    return g;
}

}  // namespace

struct Renderer::Impl {
    std::shared_ptr<Gpu> gpu;          // the shared device; the handles below are borrowed from it
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPULimits limits = {};
    std::string error;
    std::string noGpuError;
    std::string* gpuErrorP = &noGpuError;   // the shared device's error text
    std::string adapterName;
    bool float32Blend = false;
    WGPUTextureFormat format = WGPUTextureFormat_RGBA16Float;

    WGPURenderPipeline pipeline = nullptr;
    WGPUBuffer uniformBuf = nullptr;

    // Scenes (resident between draws), concatenated in the buffers. The big buffers
    // belong to `scene`, which other Renderers may share; the fields below are copies.
    std::shared_ptr<SceneGpu> scene;
    std::size_t count = 0;                      // total splats
    int numScenes = 0;
    std::size_t sceneStart[kMaxScenes + 1] = {0};
    int shDegree = 0;                           // highest degree among the scenes
    std::size_t lastDrawn = 0;                  // visible count of the last sorted draw (reuseOrder)
    WGPUBuffer orderBuf = nullptr;              // this Renderer's draw order (depends on its view)
    WGPUBindGroup bindGroup = nullptr;

    // Targets (resident, recreated when the size changes)
    int texW = 0, texH = 0;
    WGPUTexture colorTex = nullptr, depthTex = nullptr;
    WGPUTextureView colorView = nullptr, depthView = nullptr;
    WGPUBuffer readBuf = nullptr;
    uint64_t readBytes = 0;
    // Targets of the second pass (made on first use)
    int aovW = 0, aovH = 0;
    WGPUTexture aovTex[3] = {nullptr, nullptr, nullptr};
    WGPUTextureView aovView[3] = {nullptr, nullptr, nullptr};
    WGPUBuffer aovReadBuf = nullptr;
    uint64_t aovReadBytes = 0;

    // Sort scratch
    std::vector<float> depth;
    std::vector<uint32_t> idxA, idxB, keyA, keyB;

    bool init() {
        gpu = acquireGpu(error);
        if (!gpu) return false;
        device = gpu->device;
        queue = gpu->queue;
        limits = gpu->limits;
        adapterName = gpu->adapterName;
        float32Blend = gpu->float32Blend;
        format = gpu->format;
        pipeline = gpu->pipeline;
        gpuErrorP = &gpu->gpuError;
        std::lock_guard<std::recursive_mutex> lk(gpu->mutex);
        WGPUBufferDescriptor bd = {};
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bd.size = sizeof(Uniforms);
        uniformBuf = wgpuDeviceCreateBuffer(device, &bd);
        return uniformBuf != nullptr;
    }

    void releaseScene() {
        if (bindGroup) wgpuBindGroupRelease(bindGroup);
        if (orderBuf) { wgpuBufferDestroy(orderBuf); wgpuBufferRelease(orderBuf); }
        bindGroup = nullptr;
        orderBuf = nullptr;
        count = 0;
        numScenes = 0;
        lastDrawn = 0;
        scene.reset();      // frees the big buffers when no other Renderer uses them
    }

    void releaseTargets() {
        if (colorView) wgpuTextureViewRelease(colorView);
        if (depthView) wgpuTextureViewRelease(depthView);
        if (colorTex) { wgpuTextureDestroy(colorTex); wgpuTextureRelease(colorTex); }
        if (depthTex) { wgpuTextureDestroy(depthTex); wgpuTextureRelease(depthTex); }
        if (readBuf) { wgpuBufferDestroy(readBuf); wgpuBufferRelease(readBuf); }
        colorView = depthView = nullptr;
        colorTex = depthTex = nullptr;
        readBuf = nullptr;
        texW = texH = 0;
        releaseAovTargets();
    }

    void releaseAovTargets() {
        for (int i = 0; i < 3; ++i) {
            if (aovView[i]) wgpuTextureViewRelease(aovView[i]);
            if (aovTex[i]) { wgpuTextureDestroy(aovTex[i]); wgpuTextureRelease(aovTex[i]); }
            aovView[i] = nullptr;
            aovTex[i] = nullptr;
        }
        if (aovReadBuf) { wgpuBufferDestroy(aovReadBuf); wgpuBufferRelease(aovReadBuf); }
        aovReadBuf = nullptr;
        aovW = aovH = 0;
    }

    WGPUBuffer storage(uint64_t size) {
        WGPUBufferDescriptor bd = {};
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        bd.size = size < 16 ? 16 : size;
        return wgpuDeviceCreateBuffer(device, &bd);
    }

    ~Impl() {
        if (gpu) {
            std::lock_guard<std::recursive_mutex> lk(gpu->mutex);
            releaseScene();
            releaseTargets();
            if (uniformBuf) wgpuBufferRelease(uniformBuf);
        }
    }
};

Renderer::Renderer() : _p(new Impl) {
    if (!_p->init() && _p->error.empty()) _p->error = "GPU initialisation failed";
}
Renderer::~Renderer() {}
bool Renderer::valid() const { return _p->gpu && _p->device != nullptr && _p->pipeline != nullptr; }
const std::string& Renderer::lastError() const { return _p->error; }
std::string Renderer::adapterName() const { return _p->adapterName; }
bool Renderer::float32Targets() const { return _p->float32Blend; }
bool Renderer::hasScene() const { return _p->scene && _p->count > 0 && _p->bindGroup != nullptr; }
std::size_t Renderer::sceneCount() const { return _p->count; }

std::size_t Renderer::gpuBytesFor(std::size_t count, int shDegree) {
    const std::size_t k = (std::size_t)(shDegree + 1) * (std::size_t)(shDegree + 1);
    return count * (16 + 24 + 12 * k + 4 + 12);
}

bool Renderer::upload(const SceneDesc* scenes, int numScenes) {
    Impl& g = *_p;
    g.error.clear();
    if (!valid()) { g.error = "no GPU device"; return false; }
    std::lock_guard<std::recursive_mutex> gpuLock(g.gpu->mutex);
    (*g.gpuErrorP).clear();
    g.releaseScene();
    if (!scenes || numScenes < 1 || numScenes > kMaxScenes) {
        g.error = "between 1 and 8 splat scenes can be rendered together";
        return false;
    }
    std::size_t total = 0;
    int deg = 0;
    std::vector<unsigned long long> key;
    for (int si = 0; si < numScenes; ++si) {
        const SceneDesc& s = scenes[si];
        if (s.count == 0 || !s.pos || !s.scale || !s.rot || !s.opacity || !s.dc) {
            g.error = "empty splat scene";
            return false;
        }
        total += s.count;
        if (s.shRest && s.shDegree > 0) deg = std::max(deg, std::min(3, s.shDegree));
        key.push_back(s.id);
    }
    const bool shareable = std::find(key.begin(), key.end(), 0ull) == key.end();

    // ---- the big buffers: reuse another Renderer's copy of the same scenes, or build them
    std::shared_ptr<SceneGpu> sc;
    if (shareable) {
        std::map<std::vector<unsigned long long>, std::weak_ptr<SceneGpu> >::iterator it = g.gpu->sceneCache.find(key);
        if (it != g.gpu->sceneCache.end()) {
            sc = it->second.lock();
            if (!sc) g.gpu->sceneCache.erase(it);
        }
    }
    if (!sc) {
        const std::size_t K = (std::size_t)(deg + 1) * (std::size_t)(deg + 1);   // coefficients incl. band 0
        const std::size_t shStride = 3 * K;                                    // floats per splat
        const uint64_t posBytes = (uint64_t)total * 16;
        const uint64_t covBytes = (uint64_t)total * 24;
        const uint64_t shBytes = (uint64_t)total * shStride * 4;
        const uint64_t biggest = std::max(posBytes, std::max(covBytes, shBytes));
        const uint64_t limit = std::min(g.limits.maxBufferSize, g.limits.maxStorageBufferBindingSize);
        if (total > 0xFFFFFFF0ull / 16 || biggest > limit) {
            char msg[256];
            std::snprintf(msg, sizeof msg,
                          "the scene is too large for this GPU: it needs one buffer of %.0f MB, the limit is %.0f MB. "
                          "Lower SH Degree on the ReadSplats node.",
                          (double)biggest / 1048576.0, (double)limit / 1048576.0);
            g.error = msg;
            return false;
        }
        sc = std::make_shared<SceneGpu>();
        sc->gpu = g.gpu;
        sc->posBuf = g.storage(posBytes);
        sc->covBuf = g.storage(covBytes);
        sc->shBuf = g.storage(shBytes);
        sc->nrmBuf = g.storage((uint64_t)total * 12);
        wgpuDevicePoll(g.device, false, nullptr);
        if (!sc->posBuf || !sc->covBuf || !sc->shBuf || !sc->nrmBuf || !(*g.gpuErrorP).empty()) {
            g.error = "not enough graphics memory for the scene (" + (*g.gpuErrorP) + ")";
            return false;
        }
        sc->radius.resize(total);

        // Build and send in chunks so the staging copies stay small next to the scene.
        const std::size_t chunk = 1u << 18;
        std::vector<float> stPos(chunk * 4), stCov(chunk * 6), stSh(chunk * shStride), stNrm(chunk * 3);
        std::size_t base = 0;
        for (int si = 0; si < numScenes; ++si) {
            const SceneDesc& s = scenes[si];
            sc->sceneStart[si] = base;
            const int sDeg = (s.shRest && s.shDegree > 0) ? std::min(3, s.shDegree) : 0;
            const std::size_t restK = (std::size_t)(sDeg + 1) * (std::size_t)(sDeg + 1) - 1;   // bands 1.. this scene has
            // The scene keeps bands 1.. as R.. G.. B.. over its own coefficient count.
            const std::size_t sceneK = (std::size_t)(s.shDegree + 1) * (std::size_t)(s.shDegree + 1) - 1;
            for (std::size_t start = 0; start < s.count; start += chunk) {
                const std::size_t n = std::min(chunk, s.count - start);
                if (restK + 1 < K) std::fill(stSh.begin(), stSh.begin() + n * shStride, 0.f);   // missing bands = 0
                for (std::size_t j = 0; j < n; ++j) {
                    const std::size_t i = start + j;
                    float* P = &stPos[j * 4];
                    P[0] = s.pos[i * 3]; P[1] = s.pos[i * 3 + 1]; P[2] = s.pos[i * 3 + 2];
                    P[3] = s.opacity[i];

                    // Sigma = R * diag(scale^2) * R^T
                    const float qw = s.rot[i * 4], qx = s.rot[i * 4 + 1], qy = s.rot[i * 4 + 2], qz = s.rot[i * 4 + 3];
                    const float R[3][3] = {
                        {1.f - 2.f * (qy * qy + qz * qz), 2.f * (qx * qy - qw * qz),       2.f * (qx * qz + qw * qy)},
                        {2.f * (qx * qy + qw * qz),       1.f - 2.f * (qx * qx + qz * qz), 2.f * (qy * qz - qw * qx)},
                        {2.f * (qx * qz - qw * qy),       2.f * (qy * qz + qw * qx),       1.f - 2.f * (qx * qx + qy * qy)}};
                    const float sx = s.scale[i * 3], sy = s.scale[i * 3 + 1], sz = s.scale[i * 3 + 2];
                    const float s2[3] = {sx * sx, sy * sy, sz * sz};
                    auto sig = [&](int a, int b) {
                        return R[a][0] * s2[0] * R[b][0] + R[a][1] * s2[1] * R[b][1] + R[a][2] * s2[2] * R[b][2];
                    };
                    float* C = &stCov[j * 6];
                    C[0] = sig(0, 0); C[1] = sig(0, 1); C[2] = sig(0, 2);
                    C[3] = sig(1, 1); C[4] = sig(1, 2); C[5] = sig(2, 2);
                    sc->radius[base + i] = std::max(sx, std::max(sy, sz));
                    // The axis the splat is thinnest along: what stands in for a surface normal.
                    const int thin = (sx <= sy && sx <= sz) ? 0 : (sy <= sz ? 1 : 2);
                    stNrm[j * 3] = R[0][thin]; stNrm[j * 3 + 1] = R[1][thin]; stNrm[j * 3 + 2] = R[2][thin];

                    float* H = &stSh[j * shStride];
                    H[0] = s.dc[i * 3]; H[1] = s.dc[i * 3 + 1]; H[2] = s.dc[i * 3 + 2];
                    if (restK > 0) {
                        const float* src = s.shRest + i * 3 * sceneK;
                        for (std::size_t k = 0; k < restK; ++k) {
                            H[3 + k * 3]     = src[k];
                            H[3 + k * 3 + 1] = src[sceneK + k];
                            H[3 + k * 3 + 2] = src[2 * sceneK + k];
                        }
                    }
                }
                const uint64_t at = (uint64_t)(base + start);
                wgpuQueueWriteBuffer(g.queue, sc->posBuf, at * 16, stPos.data(), n * 16);
                wgpuQueueWriteBuffer(g.queue, sc->covBuf, at * 24, stCov.data(), n * 24);
                wgpuQueueWriteBuffer(g.queue, sc->shBuf, at * shStride * 4, stSh.data(), n * shStride * 4);
                wgpuQueueWriteBuffer(g.queue, sc->nrmBuf, at * 12, stNrm.data(), n * 12);
                wgpuQueueSubmit(g.queue, 0, nullptr);      // flush this chunk's staging
                wgpuDevicePoll(g.device, false, nullptr);
                if (!(*g.gpuErrorP).empty()) {
                    g.error = "uploading the scene to the GPU failed (" + (*g.gpuErrorP) + ")";
                    return false;
                }
            }
            base += s.count;
        }
        sc->sceneStart[numScenes] = base;
        sc->count = total;
        sc->numScenes = numScenes;
        sc->shDegree = deg;
        if (shareable) g.gpu->sceneCache[key] = sc;
    }

    // ---- this Renderer's own part: the draw order and the bindings
    g.orderBuf = g.storage((uint64_t)sc->count * 4);
    wgpuDevicePoll(g.device, false, nullptr);
    if (!g.orderBuf || !(*g.gpuErrorP).empty()) {
        g.error = "not enough graphics memory for the scene (" + (*g.gpuErrorP) + ")";
        g.releaseScene();
        return false;
    }
    auto entry = [](uint32_t b, WGPUBuffer buf) {
        WGPUBindGroupEntry e = {};
        e.binding = b;
        e.buffer = buf;
        e.size = WGPU_WHOLE_SIZE;
        return e;
    };
    WGPUBindGroupEntry be[6] = {entry(0, g.uniformBuf), entry(1, sc->posBuf), entry(2, sc->covBuf),
                                entry(3, sc->shBuf), entry(4, g.orderBuf), entry(5, sc->nrmBuf)};
    WGPUBindGroupDescriptor bgd = {};
    bgd.layout = g.gpu->bindLayout;
    bgd.entryCount = 6;
    bgd.entries = be;
    g.bindGroup = wgpuDeviceCreateBindGroup(g.device, &bgd);
    if (!g.bindGroup || !(*g.gpuErrorP).empty()) {
        g.error = "binding the scene buffers failed (" + (*g.gpuErrorP) + ")";
        g.releaseScene();
        return false;
    }
    g.scene = sc;
    g.count = sc->count;
    g.numScenes = sc->numScenes;
    for (int si = 0; si <= sc->numScenes; ++si) g.sceneStart[si] = sc->sceneStart[si];
    g.shDegree = sc->shDegree;
    return true;
}

bool Renderer::draw(const float* const* pos, const DrawParams& prm, int W, int H, RenderOutput& out) {
    Impl& g = *_p;
    g.error.clear();
    out.width = W;
    out.height = H;
    out.drawn = 0;
    out.sortMs = out.gpuMs = 0.;
    if (!valid() || !hasScene() || !pos) { g.error = "no splat scene uploaded"; return false; }
    std::lock_guard<std::recursive_mutex> gpuLock(g.gpu->mutex);
    (*g.gpuErrorP).clear();
    if (W <= 0 || H <= 0) { g.error = "empty output size"; return false; }
    if ((uint32_t)W > g.limits.maxTextureDimension2D || (uint32_t)H > g.limits.maxTextureDimension2D) {
        g.error = "the output is larger than the GPU's largest texture";
        return false;
    }
    out.rgba.assign((std::size_t)W * H * 4, 0.f);
    out.depth.assign((std::size_t)W * H, 0.f);

    const std::size_t N = g.count;
    const float* sceneRadius = g.scene->radius.data();
    std::size_t M = 0;
    if (prm.reuseOrder && g.lastDrawn > 0) {
        M = g.lastDrawn;            // the order buffer on the GPU is still the one to use
    } else {
        // ---- cull, then sort far to near on view depth, across all scenes
        const auto tSort = std::chrono::steady_clock::now();
        const int T = sortThreads(N);
        g.depth.resize(N);
        // Half-extent of the picture at depth 1 (with the lens shift and a few pixels of slack
        // for the low-pass), and how far a splat reaches from its centre in standard deviations.
        const float tanX = (1.f + std::fabs(prm.ndcOffsetX) + 8.f / (float)W) / std::max(1e-6f, prm.projX);
        const float tanY = (1.f + std::fabs(prm.ndcOffsetY) + 8.f / (float)H) / std::max(1e-6f, prm.projY);
        const float reach = 3.33f * std::max(0.f, prm.splatScale);
        std::vector<std::size_t> cnt((std::size_t)T, 0);
        std::vector<float> tMin((std::size_t)T, 1e30f), tMax((std::size_t)T, -1e30f);
        for (int si = 0; si < g.numScenes; ++si) {
            float mv[16];
            mul4(prm.view, prm.model[si], mv);
            // Largest stretch of the model matrix: turns a file-space radius into a world one.
            float stretch = 0.f;
            for (int c = 0; c < 3; ++c)
                stretch = std::max(stretch, std::sqrt(mv[c * 4] * mv[c * 4] + mv[c * 4 + 1] * mv[c * 4 + 1] + mv[c * 4 + 2] * mv[c * 4 + 2]));
            const float* P = pos[si];
            const std::size_t base = g.sceneStart[si], n = g.sceneStart[si + 1] - base;
            const bool cull = prm.frustumCull;
            const float nearClip = prm.nearClip;
            parallelFor(n, T, [&, base](int t, std::size_t b, std::size_t e) {
                std::size_t c = 0;
                float lo = 1e30f, hi = -1e30f;
                for (std::size_t i = b; i < e; ++i) {
                    const float x = P[i * 3], y = P[i * 3 + 1], z = P[i * 3 + 2];
                    const float d = -(mv[2] * x + mv[6] * y + mv[10] * z + mv[14]);     // depth = -z_camera
                    float keep = d;
                    if (d < nearClip) {
                        keep = -1.f;
                    } else if (cull) {
                        const float r = sceneRadius[base + i] * stretch * reach;
                        const float xc = mv[0] * x + mv[4] * y + mv[8] * z + mv[12];
                        const float yc = mv[1] * x + mv[5] * y + mv[9] * z + mv[13];
                        if (std::fabs(xc) - r > d * tanX || std::fabs(yc) - r > d * tanY) keep = -1.f;
                    }
                    g.depth[base + i] = keep;
                    if (keep >= 0.f) {
                        ++c;
                        lo = std::min(lo, d);
                        hi = std::max(hi, d);
                    }
                }
                cnt[(std::size_t)t] += c;
                tMin[(std::size_t)t] = std::min(tMin[(std::size_t)t], lo);
                tMax[(std::size_t)t] = std::max(tMax[(std::size_t)t], hi);
            });
        }
        float dMin = 1e30f, dMax = -1e30f;
        for (int t = 0; t < T; ++t) {
            M += cnt[(std::size_t)t];
            dMin = std::min(dMin, tMin[(std::size_t)t]);
            dMax = std::max(dMax, tMax[(std::size_t)t]);
        }
        out.drawn = M;
        g.lastDrawn = M;
        if (M == 0) return true;      // nothing in view: an empty frame

        const uint32_t kBits = 11, kBuckets = 1u << kBits, kMask = kBuckets - 1;
        const float kMaxKey = (float)((1u << (2 * kBits)) - 1);
        const float keyScale = (dMax > dMin) ? kMaxKey / (dMax - dMin) : 0.f;
        g.idxA.resize(M); g.idxB.resize(M); g.keyA.resize(M); g.keyB.resize(M);
        // Compact the visible splats (single pass: the per-thread counts above are per scene
        // range, not per global range, so the write position is tracked serially here).
        {
            std::size_t m = 0;
            for (std::size_t i = 0; i < N; ++i) {
                const float d = g.depth[i];
                if (d < 0.f) continue;
                g.idxA[m] = (uint32_t)i;
                g.keyA[m] = (uint32_t)((dMax - d) * keyScale);    // far = small key = drawn first
                ++m;
            }
        }
        const int TS = sortThreads(M);
        std::vector<uint32_t> hist((std::size_t)TS * kBuckets);
        for (uint32_t pass = 0; pass < 2; ++pass) {
            const uint32_t shift = pass * kBits;
            std::vector<uint32_t>& ki = pass == 0 ? g.keyA : g.keyB;
            std::vector<uint32_t>& ko = pass == 0 ? g.keyB : g.keyA;
            std::vector<uint32_t>& ii = pass == 0 ? g.idxA : g.idxB;
            std::vector<uint32_t>& io = pass == 0 ? g.idxB : g.idxA;
            std::fill(hist.begin(), hist.end(), 0u);
            parallelFor(M, TS, [&](int t, std::size_t b, std::size_t e) {
                uint32_t* h = &hist[(std::size_t)t * kBuckets];
                for (std::size_t m = b; m < e; ++m) ++h[(ki[m] >> shift) & kMask];
            });
            // Start of each (bucket, thread) run: buckets in order, threads in order inside a
            // bucket, which keeps the sort stable.
            uint32_t running = 0;
            for (uint32_t b = 0; b < kBuckets; ++b)
                for (int t = 0; t < TS; ++t) {
                    uint32_t& h = hist[(std::size_t)t * kBuckets + b];
                    const uint32_t c = h;
                    h = running;
                    running += c;
                }
            parallelFor(M, TS, [&](int t, std::size_t b, std::size_t e) {
                uint32_t* h = &hist[(std::size_t)t * kBuckets];
                for (std::size_t m = b; m < e; ++m) {
                    const uint32_t dst = h[(ki[m] >> shift) & kMask]++;
                    ko[dst] = ki[m];
                    io[dst] = ii[m];
                }
            });
        }
        out.sortMs = msSince(tSort);     // sorted order is back in idxA
        wgpuQueueWriteBuffer(g.queue, g.orderBuf, 0, g.idxA.data(), M * 4);
    }
    out.drawn = M;
    const auto tGpu = std::chrono::steady_clock::now();

    // ---- uniforms
    Uniforms u;
    std::memset(&u, 0, sizeof u);
    std::memcpy(u.view, prm.view, sizeof u.view);
    {
        // Camera position in world space, then in each scene's own space (the SH
        // colour depends on the direction the splat is seen from, in that space).
        const float* v = prm.view;
        const float camW[3] = {-(v[0] * v[12] + v[1] * v[13] + v[2] * v[14]),
                               -(v[4] * v[12] + v[5] * v[13] + v[6] * v[14]),
                               -(v[8] * v[12] + v[9] * v[13] + v[10] * v[14])};
        for (int si = 0; si < g.numScenes; ++si) {
            std::memcpy(u.models[si], prm.model[si], 16 * sizeof(float));
            float camL[3] = {camW[0], camW[1], camW[2]};
            invAffinePoint(prm.model[si], camW, camL);
            u.camPosLocal[si][0] = camL[0]; u.camPosLocal[si][1] = camL[1]; u.camPosLocal[si][2] = camL[2];
            u.camPosLocal[si][3] = 1.f;
            u.sceneEnd[si] = (uint32_t)g.sceneStart[si + 1];
        }
    }
    u.focal[0] = prm.projX * 0.5f * (float)W;
    u.focal[1] = prm.projY * 0.5f * (float)H;
    u.viewport[0] = (float)W; u.viewport[1] = (float)H;
    u.tanFov[0] = 1.f / std::max(1e-6f, prm.projX);
    u.tanFov[1] = 1.f / std::max(1e-6f, prm.projY);
    u.nearClip = prm.nearClip;
    u.splatScale = prm.splatScale;
    u.shDegree = (uint32_t)std::max(0, std::min(prm.shDegree, g.shDegree));
    u.shStride = (uint32_t)(3 * (g.shDegree + 1) * (g.shDegree + 1));
    u.lowpass = prm.antialias ? 0.3f : 0.f;
    u.maxPx = 2.f * (float)std::max(W, H);
    u.opacityScale = prm.opacityScale;
    u.numScenes = (uint32_t)g.numScenes;
    u.ndcOffset[0] = prm.ndcOffsetX;
    u.ndcOffset[1] = prm.ndcOffsetY;
    wgpuQueueWriteBuffer(g.queue, g.uniformBuf, 0, &u, sizeof u);

    // ---- targets
    const uint32_t bpp = g.float32Blend ? 16 : 8;
    const uint32_t rowBytes = ((uint32_t)W * bpp + 255u) & ~255u;
    const uint64_t texBytes = (uint64_t)rowBytes * (uint64_t)H;
    if (g.texW != W || g.texH != H) {
        g.releaseTargets();
        WGPUTextureDescriptor td = {};
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = (uint32_t)W; td.size.height = (uint32_t)H; td.size.depthOrArrayLayers = 1;
        td.format = g.format;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        g.colorTex = wgpuDeviceCreateTexture(g.device, &td);
        g.depthTex = wgpuDeviceCreateTexture(g.device, &td);
        if (g.colorTex) g.colorView = wgpuTextureCreateView(g.colorTex, nullptr);
        if (g.depthTex) g.depthView = wgpuTextureCreateView(g.depthTex, nullptr);
        WGPUBufferDescriptor rd = {};
        rd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        rd.size = texBytes * 2;
        g.readBuf = wgpuDeviceCreateBuffer(g.device, &rd);
        g.readBytes = rd.size;
        if (!g.colorView || !g.depthView || !g.readBuf || !(*g.gpuErrorP).empty()) {
            g.error = "not enough graphics memory for the render targets (" + (*g.gpuErrorP) + ")";
            g.releaseTargets();
            return false;
        }
        g.texW = W; g.texH = H;
    }

    // ---- draw + copy to the read buffer
    WGPURenderPassColorAttachment att[2] = {};
    att[0].view = g.colorView;
    att[1].view = g.depthView;
    for (int i = 0; i < 2; ++i) {
        att[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        att[i].loadOp = WGPULoadOp_Clear;
        att[i].storeOp = WGPUStoreOp_Store;
        att[i].clearValue = WGPUColor{0., 0., 0., 1.};     // alpha = transmittance, see createPipeline
    }
    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 2;
    rp.colorAttachments = att;

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, g.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, g.bindGroup, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 4, (uint32_t)M, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    for (int i = 0; i < 2; ++i) {
        WGPUTexelCopyTextureInfo srcT = {};
        srcT.texture = i == 0 ? g.colorTex : g.depthTex;
        srcT.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferInfo dstB = {};
        dstB.buffer = g.readBuf;
        dstB.layout.offset = texBytes * (uint64_t)i;
        dstB.layout.bytesPerRow = rowBytes;
        dstB.layout.rowsPerImage = (uint32_t)H;
        WGPUExtent3D ext = {(uint32_t)W, (uint32_t)H, 1};
        wgpuCommandEncoderCopyTextureToBuffer(enc, &srcT, &dstB, &ext);
    }
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g.queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    bool mapped = false;
    WGPUBufferMapCallbackInfo mcb = {};
    mcb.mode = WGPUCallbackMode_AllowProcessEvents;
    mcb.callback = onMap;
    mcb.userdata1 = &mapped;
    wgpuBufferMapAsync(g.readBuf, WGPUMapMode_Read, 0, g.readBytes, mcb);
    while (!mapped) wgpuDevicePoll(g.device, true, nullptr);
    out.waitMs = msSince(tGpu);
    if (!(*g.gpuErrorP).empty()) {
        g.error = "the GPU reported an error while drawing (" + (*g.gpuErrorP) + ")";
        wgpuBufferUnmap(g.readBuf);
        return false;
    }
    const unsigned char* px = (const unsigned char*)wgpuBufferGetConstMappedRange(g.readBuf, 0, g.readBytes);
    if (!px) {
        g.error = "reading the picture back from the GPU failed";
        return false;
    }
    // Copy out row by row, on several threads (two full-HD float pictures is a lot of bytes).
    const bool f32 = g.float32Blend;
    parallelFor((std::size_t)H, sortThreads((std::size_t)W * (std::size_t)H), [&](int, std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y) {
            const unsigned char* rowC = px + y * rowBytes;
            const unsigned char* rowD = px + texBytes + y * rowBytes;
            float* dstC = &out.rgba[y * (std::size_t)W * 4];
            float* dstD = &out.depth[y * (std::size_t)W];
            if (f32) {
                std::memcpy(dstC, rowC, (std::size_t)W * 16);
                const float* d = (const float*)rowD;
                for (int x = 0; x < W; ++x) {
                    dstC[x * 4 + 3] = 1.f - dstC[x * 4 + 3];
                    const float a = 1.f - d[x * 4 + 3];
                    dstD[x] = a > 1e-6f ? d[x * 4] / a : 0.f;
                }
            } else {
                const uint16_t* c = (const uint16_t*)rowC;
                const uint16_t* d = (const uint16_t*)rowD;
                for (int x = 0; x < W * 4; ++x) dstC[x] = halfToFloat(c[x]);
                for (int x = 0; x < W; ++x) {
                    dstC[x * 4 + 3] = 1.f - dstC[x * 4 + 3];
                    const float a = 1.f - halfToFloat(d[x * 4 + 3]);
                    dstD[x] = a > 1e-6f ? halfToFloat(d[x * 4]) / a : 0.f;
                }
            }
        }
    });
    wgpuBufferUnmap(g.readBuf);

    out.normal.clear();
    out.matte.clear();
    out.nearestDepth.clear();
    out.nearestScene.clear();
    if (prm.aovs) {
        // ---- second pass: the same splats in the same order into three more targets
        const uint32_t aRow = ((uint32_t)W * 8u + 255u) & ~255u;
        const uint64_t aBytes = (uint64_t)aRow * (uint64_t)H;
        if (g.aovW != W || g.aovH != H) {
            g.releaseAovTargets();
            WGPUTextureDescriptor td = {};
            td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
            td.dimension = WGPUTextureDimension_2D;
            td.size.width = (uint32_t)W; td.size.height = (uint32_t)H; td.size.depthOrArrayLayers = 1;
            td.format = WGPUTextureFormat_RGBA16Float;
            td.mipLevelCount = 1;
            td.sampleCount = 1;
            bool okT = true;
            for (int i = 0; i < 3; ++i) {
                g.aovTex[i] = wgpuDeviceCreateTexture(g.device, &td);
                if (g.aovTex[i]) g.aovView[i] = wgpuTextureCreateView(g.aovTex[i], nullptr);
                okT = okT && g.aovView[i];
            }
            WGPUBufferDescriptor rd = {};
            rd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
            rd.size = aBytes * 3;
            g.aovReadBuf = wgpuDeviceCreateBuffer(g.device, &rd);
            g.aovReadBytes = rd.size;
            if (!okT || !g.aovReadBuf || !(*g.gpuErrorP).empty()) {
                g.error = "not enough graphics memory for the extra layers (" + (*g.gpuErrorP) + ")";
                g.releaseAovTargets();
                return false;
            }
            g.aovW = W; g.aovH = H;
        }
        WGPURenderPassColorAttachment aatt[3] = {};
        for (int i = 0; i < 3; ++i) {
            aatt[i].view = g.aovView[i];
            aatt[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            aatt[i].loadOp = WGPULoadOp_Clear;
            aatt[i].storeOp = WGPUStoreOp_Store;
            aatt[i].clearValue = WGPUColor{0., 0., 0., 1.};
        }
        WGPURenderPassDescriptor arp = {};
        arp.colorAttachmentCount = 3;
        arp.colorAttachments = aatt;
        WGPUCommandEncoder aenc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        WGPURenderPassEncoder apass = wgpuCommandEncoderBeginRenderPass(aenc, &arp);
        wgpuRenderPassEncoderSetPipeline(apass, g.gpu->pipelineAov);
        wgpuRenderPassEncoderSetBindGroup(apass, 0, g.bindGroup, 0, nullptr);
        wgpuRenderPassEncoderDraw(apass, 4, (uint32_t)M, 0, 0);
        wgpuRenderPassEncoderEnd(apass);
        wgpuRenderPassEncoderRelease(apass);
        for (int i = 0; i < 3; ++i) {
            WGPUTexelCopyTextureInfo srcT = {};
            srcT.texture = g.aovTex[i];
            srcT.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferInfo dstB = {};
            dstB.buffer = g.aovReadBuf;
            dstB.layout.offset = aBytes * (uint64_t)i;
            dstB.layout.bytesPerRow = aRow;
            dstB.layout.rowsPerImage = (uint32_t)H;
            WGPUExtent3D ext = {(uint32_t)W, (uint32_t)H, 1};
            wgpuCommandEncoderCopyTextureToBuffer(aenc, &srcT, &dstB, &ext);
        }
        WGPUCommandBuffer acmd = wgpuCommandEncoderFinish(aenc, nullptr);
        wgpuQueueSubmit(g.queue, 1, &acmd);
        wgpuCommandBufferRelease(acmd);
        wgpuCommandEncoderRelease(aenc);

        bool amapped = false;
        WGPUBufferMapCallbackInfo amcb = {};
        amcb.mode = WGPUCallbackMode_AllowProcessEvents;
        amcb.callback = onMap;
        amcb.userdata1 = &amapped;
        wgpuBufferMapAsync(g.aovReadBuf, WGPUMapMode_Read, 0, g.aovReadBytes, amcb);
        while (!amapped) wgpuDevicePoll(g.device, true, nullptr);
        if (!(*g.gpuErrorP).empty()) {
            g.error = "the GPU reported an error while drawing the extra layers (" + (*g.gpuErrorP) + ")";
            wgpuBufferUnmap(g.aovReadBuf);
            return false;
        }
        const unsigned char* apx = (const unsigned char*)wgpuBufferGetConstMappedRange(g.aovReadBuf, 0, g.aovReadBytes);
        if (!apx) {
            g.error = "reading the extra layers back from the GPU failed";
            return false;
        }
        const std::size_t np = (std::size_t)W * (std::size_t)H;
        out.normal.assign(np * 3, 0.f);
        out.matte.assign(np * 3, 0.f);
        out.nearestDepth.assign(np, 0.f);
        out.nearestScene.assign(np, 0.f);
        parallelFor((std::size_t)H, sortThreads(np), [&](int, std::size_t y0, std::size_t y1) {
            for (std::size_t y = y0; y < y1; ++y) {
                const uint16_t* rn = (const uint16_t*)(apx + y * aRow);
                const uint16_t* rm = (const uint16_t*)(apx + aBytes + y * aRow);
                const uint16_t* rd = (const uint16_t*)(apx + 2 * aBytes + y * aRow);
                for (int x = 0; x < W; ++x) {
                    const std::size_t i = y * (std::size_t)W + (std::size_t)x;
                    // normal: blended vector, back to unit length
                    const float nx = halfToFloat(rn[x * 4]), ny = halfToFloat(rn[x * 4 + 1]), nz = halfToFloat(rn[x * 4 + 2]);
                    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (nl > 1e-6f) {
                        out.normal[i * 3] = nx / nl; out.normal[i * 3 + 1] = ny / nl; out.normal[i * 3 + 2] = nz / nl;
                    }
                    for (int c = 0; c < 3; ++c) out.matte[i * 3 + c] = halfToFloat(rm[x * 4 + c]);
                    if (halfToFloat(rd[x * 4 + 3]) < 0.5f) {       // transmittance 0 = a solid splat was written
                        out.nearestDepth[i] = halfToFloat(rd[x * 4]);
                        out.nearestScene[i] = halfToFloat(rd[x * 4 + 1]);
                    }
                }
            }
        });
        wgpuBufferUnmap(g.aovReadBuf);
    }
    out.gpuMs = msSince(tGpu);
    return true;
}

}  // namespace SplatCore
