#include "FastVolumeCore.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "webgpu.h"
#include "wgpu.h"

namespace FastVolume {

// ------------------------------------------------------------- compression
namespace {

struct Compressed {
    std::vector<uint32_t> table;
    std::vector<float> headers;      // lo, scale per brick
    std::vector<uint8_t> qdata;
    int off[3] = {0, 0, 0};
    int dims[3] = {0, 0, 0};
    int nb[3] = {0, 0, 0};
    bool empty() const { return qdata.empty(); }
};

Compressed compressGrid(const openvdb::FloatGrid::ConstPtr& grid) {
    Compressed c;
    if (!grid || grid->empty()) return c;
    openvdb::CoordBBox bbox = grid->evalActiveVoxelBoundingBox();
    for (int i = 0; i < 3; ++i) {
        int lo = bbox.min()[i] & ~7;
        int hi = (bbox.max()[i] + 8) & ~7;
        c.off[i] = lo;
        c.dims[i] = hi - lo;
        c.nb[i] = c.dims[i] / 8;
    }
    c.table.assign((size_t)c.nb[0] * c.nb[1] * c.nb[2], 0xffffffffu);

    for (auto leaf = grid->tree().cbeginLeaf(); leaf; ++leaf) {
        const openvdb::Coord o = leaf->origin();
        const int bx = (o.x() - c.off[0]) >> 3;
        const int by = (o.y() - c.off[1]) >> 3;
        const int bz = (o.z() - c.off[2]) >> 3;

        float lo = 1e30f, hi = -1e30f;
        float vals[512];
        for (uint32_t off = 0; off < 512; ++off) {
            // leaf buffer order x<<6|y<<3|z; ours z*64+y*8+x
            const int x = (off >> 6) & 7, y = (off >> 3) & 7, z = off & 7;
            const float v = leaf->getValue(off);
            vals[z * 64 + y * 8 + x] = v;
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        const float scale = hi > lo ? hi - lo : 1.0f;
        const uint32_t bi = (uint32_t)(c.headers.size() / 2);
        c.table[((size_t)bz * c.nb[1] + by) * c.nb[0] + bx] = bi;
        c.headers.push_back(lo);
        c.headers.push_back(scale);
        for (int i = 0; i < 512; ++i) {
            float q = (vals[i] - lo) / scale * 255.0f + 0.5f;
            c.qdata.push_back((uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q));
        }
    }
    while (c.qdata.size() % 4) c.qdata.push_back(0);
    return c;
}

// --------------------------------------------------------------- small math
struct Mat34 {                     // 3x3 linear part (columns) + translation
    double m[3][3];                // m[row][col]
    double t[3];
    static Mat34 identity() {
        Mat34 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
        return r;
    }
};

Mat34 mul(const Mat34& a, const Mat34& b) {  // a after b: x -> a(b(x))
    Mat34 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
        r.t[i] = a.t[i];
        for (int k = 0; k < 3; ++k) r.t[i] += a.m[i][k] * b.t[k];
    }
    return r;
}

bool invert(const Mat34& a, Mat34& out) {
    const double (*m)[3] = a.m;
    const double det =
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-20) return false;
    const double id = 1.0 / det;
    out.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
    out.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    out.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    out.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
    out.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    out.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    out.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
    out.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    out.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    for (int i = 0; i < 3; ++i) {
        out.t[i] = 0;
        for (int k = 0; k < 3; ++k) out.t[i] -= out.m[i][k] * a.t[k];
    }
    return true;
}

void apply(const Mat34& a, const double* v, double* out, bool point) {
    for (int i = 0; i < 3; ++i) {
        out[i] = point ? a.t[i] : 0.0;
        for (int k = 0; k < 3; ++k) out[i] += a.m[i][k] * v[k];
    }
}

// index->world transform of a grid, robust to OpenVDB map conventions:
// probe the map with basis vectors instead of decoding matrix layout.
Mat34 gridIndexToWorld(const openvdb::FloatGrid::ConstPtr& grid) {
    Mat34 r = Mat34::identity();
    if (!grid) return r;
    const openvdb::math::Transform& xf = grid->transform();
    openvdb::Vec3d o = xf.indexToWorld(openvdb::Vec3d(0, 0, 0));
    openvdb::Vec3d ex = xf.indexToWorld(openvdb::Vec3d(1, 0, 0)) - o;
    openvdb::Vec3d ey = xf.indexToWorld(openvdb::Vec3d(0, 1, 0)) - o;
    openvdb::Vec3d ez = xf.indexToWorld(openvdb::Vec3d(0, 0, 1)) - o;
    for (int i = 0; i < 3; ++i) {
        r.m[i][0] = ex[i];
        r.m[i][1] = ey[i];
        r.m[i][2] = ez[i];
        r.t[i] = o[i];
    }
    return r;
}

void replaceAll(std::string& s, const std::string& k, const std::string& v) {
    for (size_t p = 0; (p = s.find(k, p)) != std::string::npos; p += v.size())
        s.replace(p, k.size(), v);
}

std::string vec3(const double* v) {
    char b[128];
    snprintf(b, sizeof b, "vec3<f32>(%.8f, %.8f, %.8f)", v[0], v[1], v[2]);
    return b;
}
std::string vec3i(const int* v) {
    char b[96];
    snprintf(b, sizeof b, "vec3<i32>(%d, %d, %d)", v[0], v[1], v[2]);
    return b;
}
std::string f32(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.8f", v);
    return b;
}

const char* FETCH_TMPL = R"(
fn fetch_P(vw: vec3<i32>) -> f32 {
    let v = vw - P_OFF;
    if (any(v < vec3<i32>(0)) || any(v >= P_DIMS)) { return 0.0; }
    let b = vec3<u32>(v) >> vec3<u32>(3u);
    let w = vec3<u32>(v) & vec3<u32>(7u);
    let nb = vec3<u32>(P_NB);
    let bi = P_table[(b.z * nb.y + b.y) * nb.x + b.x];
    if (bi == 0xffffffffu) { return 0.0; }
    let h = P_headers[bi];
    let vi = bi * 512u + w.z * 64u + w.y * 8u + w.x;
    let word = P_vdata[vi >> 2u];
    let byte = (word >> ((vi & 3u) * 8u)) & 0xffu;
    return h.x + f32(byte) * (1.0 / 255.0) * h.y;
}

fn occupied_P(vw: vec3<i32>) -> bool {
    let v = vw - P_OFF;
    if (any(v < vec3<i32>(0)) || any(v >= P_DIMS)) { return false; }
    let b = vec3<u32>(v) >> vec3<u32>(3u);
    let nb = vec3<u32>(P_NB);
    return P_table[(b.z * nb.y + b.y) * nb.x + b.x] != 0xffffffffu;
}

fn tri_P(p: vec3<f32>) -> f32 {
    let q = p - vec3<f32>(0.5);
    let f = floor(q);
    let t = q - f;
    let i = vec3<i32>(f);
    let c00 = mix(fetch_P(i), fetch_P(i + vec3<i32>(1, 0, 0)), t.x);
    let c10 = mix(fetch_P(i + vec3<i32>(0, 1, 0)), fetch_P(i + vec3<i32>(1, 1, 0)), t.x);
    let c01 = mix(fetch_P(i + vec3<i32>(0, 0, 1)), fetch_P(i + vec3<i32>(1, 0, 1)), t.x);
    let c11 = mix(fetch_P(i + vec3<i32>(0, 1, 1)), fetch_P(i + vec3<i32>(1, 1, 1)), t.x);
    return mix(mix(c00, c10, t.y), mix(c01, c11, t.y), t.z);
}
)";

std::string fetchFor(const char* lower, const char* upper) {
    std::string s = FETCH_TMPL;
    replaceAll(s, "P_OFF", std::string(upper) + "_OFF");
    replaceAll(s, "P_DIMS", std::string(upper) + "_DIMS");
    replaceAll(s, "P_NB", std::string(upper) + "_NB");
    replaceAll(s, "P_table", std::string(lower) + "_table");
    replaceAll(s, "P_headers", std::string(lower) + "_headers");
    replaceAll(s, "P_vdata", std::string(lower) + "_vdata");
    replaceAll(s, "_P(", std::string("_") + lower + "(");
    return s;
}

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

}  // namespace

// One light in the uniform (std140: 5×vec4 = 80 B). All vectors are INDEX
// space (draw() converts from world). pos.w carries the type as a float.
struct GpuLight {
    float pos[3];  float type;      // index pos (point/spot/area centre), type
    float dir[3];  float cosInner;  // index dir: DISTANT/AREA=to-light, SPOT=axis | spot cosInner
    float col[3];  float cosOuter;  // color * intensity | spot cosOuter
    float uax[3];  float halfU;     // area beam width axis (index) | half width (world)
    float vax[3];  float halfV;     // area beam height axis (index) | half height (world)
};
static_assert(sizeof(GpuLight) == 80, "GpuLight must be std140 vec4*5");

// Per-frame uniform block — must match `struct Uni` in the WGSL byte-for-byte
// (std140 16-byte rows). Globals (128 B) then the light array. draw() rewrites
// this buffer instead of recompiling shaders.
struct Uniforms {
    float    cam[3];     float    tanfov;      // CAM, TANFOV
    float    fwd[3];     float    tanfovv;     // FWD, TANFOVV
    float    right[3];   float    lfacf;       // RIGHT, LFACF
    float    up[3];      float    sigma;       // UP, SIGMA
    float    ambient[3]; float    albedo;      // AMBIENT, ALBEDO
    int32_t  lg[3];      int32_t  steps;       // LG, STEPS
    float    hg_g; float fire_k; float fire_max; int32_t numLights;
    uint32_t w, h, npix; float ref2;           // W, H, NPIX, point-falloff ref dist^2
    float    fire_light; float _fp0, _fp1, _fp2; // fire->smoke illumination scale
    GpuLight lights[MAX_LIGHTS];
};
static_assert(MAX_LIGHTS == 8, "WGSL array size below is hard-coded to 8");
static_assert(sizeof(Uniforms) == 144 + 80 * 8, "Uniforms must match WGSL std140 layout");

// WGSL declaration of the uniform block (same field order/packing as above).
const char* UNI_STRUCT =
    "struct Light {\n"
    "  pos: vec4<f32>, dir: vec4<f32>, col: vec4<f32>, uax: vec4<f32>, vax: vec4<f32>,\n"
    "};\n"
    "struct Uni {\n"
    "  cam: vec3<f32>, tanfov: f32,\n"
    "  fwd: vec3<f32>, tanfovv: f32,\n"
    "  right: vec3<f32>, lfacf: f32,\n"
    "  up: vec3<f32>, sigma: f32,\n"
    "  ambient: vec3<f32>, albedo: f32,\n"
    "  lg: vec3<i32>, steps: i32,\n"
    "  hg_g: f32, fire_k: f32, fire_max: f32, num_lights: i32,\n"
    "  w: u32, h: u32, npix: u32, ref2: f32,\n"
    "  fire_light: f32, fp0: f32, fp1: f32, fp2: f32,\n"
    "  lights: array<Light, 8>,\n"
    "};\n";

// Resident compressed volume on the GPU — the cached product of upload().
// Holds the brick storage buffers, the baked-geometry compute pipelines, plus
// the geometry/transform draw() needs CPU-side. Look and camera are NOT here;
// they go through the per-frame uniform so knob tweaks reuse all of this.
struct Uploaded {
    WGPUBuffer dTable = nullptr, dHead = nullptr, dData = nullptr;
    WGPUBuffer fTable = nullptr, fHead = nullptr, fData = nullptr;  // fb (=d if no flames)
    WGPUComputePipeline lightPipe = nullptr, renderPipe = nullptr;  // baked geometry, compiled once
    int dOff[3] = {0,0,0}, dDims[3] = {0,0,0}, dNb[3] = {0,0,0};
    int fOff[3] = {0,0,0}, fDims[3] = {0,0,0}, fNb[3] = {0,0,0};
    bool fEmpty = true;
    double gmin[3] = {0,0,0}, gmax[3] = {0,0,0};
    Mat34 i2w = Mat34::identity();
    Mat34 w2i = Mat34::identity();
    bool valid = false;
};

// -------------------------------------------------------------------- Impl
struct Renderer::Impl {
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    std::string error;
    std::string adapterName;
    Uploaded vol;
    WGPUBuffer uniformBuf = nullptr;   // resident per-frame uniform (160 B)

    void releaseVol() {
        for (WGPUBuffer b : {vol.dTable, vol.dHead, vol.dData,
                             vol.fTable, vol.fHead, vol.fData})
            if (b) wgpuBufferRelease(b);
        if (vol.lightPipe) wgpuComputePipelineRelease(vol.lightPipe);
        if (vol.renderPipe) wgpuComputePipelineRelease(vol.renderPipe);
        vol = Uploaded{};
    }

    WGPUBuffer ensureUniform(size_t size) {
        if (!uniformBuf) {
            WGPUBufferDescriptor bd = {};
            bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            bd.size = size;
            uniformBuf = wgpuDeviceCreateBuffer(device, &bd);
        }
        return uniformBuf;
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

        WGPURequestDeviceCallbackInfo dcb = {};
        dcb.mode = WGPUCallbackMode_AllowProcessEvents;
        dcb.callback = onDevice;
        dcb.userdata1 = &device;
        wgpuAdapterRequestDevice(adapter, nullptr, dcb);
        for (int i = 0; i < 10000 && !device; ++i) wgpuInstanceProcessEvents(instance);
        if (!device) { error = "device request failed"; return false; }
        queue = wgpuDeviceGetQueue(device);

        WGPUAdapterInfo info = {};
        wgpuAdapterGetInfo(adapter, &info);
        adapterName.assign(info.device.data,
                           info.device.length == WGPU_STRLEN
                               ? strlen(info.device.data) : info.device.length);
        return true;
    }

    WGPUBuffer storage(const void* data, size_t size, bool copySrc = false) {
        WGPUBufferDescriptor bd = {};
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst
                   | (copySrc ? WGPUBufferUsage_CopySrc : 0);
        bd.size = size < 16 ? 16 : size;
        WGPUBuffer b = wgpuDeviceCreateBuffer(device, &bd);
        if (data && size) wgpuQueueWriteBuffer(queue, b, 0, data, size);
        return b;
    }

    WGPUComputePipeline pipeline(const std::string& code) {
        WGPUShaderSourceWGSL src = {};
        src.chain.sType = WGPUSType_ShaderSourceWGSL;
        src.code = sv(code.c_str());
        WGPUShaderModuleDescriptor smd = {};
        smd.nextInChain = &src.chain;
        WGPUShaderModule mod = wgpuDeviceCreateShaderModule(device, &smd);
        WGPUComputePipelineDescriptor pd = {};
        pd.compute.module = mod;
        pd.compute.entryPoint = sv("main");
        WGPUComputePipeline p = wgpuDeviceCreateComputePipeline(device, &pd);
        wgpuShaderModuleRelease(mod);
        return p;
    }

    ~Impl() {
        releaseVol();
        if (uniformBuf) wgpuBufferRelease(uniformBuf);
    }
};

Renderer::Renderer() : _p(new Impl) { _p->init(); }
Renderer::~Renderer() = default;
bool Renderer::hasVolume() const { return _p->vol.valid; }
bool Renderer::valid() const { return _p->device != nullptr; }
const std::string& Renderer::lastError() const { return _p->error; }
std::string Renderer::adapterName() const { return _p->adapterName; }

// ------------------------------------------------------------------ upload
// Expensive, look-independent half: compress the grids, create the resident
// brick buffers, and cache the geometry/transform. Cached on the Renderer so
// repeated draw() calls (knob tweaks) reuse it.
bool Renderer::upload(const openvdb::FloatGrid::ConstPtr& density,
                      const openvdb::FloatGrid::ConstPtr& flames,
                      const float* volWorldMatrix) {
    if (!valid()) return false;
    if (!density || density->empty()) { _p->error = "empty density grid"; return false; }

    Compressed d = compressGrid(density);
    Compressed f = compressGrid(flames);
    const Compressed& fb = f.empty() ? d : f;  // placeholder bindings

    // ---- transforms: index space -> world space
    Mat34 i2w = gridIndexToWorld(density);
    if (volWorldMatrix) {
        Mat34 node{};
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) node.m[r][c] = volWorldMatrix[c * 4 + r];
        for (int r = 0; r < 3; ++r) node.t[r] = volWorldMatrix[12 + r];
        i2w = mul(node, i2w);
    }
    Mat34 w2i;
    if (!invert(i2w, w2i)) { _p->error = "singular volume transform"; return false; }

    // march bounds: union of density and flames index bboxes
    double gmin[3], gmax[3];
    for (int i = 0; i < 3; ++i) {
        gmin[i] = d.off[i];
        gmax[i] = d.off[i] + d.dims[i];
        if (!f.empty()) {
            gmin[i] = std::min(gmin[i], (double)f.off[i]);
            gmax[i] = std::max(gmax[i], (double)f.off[i] + f.dims[i]);
        }
    }

    // ---- (re)create resident GPU brick buffers, replacing any prior volume
    Impl& g = *_p;
    g.releaseVol();
    Uploaded& v = g.vol;
    v.dTable = g.storage(d.table.data(), d.table.size() * 4);
    v.dHead  = g.storage(d.headers.data(), d.headers.size() * 4);
    v.dData  = g.storage(d.qdata.data(), d.qdata.size());
    v.fTable = g.storage(fb.table.data(), fb.table.size() * 4);
    v.fHead  = g.storage(fb.headers.data(), fb.headers.size() * 4);
    v.fData  = g.storage(fb.qdata.data(), fb.qdata.size());
    for (int i = 0; i < 3; ++i) {
        v.dOff[i] = d.off[i];  v.dDims[i] = d.dims[i];  v.dNb[i] = d.nb[i];
        v.fOff[i] = fb.off[i]; v.fDims[i] = fb.dims[i]; v.fNb[i] = fb.nb[i];
        v.gmin[i] = gmin[i];   v.gmax[i] = gmax[i];
    }
    v.fEmpty = f.empty();
    v.i2w = i2w;
    v.w2i = w2i;

    // ---- bake the upload-stable geometry into WGSL consts. Everything that
    // changes per frame (look, camera, sun, resolution) is read from the
    // uniform block instead, so these pipelines compile ONCE per upload and
    // every draw() reuses them — no per-frame shader recompilation.
    static const int kZero3[3] = {0, 0, 0};
    std::string geom;
    geom += "const GMIN = " + vec3(gmin) + ";\nconst GMAX = " + vec3(gmax) + ";\n";
    geom += "const D_OFF = " + vec3i(d.off) + ";\nconst D_DIMS = " + vec3i(d.dims)
          + ";\nconst D_NB = " + vec3i(d.nb) + ";\n";
    geom += "const F_OFF = " + vec3i(fb.off) + ";\nconst F_DIMS = "
          + vec3i(f.empty() ? kZero3 : fb.dims) + ";\nconst F_NB = " + vec3i(fb.nb) + ";\n";
    {
        double m0[3] = {i2w.m[0][0], i2w.m[0][1], i2w.m[0][2]};
        double m1[3] = {i2w.m[1][0], i2w.m[1][1], i2w.m[1][2]};
        double m2[3] = {i2w.m[2][0], i2w.m[2][1], i2w.m[2][2]};
        geom += "const M0 = " + vec3(m0) + ";\nconst M1 = " + vec3(m1)
              + ";\nconst M2 = " + vec3(m2) + ";\n";
    }

    std::string lightSrc = std::string(R"(
@group(0) @binding(0) var<storage, read> d_table: array<u32>;
@group(0) @binding(1) var<storage, read> d_headers: array<vec2<f32>>;
@group(0) @binding(2) var<storage, read> d_vdata: array<u32>;
@group(0) @binding(3) var<storage, read_write> lgrid: array<f32>;
@group(0) @binding(4) var<uniform> U: Uni;
@group(0) @binding(5) var<storage, read> f_table: array<u32>;
@group(0) @binding(6) var<storage, read> f_headers: array<vec2<f32>>;
@group(0) @binding(7) var<storage, read> f_vdata: array<u32>;
)") + UNI_STRUCT + geom + fetchFor("d", "D") + fetchFor("f", "F") + R"(
// Per-light transmittance grids in lgrid[light*cells + cell], plus one extra
// slice at lgrid[num_lights*cells + cell] holding a blurred flame value — the
// fire's own light spread into the surrounding cells (sampled in the march).
@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let g = vec3<i32>(gid);
    if (any(g >= U.lg)) { return; }
    let cells = u32(U.lg.x) * u32(U.lg.y) * u32(U.lg.z);
    let idx = (u32(g.z) * u32(U.lg.y) + u32(g.y)) * u32(U.lg.x) + u32(g.x);
    let cell = GMIN + (vec3<f32>(g) + 0.5) * U.lfacf;
    for (var l = 0; l < U.num_lights; l = l + 1) {
        let L = U.lights[l];
        let lt = i32(L.pos.w);
        var dir: vec3<f32>;
        var maxd = 1.0e9;
        if (lt == 1 || lt == 3) {           // distant / area beam: fixed parallel dir
            dir = normalize(L.dir.xyz);
        } else {                            // point/spot: march toward the position
            let dd = L.pos.xyz - cell;
            maxd = length(dd);
            dir = dd / max(maxd, 1.0e-6);
        }
        let stepw = length(vec3<f32>(dot(M0, dir), dot(M1, dir), dot(M2, dir))) * U.lfacf;
        var p = cell + dir * (U.lfacf * 0.5);
        var od = 0.0;
        var travelled = U.lfacf * 0.5;
        for (var s = 0u; s < 1024u; s = s + 1u) {
            if (any(p < GMIN) || any(p > GMAX) || travelled >= maxd) { break; }
            od = od + tri_d(p) * stepw;
            p = p + dir * U.lfacf;
            travelled = travelled + U.lfacf;
        }
        lgrid[u32(l) * cells + idx] = exp(-U.sigma * od);
    }
    // Fire-glow slice: DILATE the flame field outward (max over a 2-ring
    // neighbourhood) so the fire's emission spreads into the surrounding smoke
    // WITHOUT dimming the core (a box blur would). Inner ring at full strength,
    // outer ring attenuated. tri_f is 0 when there is no flames grid.
    let R = U.lfacf * 1.5;
    var glow = tri_f(cell);
    glow = max(glow, tri_f(cell + vec3<f32>( R, 0.0, 0.0)));
    glow = max(glow, tri_f(cell + vec3<f32>(-R, 0.0, 0.0)));
    glow = max(glow, tri_f(cell + vec3<f32>(0.0,  R, 0.0)));
    glow = max(glow, tri_f(cell + vec3<f32>(0.0, -R, 0.0)));
    glow = max(glow, tri_f(cell + vec3<f32>(0.0, 0.0,  R)));
    glow = max(glow, tri_f(cell + vec3<f32>(0.0, 0.0, -R)));
    let R2 = R * 2.0;
    var outer = tri_f(cell + vec3<f32>( R2, 0.0, 0.0));
    outer = max(outer, tri_f(cell + vec3<f32>(-R2, 0.0, 0.0)));
    outer = max(outer, tri_f(cell + vec3<f32>(0.0,  R2, 0.0)));
    outer = max(outer, tri_f(cell + vec3<f32>(0.0, -R2, 0.0)));
    outer = max(outer, tri_f(cell + vec3<f32>(0.0, 0.0,  R2)));
    outer = max(outer, tri_f(cell + vec3<f32>(0.0, 0.0, -R2)));
    glow = max(glow, outer * 0.5);
    lgrid[u32(U.num_lights) * cells + idx] = glow;
}
)";

    std::string renderSrc = std::string(R"(
@group(0) @binding(0) var<storage, read> d_table: array<u32>;
@group(0) @binding(1) var<storage, read> d_headers: array<vec2<f32>>;
@group(0) @binding(2) var<storage, read> d_vdata: array<u32>;
@group(0) @binding(3) var<storage, read> f_table: array<u32>;
@group(0) @binding(4) var<storage, read> f_headers: array<vec2<f32>>;
@group(0) @binding(5) var<storage, read> f_vdata: array<u32>;
@group(0) @binding(6) var<storage, read> lgrid: array<f32>;
@group(0) @binding(7) var<storage, read_write> out_img: array<vec4<f32>>;
@group(0) @binding(8) var<uniform> U: Uni;
)") + UNI_STRUCT + geom + fetchFor("d", "D") + fetchFor("f", "F") + R"(
fn lsample(g: vec3<i32>, base: u32) -> f32 {
    let c = clamp(g, vec3<i32>(0), U.lg - 1);
    return lgrid[base + (u32(c.z) * u32(U.lg.y) + u32(c.y)) * u32(U.lg.x) + u32(c.x)];
}

// Trilinear sample of light `base`'s transmittance grid slice at index point p.
fn light_tri(p: vec3<f32>, base: u32) -> f32 {
    let q = (p - GMIN) / U.lfacf - 0.5;
    let f = floor(q);
    let t = q - f;
    let i = vec3<i32>(f);
    let c00 = mix(lsample(i, base), lsample(i + vec3<i32>(1, 0, 0), base), t.x);
    let c10 = mix(lsample(i + vec3<i32>(0, 1, 0), base), lsample(i + vec3<i32>(1, 1, 0), base), t.x);
    let c01 = mix(lsample(i + vec3<i32>(0, 0, 1), base), lsample(i + vec3<i32>(1, 0, 1), base), t.x);
    let c11 = mix(lsample(i + vec3<i32>(0, 1, 1), base), lsample(i + vec3<i32>(1, 1, 1), base), t.x);
    return mix(mix(c00, c10, t.y), mix(c01, c11, t.y), t.z);
}

fn fire_color(fl: f32) -> vec3<f32> {
    let x = clamp(fl / U.fire_max, 0.0, 1.0);
    return vec3<f32>(pow(x, 0.6), pow(x, 1.7), pow(x, 4.0)) * (0.4 + 4.0 * x);
}

fn hg(cos_theta: f32, g: f32) -> f32 {
    let g2 = g * g;
    return (1.0 - g2) / (12.5664 * pow(1.0 + g2 - 2.0 * g * cos_theta, 1.5));
}

fn ray_box(ro: vec3<f32>, rd: vec3<f32>) -> vec2<f32> {
    let inv = 1.0 / rd;
    let t0 = (GMIN - ro) * inv;
    let t1 = (GMAX - ro) * inv;
    let tmin = min(t0, t1);
    let tmax = max(t0, t1);
    return vec2<f32>(
        max(max(max(tmin.x, tmin.y), tmin.z), 0.0),
        min(min(tmax.x, tmax.y), tmax.z));
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= U.w || gid.y >= U.h) { return; }
    let u = (f32(gid.x) + 0.5) / f32(U.w) * 2.0 - 1.0;
    let v = 1.0 - (f32(gid.y) + 0.5) / f32(U.h) * 2.0;
    var rd = normalize(U.fwd + U.right * (u * U.tanfov) + U.up * (v * U.tanfovv));
    let tt = ray_box(U.cam, rd);

    // world length of one index-space unit along this ray, world ray dir
    let rdw = vec3<f32>(dot(M0, rd), dot(M1, rd), dot(M2, rd));
    let wpi = length(rdw);
    let rdw_n = rdw / wpi;
    let cells = u32(U.lg.x) * u32(U.lg.y) * u32(U.lg.z);

    var transmittance = 1.0;
    var rad_sun = vec3<f32>(0.0);
    var rad_amb = vec3<f32>(0.0);
    var rad_emit = vec3<f32>(0.0);
    var depth_acc = 0.0;
    var temp_acc = 0.0;
    var shadow_acc = 0.0;
    var weight_acc = 0.0;

    if (tt.y > tt.x) {
        let dt = (tt.y - tt.x) / f32(U.steps);
        let dtw = dt * wpi;
        var t = tt.x;
        var iter = 0u;
        loop {
            if (t >= tt.y || iter >= 16384u || transmittance < 0.005) { break; }
            iter = iter + 1u;
            let p = U.cam + rd * (t + 0.5 * dt);
            let vw = vec3<i32>(floor(p));
            if (occupied_d(vw) || occupied_f(vw)) {
                let den = tri_d(p);
                let fl = tri_f(p);
                if (den > 0.0001 || fl > 0.001) {
                    let ext = max(den, 0.0) * U.sigma;
                    let t_step = exp(-ext * dtw);
                    // Sum every light's single-scatter contribution.
                    var sun_total = vec3<f32>(0.0);
                    var vis0 = 0.0;
                    for (var l = 0; l < U.num_lights; l = l + 1) {
                        let L = U.lights[l];
                        let lt = i32(L.pos.w);
                        var diri: vec3<f32>;        // index-space direction TO the light
                        var falloff = 1.0;
                        if (lt == 1 || lt == 3) {   // distant / area beam: parallel
                            diri = normalize(L.dir.xyz);
                        } else {                    // point / spot
                            let dd = L.pos.xyz - p;
                            let ddw = vec3<f32>(dot(M0, dd), dot(M1, dd), dot(M2, dd));
                            falloff = U.ref2 / (U.ref2 + dot(ddw, ddw));  // 0.5 at ref dist
                            diri = dd / max(length(dd), 1.0e-6);
                        }
                        let dirw = normalize(vec3<f32>(dot(M0, diri), dot(M1, diri), dot(M2, diri)));
                        var cone = 1.0;
                        if (lt == 2) {             // spot cone around the shine axis L.dir
                            let axisw = normalize(vec3<f32>(dot(M0, L.dir.xyz), dot(M1, L.dir.xyz), dot(M2, L.dir.xyz)));
                            cone = smoothstep(L.col.w, L.dir.w, dot(-dirw, axisw));  // cosOuter,cosInner
                        } else if (lt == 3) {      // area: rectangular beam cross-section mask
                            let dd = p - L.pos.xyz;                          // index offset from centre
                            let dw = vec3<f32>(dot(M0, dd), dot(M1, dd), dot(M2, dd));
                            let uw = normalize(vec3<f32>(dot(M0, L.uax.xyz), dot(M1, L.uax.xyz), dot(M2, L.uax.xyz)));
                            let vw = normalize(vec3<f32>(dot(M0, L.vax.xyz), dot(M1, L.vax.xyz), dot(M2, L.vax.xyz)));
                            let mu = 1.0 - smoothstep(L.uax.w * 0.7, L.uax.w, abs(dot(dw, uw)));
                            let mv = 1.0 - smoothstep(L.vax.w * 0.7, L.vax.w, abs(dot(dw, vw)));
                            cone = mu * mv;        // inside the Width x Height rectangle
                        }
                        let vis = light_tri(p, u32(l) * cells);
                        let ph = hg(dot(rdw_n, dirw), U.hg_g);
                        sun_total = sun_total + L.col.xyz *
                            ((vis * ph + sqrt(vis) * 0.0796 * 0.25) * falloff * cone);
                        if (l == 0) { vis0 = vis; }
                    }
                    // Fire as a light: blurred flame field (extra lgrid slice)
                    // mapped through the fire ramp, scattered by the smoke.
                    let fglow = light_tri(p, u32(U.num_lights) * cells);
                    let fire_scatter = fire_color(fglow) * U.fire_light;
                    let amb_term = U.ambient * (0.15 + 0.85 * vis0) + fire_scatter;
                    let wgt = transmittance * (1.0 - t_step);
                    rad_sun = rad_sun + U.albedo * sun_total * wgt;
                    rad_amb = rad_amb + U.albedo * amb_term * wgt;
                    rad_emit = rad_emit + transmittance * fire_color(fl) * U.fire_k * dtw;
                    depth_acc = depth_acc + wgt * (t + 0.5 * dt) * wpi;
                    temp_acc = temp_acc + wgt * fl;
                    shadow_acc = shadow_acc + wgt * vis0;
                    weight_acc = weight_acc + wgt;
                    transmittance = transmittance * t_step;
                }
                t = t + dt;
            } else {
                let rel = (p - vec3<f32>(D_OFF)) / 8.0;
                let bmin = floor(rel) * 8.0 + vec3<f32>(D_OFF);
                let inv = 1.0 / rd;
                let ta = (bmin - U.cam) * inv;
                let tb = (bmin + 8.0 - U.cam) * inv;
                let texit = min(min(max(ta.x, tb.x), max(ta.y, tb.y)), max(ta.z, tb.z));
                let ahead = max(ceil((texit + 0.001 - t) / dt), 1.0);
                t = t + ahead * dt;
            }
        }
    }

    let pix = gid.y * U.w + gid.x;
    let alpha = 1.0 - transmittance;
    let inv_w = select(0.0, 1.0 / weight_acc, weight_acc > 0.0001);
    out_img[pix] = vec4<f32>(rad_sun + rad_amb + rad_emit, alpha);
    out_img[U.npix + pix] = vec4<f32>(rad_emit, depth_acc * inv_w);
    out_img[2u * U.npix + pix] = vec4<f32>(rad_sun, temp_acc * inv_w);
    out_img[3u * U.npix + pix] = vec4<f32>(rad_amb, shadow_acc * inv_w);
}
)";

    v.lightPipe = g.pipeline(lightSrc);
    v.renderPipe = g.pipeline(renderSrc);
    if (!v.lightPipe || !v.renderPipe) {
        g.releaseVol();
        _p->error = "shader compile failed";
        return false;
    }
    v.valid = true;
    return true;
}

// -------------------------------------------------------------------- draw
// Cheap per-frame half: light grid + ray march + readback against the last
// uploaded volume. Shaders still bake look/camera constants (uniform-buffer
// migration is a separate follow-up), but no recompression happens here.
bool Renderer::draw(const CameraParams& cam, const LookParams& look,
                    int W, int H, RenderOutput& out) {
    if (!valid()) return false;
    if (!_p->vol.valid) { _p->error = "draw() called before a successful upload()"; return false; }

    Impl& g = *_p;
    Uploaded& v = g.vol;
    const double* gmin = v.gmin;
    const double* gmax = v.gmax;
    const Mat34& i2w = v.i2w;
    const Mat34& w2i = v.w2i;

    int lg[3];
    for (int i = 0; i < 3; ++i)
        lg[i] = (int)((gmax[i] - gmin[i]) + look.lightFactor - 1) / look.lightFactor;

    // ---- camera basis in world space
    double camW[3], fwdW[3], rightW[3], upW[3];
    if (cam.valid) {
        const double cx = cos(cam.rx * M_PI / 180), sx = sin(cam.rx * M_PI / 180);
        const double cy = cos(cam.ry * M_PI / 180), sy = sin(cam.ry * M_PI / 180);
        const double cz = cos(cam.rz * M_PI / 180), sz = sin(cam.rz * M_PI / 180);
        // extrinsic XYZ: R = Rz * Ry * Rx
        double R[3][3] = {
            {cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz},
            {cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz},
            {-sy,     sx * cy,                cx * cy}};
        camW[0] = cam.tx; camW[1] = cam.ty; camW[2] = cam.tz;
        for (int i = 0; i < 3; ++i) {
            rightW[i] = R[i][0];
            upW[i] = R[i][1];
            fwdW[i] = -R[i][2];   // camera looks down local -Z
        }
    } else {
        // auto-frame: orbit position relative to the world-space bbox
        double c[3], ext = 0;
        for (int i = 0; i < 3; ++i) c[i] = (gmin[i] + gmax[i]) * 0.5;
        double cw[3];
        apply(i2w, c, cw, true);
        for (int i = 0; i < 3; ++i) {
            const double e = (gmax[i] - gmin[i]) * sqrt(i2w.m[0][i] * i2w.m[0][i]
                + i2w.m[1][i] * i2w.m[1][i] + i2w.m[2][i] * i2w.m[2][i]);
            ext = std::max(ext, e);
        }
        const double dir[3] = {0.5494, 0.0999, 0.7492};  // normalized (1.1,0.2,1.5)
        for (int i = 0; i < 3; ++i) {
            camW[i] = cw[i] + dir[i] * ext * 1.35;
            fwdW[i] = cw[i] - camW[i];
        }
        const double fl = sqrt(fwdW[0] * fwdW[0] + fwdW[1] * fwdW[1] + fwdW[2] * fwdW[2]);
        for (int i = 0; i < 3; ++i) fwdW[i] /= fl;
        rightW[0] = -fwdW[2]; rightW[1] = 0; rightW[2] = fwdW[0];
        const double rn = sqrt(rightW[0] * rightW[0] + rightW[2] * rightW[2]);
        rightW[0] /= rn; rightW[2] /= rn;
        upW[0] = rightW[1] * fwdW[2] - rightW[2] * fwdW[1];
        upW[1] = rightW[2] * fwdW[0] - rightW[0] * fwdW[2];
        upW[2] = rightW[0] * fwdW[1] - rightW[1] * fwdW[0];
    }

    // transform camera into index space (basis vectors: linear part only)
    double camI[3], fwdI[3], rightI[3], upI[3];
    apply(w2i, camW, camI, true);
    apply(w2i, fwdW, fwdI, false);
    apply(w2i, rightW, rightI, false);
    apply(w2i, upW, upI, false);

    // ---- light list, converted world -> index space (directions are vectors,
    // positions are points). A normalize lambda keeps it tidy.
    auto toIndexDir = [&](const float w[3], float out[3]) {
        double wv[3] = {w[0], w[1], w[2]};
        double n = sqrt(wv[0]*wv[0] + wv[1]*wv[1] + wv[2]*wv[2]);
        if (n < 1e-12) n = 1.0;
        double wn[3] = {wv[0]/n, wv[1]/n, wv[2]/n}, di[3];
        apply(w2i, wn, di, false);
        double ni = sqrt(di[0]*di[0] + di[1]*di[1] + di[2]*di[2]);
        if (ni < 1e-12) ni = 1.0;
        out[0] = (float)(di[0]/ni); out[1] = (float)(di[1]/ni); out[2] = (float)(di[2]/ni);
    };
    GpuLight gpuLights[MAX_LIGHTS] = {};
    int nL = look.numLights;
    if (nL < 0) {
        // fallback: one distant light from the legacy sun* fields (CLI/test_core).
        // nL == 0 falls through as "genuinely no lights" (unlit volume).
        gpuLights[0].type = (float)LIGHT_DISTANT;
        toIndexDir(look.sunDir, gpuLights[0].dir);
        for (int c = 0; c < 3; ++c) gpuLights[0].col[c] = look.sunColor[c] * look.sunIntensity;
        nL = 1;
    } else {
        if (nL > MAX_LIGHTS) nL = MAX_LIGHTS;
        for (int i = 0; i < nL; ++i) {
            const LightDesc& L = look.lights[i];
            GpuLight& G = gpuLights[i];
            G.type = (float)L.type;
            for (int c = 0; c < 3; ++c) G.col[c] = L.color[c];
            G.cosInner = L.cosInner; G.cosOuter = L.cosOuter;
            if (L.type == LIGHT_DISTANT) {
                toIndexDir(L.dir, G.dir);          // to-light direction
            } else {
                double wp[3] = {L.pos[0], L.pos[1], L.pos[2]}, pi[3];
                apply(w2i, wp, pi, true);          // centre: point transform
                G.pos[0] = (float)pi[0]; G.pos[1] = (float)pi[1]; G.pos[2] = (float)pi[2];
                if (L.type == LIGHT_SPOT) {
                    toIndexDir(L.dir, G.dir);      // shine axis
                } else if (L.type == LIGHT_AREA) {
                    toIndexDir(L.dir, G.dir);      // beam to-light direction (normal)
                    toIndexDir(L.uax, G.uax);      // beam width axis
                    toIndexDir(L.vax, G.vax);      // beam height axis
                    G.halfU = L.halfU; G.halfV = L.halfV;  // world half-sizes
                }
            }
        }
    }

    const double tanfov = (cam.valid ? cam.hAperture / (2.0 * cam.focal)
                                     : tan(28.0 * M_PI / 180.0));
    const double aspectV = cam.valid
        ? (cam.vAperture / (2.0 * cam.focal)) / tanfov : (double)H / W * 0 + 1.0;
    // vertical tan: cam.valid ? vA/(2f) : tanfov * H/W
    const double tanfovV = cam.valid ? cam.vAperture / (2.0 * cam.focal)
                                     : tanfov * H / (double)W;
    (void)aspectV;

    // ---- per-frame uniform: all look/camera/light/resolution values. No
    // shaders are built here — v.lightPipe/v.renderPipe were baked in upload().
    Uniforms uni{};
    for (int i = 0; i < 3; ++i) {
        uni.cam[i]     = (float)camI[i];
        uni.fwd[i]     = (float)fwdI[i];
        uni.right[i]   = (float)rightI[i];
        uni.up[i]      = (float)upI[i];
        uni.ambient[i] = look.ambient[i];
        uni.lg[i]      = lg[i];
    }
    uni.tanfov   = (float)tanfov;
    uni.tanfovv  = (float)tanfovV;
    uni.lfacf    = (float)look.lightFactor;
    uni.sigma    = look.sigma;
    uni.albedo   = look.albedo;
    uni.hg_g     = look.hgG;
    uni.fire_k   = look.fireK;
    uni.fire_max = look.fireMax;
    uni.numLights = nL;
    uni.fire_light = look.fireLight;
    uni.steps = look.steps;
    uni.w = (uint32_t)W;
    uni.h = (uint32_t)H;
    uni.npix = (uint32_t)(W * H);
    {   // world-space bbox diagonal^2 — reference for point/spot falloff
        double r2 = 0.0;
        for (int i = 0; i < 3; ++i) {
            const double colLen = sqrt(i2w.m[0][i]*i2w.m[0][i]
                + i2w.m[1][i]*i2w.m[1][i] + i2w.m[2][i]*i2w.m[2][i]);
            const double e = (gmax[i] - gmin[i]) * colLen;
            r2 += e * e;
        }
        uni.ref2 = (float)(r2 > 1e-12 ? r2 : 1.0);
    }
    for (int i = 0; i < nL; ++i) uni.lights[i] = gpuLights[i];
    WGPUBuffer uniBuf = g.ensureUniform(sizeof uni);
    wgpuQueueWriteBuffer(g.queue, uniBuf, 0, &uni, sizeof uni);

    // ---- per-draw GPU resources (brick buffers + pipelines are resident in v).
    // Light grid holds one transmittance slice per light + 1 fire-glow slice.
    const size_t lgN = (size_t)lg[0] * lg[1] * lg[2] * (size_t)(nL + 1);
    WGPUBuffer lgrid = g.storage(nullptr, lgN * 4);
    const size_t outBytes = (size_t)W * H * 16 * 4;  // 4 layers
    WGPUBuffer outBuf = g.storage(nullptr, outBytes, /*copySrc=*/true);
    WGPUBufferDescriptor rd2 = {};
    rd2.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    rd2.size = outBytes;
    WGPUBuffer readBuf = wgpuDeviceCreateBuffer(g.device, &rd2);

    auto entry = [](uint32_t b, WGPUBuffer buf) {
        WGPUBindGroupEntry e = {};
        e.binding = b;
        e.buffer = buf;
        e.size = WGPU_WHOLE_SIZE;
        return e;
    };
    WGPUBindGroupEntry le[8] = {entry(0, v.dTable), entry(1, v.dHead),
                                entry(2, v.dData), entry(3, lgrid), entry(4, uniBuf),
                                entry(5, v.fTable), entry(6, v.fHead), entry(7, v.fData)};
    WGPUBindGroupDescriptor lbd = {};
    lbd.layout = wgpuComputePipelineGetBindGroupLayout(v.lightPipe, 0);
    lbd.entryCount = 8;
    lbd.entries = le;
    WGPUBindGroup lightBG = wgpuDeviceCreateBindGroup(g.device, &lbd);

    WGPUBindGroupEntry re[9] = {entry(0, v.dTable), entry(1, v.dHead), entry(2, v.dData),
                                entry(3, v.fTable), entry(4, v.fHead), entry(5, v.fData),
                                entry(6, lgrid), entry(7, outBuf), entry(8, uniBuf)};
    WGPUBindGroupDescriptor rbd = {};
    rbd.layout = wgpuComputePipelineGetBindGroupLayout(v.renderPipe, 0);
    rbd.entryCount = 9;
    rbd.entries = re;
    WGPUBindGroup renderBG = wgpuDeviceCreateBindGroup(g.device, &rbd);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, v.lightPipe);
    wgpuComputePassEncoderSetBindGroup(pass, 0, lightBG, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (lg[0] + 3) / 4, (lg[1] + 3) / 4, (lg[2] + 3) / 4);
    wgpuComputePassEncoderSetPipeline(pass, v.renderPipe);
    wgpuComputePassEncoderSetBindGroup(pass, 0, renderBG, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (W + 7) / 8, (H + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(enc, outBuf, 0, readBuf, 0, outBytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g.queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    wgpuComputePassEncoderRelease(pass);

    bool mapped = false;
    WGPUBufferMapCallbackInfo mcb = {};
    mcb.mode = WGPUCallbackMode_AllowProcessEvents;
    mcb.callback = onMap;
    mcb.userdata1 = &mapped;
    wgpuBufferMapAsync(readBuf, WGPUMapMode_Read, 0, outBytes, mcb);
    while (!mapped) wgpuDevicePoll(g.device, true, nullptr);
    const float* px = (const float*)wgpuBufferGetConstMappedRange(readBuf, 0, outBytes);

    const size_t n = (size_t)W * H * 4;
    out.width = W;
    out.height = H;
    out.beauty.assign(px, px + n);
    out.emission.assign(px + n, px + 2 * n);
    out.sunScatter.assign(px + 2 * n, px + 3 * n);
    out.ambScatter.assign(px + 3 * n, px + 4 * n);
    wgpuBufferUnmap(readBuf);

    // release ONLY the per-draw resources; v.* buffers/pipelines + the uniform
    // buffer stay resident for the next draw().
    for (WGPUBuffer b : {lgrid, outBuf, readBuf})
        wgpuBufferRelease(b);
    wgpuBindGroupRelease(lightBG);
    wgpuBindGroupRelease(renderBG);
    return true;
}

// ------------------------------------------------------------------ render
// Convenience: full uncached path (upload then draw). Used by the CLI and as
// the fallback when a caller doesn't manage the upload/draw split itself.
bool Renderer::render(const openvdb::FloatGrid::ConstPtr& density,
                      const openvdb::FloatGrid::ConstPtr& flames,
                      const CameraParams& cam, const LookParams& look,
                      const float* volWorldMatrix,
                      int W, int H, RenderOutput& out) {
    if (!upload(density, flames, volWorldMatrix)) return false;
    return draw(cam, look, W, H, out);
}

}  // namespace FastVolume
