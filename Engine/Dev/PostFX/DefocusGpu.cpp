/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#include "DefocusGpu.h"

#ifndef DEFOCUS_HAVE_WGPU

namespace defocus {
namespace gpu {

bool available(std::string* why)
{
    if (why) {
        *why = "this build has no GPU support (wgpu)";
    }
    return false;
}

bool scatter(const Job&, std::vector<float>&, std::string& err, const std::function<bool()>&)
{
    err = "this build has no GPU support (wgpu)";
    return false;
}

} // namespace gpu
} // namespace defocus

#else // DEFOCUS_HAVE_WGPU

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "webgpu.h"
#include "wgpu.h"

namespace defocus {
namespace gpu {

namespace {

const int kNumCh = 7;
const int kTile = 128;                  // output columns per pass-B thread

// Per dispatch, in a 256-byte slot of one uniform buffer. Matches `struct Params` in the WGSL.
struct Params
{
    std::int32_t w, h, nTiles, keyY0;
    std::int32_t nKeyRows, reachY, reachX;
    std::uint32_t shapeOff;
    std::uint32_t rowOff, outOff, nShapes, nCons;
    std::int32_t ow, oh, level, sc;
    std::int32_t lw, lh, ilo, jlo;
    std::int32_t oxD, oyD;
    std::uint32_t flags, mlOff;
};
static_assert(sizeof(Params) == 96, "Params must match the WGSL struct");
const std::uint64_t kParamStride = 256;

enum CompositeFlags : std::uint32_t
{
    eHasFull = 1u, eHasFullFill = 2u, eHasLvl = 4u, eHasLvlFill = 8u
};

const char* kCommonWgsl = R"WGSL(
struct Shape {
  bx: f32, by: f32, ext: f32, norm: f32,
  inv: array<f32, 4>,
  v: array<f32, 7>,
  kx: i32,
  clip: array<f32, 12>,
  energy: f32, flags: u32, ringOff: u32, nRings: u32,
};
struct Params {
  w: i32, h: i32, nTiles: i32, keyY0: i32,
  nKeyRows: i32, reachY: i32, reachX: i32, shapeOff: u32,
  rowOff: u32, outOff: u32, nShapes: u32, nCons: u32,
  ow: i32, oh: i32, level: i32, sc: i32,
  lw: i32, lh: i32, ilo: i32, jlo: i32,
  oxD: i32, oyD: i32, flags: u32, mlOff: u32,
};
)WGSL";

// clip_line and sub_rows: DefocusCore's clipLine and scatterSample's sub-row count, in f32.
const char* kClipWgsl = R"WGSL(
fn sub_rows(ext: f32) -> i32 {
  if (ext < 2.0) { return 16; }
  if (ext < 8.0) { return 8; }
  if (ext < 32.0) { return 4; }
  return 2;
}

// The part of the line u(t) = a t + b inside the aperture scaled by sc and the shape's clip discs:
// (1, t0, t1), or (0, 0, 0) when empty.
fn clip_line(sc: f32, sh: ptr<function, Shape>, ax: f32, ay: f32, bx: f32, by: f32) -> vec3<f32> {
  var t0 = -3.0e38;
  var t1 = 3.0e38;
  let aa = ax * ax + ay * ay;
  for (var i = 0u; i < P.nCons; i++) {
    let a = cons[2u * i];
    let b = cons[2u * i + 1u];
    if (a.w > 0.5) {
      let ox = bx - sc * b.x;
      let oy = by - sc * b.y;
      let B = 2.0 * (ax * ox + ay * oy);
      let C = ox * ox + oy * oy - sc * sc * b.z * b.z;
      let disc = B * B - 4.0 * aa * C;
      if (disc <= 0.0) { return vec3<f32>(0.0); }
      let sq = sqrt(disc);
      t0 = max(t0, (-B - sq) / (2.0 * aa));
      t1 = min(t1, (-B + sq) / (2.0 * aa));
    } else {
      let kk = a.x * ax + a.y * ay;
      let mm = sc * a.z - (a.x * bx + a.y * by);
      if (abs(kk) < 1e-12) {
        if (mm < 0.0) { return vec3<f32>(0.0); }
      } else if (kk > 0.0) {
        t1 = min(t1, mm / kk);
      } else {
        t0 = max(t0, mm / kk);
      }
    }
    if (t0 >= t1) { return vec3<f32>(0.0); }
  }
  let nClip = ((*sh).flags >> 1u) & 7u;
  for (var i = 0u; i < nClip; i++) {
    let ox = bx - (*sh).clip[3u * i];
    let oy = by - (*sh).clip[3u * i + 1u];
    let r = (*sh).clip[3u * i + 2u];
    let B = 2.0 * (ax * ox + ay * oy);
    let C = ox * ox + oy * oy - r * r;
    let disc = B * B - 4.0 * aa * C;
    if (disc <= 0.0) { return vec3<f32>(0.0); }
    let sq = sqrt(disc);
    t0 = max(t0, (-B - sq) / (2.0 * aa));
    t1 = min(t1, (-B + sq) / (2.0 * aa));
  }
  if (t0 < t1) { return vec3<f32>(1.0, t0, t1); }
  return vec3<f32>(0.0);
}
)WGSL";

// Pass A, one thread per shape: the kernel's area over all its sub-rows and rings -> norm.
const char* kAreaWgsl = R"WGSL(
@group(0) @binding(0) var<storage, read_write> shapes: array<Shape>;
@group(0) @binding(1) var<storage, read> cons: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> rings: array<vec2<f32>>;
@group(0) @binding(3) var<uniform> P: Params;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nw: vec3<u32>) {
  let i = gid.x + gid.y * nw.x * 64u;
  if (i >= P.nShapes) { return; }
  var sh = shapes[i];
  if ((sh.flags & 1u) != 0u) {
    shapes[i].norm = sh.energy;
    return;
  }
  let ext = sh.ext;
  let ns = sub_rows(ext);
  let fns = f32(ns);
  let yLo = i32(floor(sh.by - ext + 0.5));
  let yHi = i32(floor(sh.by + ext + 0.5));
  var area = 0.0;
  for (var y = yLo; y <= yHi; y++) {
    for (var j = 0; j < ns; j++) {
      let yy = (f32(y) - 0.5 + (f32(j) + 0.5) / fns) - sh.by;
      if (yy < -ext || yy > ext) { continue; }
      let bxu = yy * sh.inv[1];
      let byu = yy * sh.inv[3];
      for (var r = 0u; r < sh.nRings; r++) {
        let rg = rings[sh.ringOff + r];
        let t = clip_line(rg.x, &sh, sh.inv[0], sh.inv[2], bxu, byu);
        if (t.x > 0.5) { area += rg.y / fns * (t.z - t.y); }
      }
    }
  }
  if (area <= 1e-9) {
    shapes[i].flags = sh.flags | 1u;   // too small to draw: a point (DefocusCore does the same)
    shapes[i].norm = sh.energy;
  } else {
    shapes[i].norm = sh.energy / area;
  }
}
)WGSL";

// Pass B, one 64-thread workgroup per (row, 128-column tile) of a target. The shapes that can reach
// the row (per sample row: a binary search for the x range) are shared out among the threads; each
// adds its sub-row spans to the tile's difference array in workgroup memory (SpanBuffer::span;
// float adds as compare-and-swap loops: WGSL has no float atomics), then the tile is prefix-summed
// and written out. The workgroup owns its cells: nothing else writes them.
const char* kSpanWgsl = R"WGSL(
@group(0) @binding(0) var<storage, read> shapes: array<Shape>;
@group(0) @binding(1) var<storage, read> cons: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> rings: array<vec2<f32>>;
@group(0) @binding(3) var<uniform> P: Params;
@group(0) @binding(4) var<storage, read> rowStart: array<u32>;
@group(0) @binding(5) var<storage, read_write> outb: array<f32>;

const CHUNK: u32 = 256u;               // sample rows per round
var<workgroup> cells: array<atomic<u32>, 903>;      // (128 + 1) x 7
var<workgroup> rowLo: array<u32, 256>;
var<workgroup> rowFirst: array<u32, 257>;           // prefix of the per-row candidate counts
var<workgroup> total: u32;

var<private> tx0: i32;
var<private> tx1: i32;

fn add_cell(i: u32, x: f32) {
  var old = atomicLoad(&cells[i]);
  loop {
    let r = atomicCompareExchangeWeak(&cells[i], old, bitcast<u32>(bitcast<f32>(old) + x));
    if (r.exchanged) { break; }
    old = r.old_value;
  }
}

fn add_range(i0: i32, i1: i32, v: ptr<function, array<f32, 7>>, s: f32) {
  let a = max(i0, tx0);
  let b = min(i1, tx1);
  if (a > b) { return; }
  let ia = u32(a - tx0) * 7u;
  for (var c = 0u; c < 7u; c++) {
    let x = (*v)[c] * s;
    if (x != 0.0) { add_cell(ia + c, x); }
  }
  if (b + 1 <= tx1) {
    let ib = u32(b + 1 - tx0) * 7u;
    for (var c = 0u; c < 7u; c++) {
      let x = (*v)[c] * s;
      if (x != 0.0) { add_cell(ib + c, -x); }
    }
  }
}

fn span(X0: f32, X1: f32, v: ptr<function, array<f32, 7>>, s: f32) {
  let w = P.w;
  if (X1 <= -0.5 || X0 >= f32(w) - 0.5 || X1 <= X0) { return; }
  let c0 = i32(floor(X0 + 0.5));
  let c1 = i32(floor(X1 + 0.5));
  if (c0 == c1) {
    if (c0 >= 0 && c0 < w) { add_range(c0, c0, v, s * (X1 - X0)); }
  } else {
    if (c0 >= 0) { add_range(c0, c0, v, s * (f32(c0) + 0.5 - X0)); }
    if (c1 < w) { add_range(c1, c1, v, s * (X1 - (f32(c1) - 0.5))); }
    let m0 = max(0, c0 + 1);
    let m1 = min(w - 1, c1 - 1);
    if (m0 <= m1) { add_range(m0, m1, v, s); }
  }
}

fn point(cx: i32, v: ptr<function, array<f32, 7>>, s: f32) {
  if (cx < 0 || cx >= P.w) { return; }
  add_range(cx, cx, v, s);
}

fn draw(k: u32, y: i32) {
  // the row test first, from three fields, before loading the whole shape
  let by = shapes[k].by;
  let ext = shapes[k].ext;
  let flags = shapes[k].flags;
  if ((flags & 1u) != 0u) {
    if (abs(f32(y) - by) > 1.5) { return; }
  } else if (y < i32(floor(by - ext + 0.5)) || y > i32(floor(by + ext + 0.5))) {
    return;
  }
  var sh = shapes[k];
  var v: array<f32, 7>;
  for (var c = 0u; c < 7u; c++) { v[c] = sh.v[c]; }
  if ((flags & 1u) != 0u) {
    if ((flags & 16u) == 0u) {
      let cx = i32(floor(sh.bx + 0.5));
      let cy = i32(floor(sh.by + 0.5));
      if (cy == y) { point(cx, &v, sh.norm); }
    } else {
      // a sub-pixel kernel with a centre offset: split bilinearly (DefocusCore's pointSample)
      let x0 = i32(floor(sh.bx));
      let y0 = i32(floor(sh.by));
      let fx = sh.bx - f32(x0);
      let fy = sh.by - f32(y0);
      for (var j = 0; j < 2; j++) {
        if (y0 + j == y) {
          let wy = select(1.0 - fy, fy, j == 1);
          point(x0, &v, sh.norm * wy * (1.0 - fx));
          point(x0 + 1, &v, sh.norm * wy * fx);
        }
      }
    }
    return;
  }
  let ns = sub_rows(ext);
  let fns = f32(ns);
  for (var j = 0; j < ns; j++) {
    let yy = (f32(y) - 0.5 + (f32(j) + 0.5) / fns) - sh.by;
    if (yy < -ext || yy > ext) { continue; }
    let bxu = yy * sh.inv[1];
    let byu = yy * sh.inv[3];
    for (var r = 0u; r < sh.nRings; r++) {
      let rg = rings[sh.ringOff + r];
      let t = clip_line(rg.x, &sh, sh.inv[0], sh.inv[2], bxu, byu);
      if (t.x > 0.5) { span(sh.bx + t.y, sh.bx + t.z, &v, rg.y / fns * sh.norm); }
    }
  }
}

@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg: vec3<u32>, @builtin(num_workgroups) nw: vec3<u32>,
        @builtin(local_invocation_index) li: u32) {
  let wid = wg.x + wg.y * nw.x;
  let tile = i32(wid % u32(P.nTiles));
  let y = i32(wid / u32(P.nTiles));
  if (y >= P.h) { return; }   // the whole workgroup: uniform
  tx0 = tile * 128;
  tx1 = min(P.w - 1, tx0 + 127);
  let tw = u32(tx1 - tx0 + 1);
  for (var i = li; i < 903u; i += 64u) { atomicStore(&cells[i], 0u); }

  let kyLo = max(y - P.reachY, P.keyY0);
  let kyHi = min(y + P.reachY, P.keyY0 + P.nKeyRows - 1);
  let xMin = tx0 - P.reachX;
  let xMax = tx1 + P.reachX;
  for (var ky0 = kyLo; ky0 <= kyHi; ky0 += i32(CHUNK)) {
    let nRows = u32(min(kyHi - ky0 + 1, i32(CHUNK)));
    // each thread: the candidate range of some sample rows
    for (var r = li; r < nRows; r += 64u) {
      let kr = u32(ky0 - P.keyY0) + r;
      let s0 = rowStart[P.rowOff + kr];
      let s1 = rowStart[P.rowOff + kr + 1u];
      var lo = s0;
      var hi = s1;
      while (lo < hi) {
        let mid = (lo + hi) / 2u;
        if (shapes[P.shapeOff + mid].kx < xMin) { lo = mid + 1u; } else { hi = mid; }
      }
      var e = lo;
      hi = s1;
      while (e < hi) {
        let mid = (e + hi) / 2u;
        if (shapes[P.shapeOff + mid].kx <= xMax) { e = mid + 1u; } else { hi = mid; }
      }
      rowLo[r] = lo;
      rowFirst[r + 1u] = e - lo;
    }
    workgroupBarrier();
    if (li == 0u) {
      rowFirst[0] = 0u;
      for (var r = 0u; r < nRows; r++) { rowFirst[r + 1u] += rowFirst[r]; }
      total = rowFirst[nRows];
    }
    let n = workgroupUniformLoad(&total);
    // the candidates, shared out: thread li takes li, li + 64, ...
    for (var i = li; i < n; i += 64u) {
      var lo = 0u;
      var hi = nRows;
      while (lo + 1u < hi) {
        let mid = (lo + hi) / 2u;
        if (rowFirst[mid] <= i) { lo = mid; } else { hi = mid; }
      }
      draw(P.shapeOff + rowLo[lo] + (i - rowFirst[lo]), y);
    }
    workgroupBarrier();
  }

  // resolve: one thread per channel prefix-sums the tile, then everyone writes it out
  if (li < 7u) {
    var acc = 0.0;
    for (var x = 0u; x < tw; x++) {
      acc += bitcast<f32>(atomicLoad(&cells[x * 7u + li]));
      atomicStore(&cells[x * 7u + li], bitcast<u32>(acc));
    }
  }
  workgroupBarrier();
  let rowBase = P.outOff + (u32(y) * u32(P.w) + u32(tx0)) * 7u;
  for (var i = li; i < tw * 7u; i += 64u) {
    outb[rowBase + i] = bitcast<f32>(atomicLoad(&cells[i]));
  }
}
)WGSL";

// Pass C, one thread per output pixel: the slice = full-res buffer + fills x silhouette + the level
// buffer (cells + fills x level silhouette) upsampled bilinearly; composited over the accumulator
// per channel group (red with its alpha, blue with its, the rest with alpha), coverage above 1
// renormalised. DefocusCore's per-strip loop, whole frame at once.
const char* kCompositeWgsl = R"WGSL(
@group(0) @binding(0) var<storage, read> full: array<f32>;
@group(0) @binding(1) var<storage, read> fullF: array<f32>;
@group(0) @binding(2) var<storage, read> lvl: array<f32>;
@group(0) @binding(3) var<storage, read> lvlF: array<f32>;
@group(0) @binding(4) var<storage, read> inside: array<f32>;
@group(0) @binding(5) var<storage, read> mL: array<f32>;
@group(0) @binding(6) var<storage, read_write> accb: array<f32>;
@group(0) @binding(7) var<uniform> P: Params;

fn level_at(i: i32, j: i32, c: u32) -> f32 {
  if (i < 0 || i >= P.lw || j < 0 || j >= P.lh) { return 0.0; }
  let q = u32(j * P.lw + i);
  var r = 0.0;
  if ((P.flags & 4u) != 0u) { r += lvl[q * 7u + c]; }
  if ((P.flags & 8u) != 0u) { r += lvlF[q * 7u + c] * mL[P.mlOff + q]; }
  return r;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let x = i32(gid.x);
  let y = i32(gid.y);
  if (x >= P.ow || y >= P.oh) { return; }
  let p = u32(y * P.ow + x);
  var v: array<f32, 7>;
  for (var c = 0u; c < 7u; c++) {
    var s = 0.0;
    if ((P.flags & 1u) != 0u) { s += full[p * 7u + c]; }
    if ((P.flags & 2u) != 0u) { s += fullF[p * 7u + c] * inside[p]; }
    v[c] = s;
  }
  if (P.level > 0 && (P.flags & 12u) != 0u) {
    let sc = f32(P.sc);
    let u = (f32(x + P.oxD) + 0.5) / sc - 0.5 - f32(P.ilo);
    let vv = (f32(y + P.oyD) + 0.5) / sc - 0.5 - f32(P.jlo);
    var i0 = i32(floor(u));
    let fx = u - f32(i0);
    i0 = max(0, min(P.lw - 2, i0));
    var j0 = i32(floor(vv));
    let fy = vv - f32(j0);
    j0 = max(0, min(P.lh - 2, j0));
    for (var c = 0u; c < 7u; c++) {
      let a0 = level_at(i0, j0, c);
      let a0n = level_at(i0 + 1, j0, c);
      let a1 = level_at(i0, j0 + 1, c);
      let a1n = level_at(i0 + 1, j0 + 1, c);
      let top = a0 + (a0n - a0) * fx;
      let bot = a1 + (a1n - a1) * fx;
      v[c] += top + (bot - top) * fy;
    }
  }
  let o = p * 7u;
  // groups: (0, 5) by 5, (2, 6) by 6, (1, 3, 4) by 3
  for (var g = 0; g < 3; g++) {
    var cov = 3u;
    var c0 = 1u; var c1 = 3u; var c2 = 4u; var n = 3;
    if (g == 0) { cov = 5u; c0 = 0u; c1 = 5u; n = 2; }
    if (g == 1) { cov = 6u; c0 = 2u; c1 = 6u; n = 2; }
    var a = v[cov];
    var k = 1.0;
    if (a > 1.0) { k = 1.0 / a; a = 1.0; }
    let t = 1.0 - max(0.0, a);
    accb[o + c0] = v[c0] * k + accb[o + c0] * t;
    accb[o + c1] = v[c1] * k + accb[o + c1] * t;
    if (n == 3) { accb[o + c2] = v[c2] * k + accb[o + c2] * t; }
  }
}
)WGSL";

WGPUStringView sv(const char* s)
{
    WGPUStringView v;
    v.data = s;
    v.length = std::strlen(s);
    return v;
}

void onAdapter(WGPURequestAdapterStatus, WGPUAdapter a, WGPUStringView, void* u, void*)
{
    *(WGPUAdapter*)u = a;
}

void onDevice(WGPURequestDeviceStatus, WGPUDevice d, WGPUStringView, void* u, void*)
{
    *(WGPUDevice*)u = d;
}

void onMap(WGPUMapAsyncStatus, WGPUStringView, void* u, void*)
{
    *(bool*)u = true;
}

// One device for every defocus render in the process, created on first use; renders take turns.
struct Context
{
    std::mutex lock;
    bool tried = false;
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPULimits limits = {};
    std::string adapterName;
    std::string initError;
    std::string error;                  // validation / out-of-memory errors since the last check
    WGPUComputePipeline area = nullptr, spans = nullptr, composite = nullptr;

    static void onError(WGPUDevice const*, WGPUErrorType, WGPUStringView message, void* u, void*)
    {
        Context* c = (Context*)u;
        if ( c->error.empty() ) {
            c->error.assign(message.data, message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length);
        }
    }

    WGPUComputePipeline pipeline(const std::string& code)
    {
        WGPUShaderSourceWGSL src = {};
        src.chain.sType = WGPUSType_ShaderSourceWGSL;
        src.code = sv( code.c_str() );
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

    void wait()
    {
        wgpuDevicePoll(device, true, nullptr);
    }

    bool init()
    {
        tried = true;
        instance = wgpuCreateInstance(nullptr);
        if (!instance) {
            initError = "wgpuCreateInstance failed";
            return false;
        }
        WGPURequestAdapterOptions opts = {};
        opts.powerPreference = WGPUPowerPreference_HighPerformance;
        WGPURequestAdapterCallbackInfo acb = {};
        acb.mode = WGPUCallbackMode_AllowProcessEvents;
        acb.callback = onAdapter;
        acb.userdata1 = &adapter;
        wgpuInstanceRequestAdapter(instance, &opts, acb);
        for (int i = 0; i < 10000 && !adapter; ++i) {
            wgpuInstanceProcessEvents(instance);
        }
        if (!adapter) {
            initError = "no GPU adapter";
            return false;
        }
        // Ask for what the adapter can do: a 4K frame's buffers exceed the default 128 MB bindings.
        WGPULimits supported = {};
        wgpuAdapterGetLimits(adapter, &supported);
        WGPUDeviceDescriptor dd = {};
        dd.requiredLimits = &supported;
        dd.uncapturedErrorCallbackInfo.callback = onError;
        dd.uncapturedErrorCallbackInfo.userdata1 = this;
        WGPURequestDeviceCallbackInfo dcb = {};
        dcb.mode = WGPUCallbackMode_AllowProcessEvents;
        dcb.callback = onDevice;
        dcb.userdata1 = &device;
        wgpuAdapterRequestDevice(adapter, &dd, dcb);
        for (int i = 0; i < 10000 && !device; ++i) {
            wgpuInstanceProcessEvents(instance);
        }
        if (!device) {
            initError = "GPU device request failed";
            return false;
        }
        queue = wgpuDeviceGetQueue(device);
        wgpuDeviceGetLimits(device, &limits);
        WGPUAdapterInfo info = {};
        wgpuAdapterGetInfo(adapter, &info);
        adapterName.assign(info.device.data,
                           info.device.length == WGPU_STRLEN ? std::strlen(info.device.data) : info.device.length);

        const std::string common = std::string(kCommonWgsl);
        area = pipeline(common + kAreaWgsl + kClipWgsl);
        spans = pipeline(common + kSpanWgsl + kClipWgsl);
        composite = pipeline(common + kCompositeWgsl);
        wait();
        if ( !area || !spans || !composite || !error.empty() ) {
            initError = "GPU shaders failed: " + error;
            error.clear();
            device = nullptr;
            return false;
        }
        return true;
    }
};

Context& context()
{
    static Context c;
    return c;
}

// Storage buffer (at least 16 bytes: zero-size bindings aren't allowed), with optional contents.
WGPUBuffer storage(Context& g, const void* data, std::uint64_t size, WGPUBufferUsage extra = 0)
{
    WGPUBufferDescriptor bd = {};
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | extra;
    bd.size = std::max<std::uint64_t>(16, (size + 3) & ~std::uint64_t(3));
    WGPUBuffer b = wgpuDeviceCreateBuffer(g.device, &bd);
    if (b && data && size) {
        wgpuQueueWriteBuffer(g.queue, b, 0, data, (size + 3) & ~std::uint64_t(3));
    }
    return b;
}

WGPUBindGroupEntry entry(std::uint32_t binding, WGPUBuffer b, std::uint64_t offset = 0,
                         std::uint64_t size = WGPU_WHOLE_SIZE)
{
    WGPUBindGroupEntry e = {};
    e.binding = binding;
    e.buffer = b;
    e.offset = offset;
    e.size = size;
    return e;
}

// Everything one job uploads, released together.
struct Buffers
{
    std::vector<WGPUBuffer> owned;
    std::vector<WGPUBindGroup> groups;

    WGPUBuffer keep(WGPUBuffer b)
    {
        owned.push_back(b);
        return b;
    }

    ~Buffers()
    {
        for (WGPUBindGroup g : groups) {
            wgpuBindGroupRelease(g);
        }
        for (WGPUBuffer b : owned) {
            if (b) {
                wgpuBufferRelease(b);
            }
        }
    }
};

// 1D thread count as a 2D dispatch of 64-wide workgroups (the per-dimension limit is 65535).
void dispatch1D(WGPUComputePassEncoder pass, std::uint64_t threads)
{
    const std::uint64_t groups = (threads + 63) / 64;
    const std::uint32_t gx = (std::uint32_t)std::min<std::uint64_t>(groups, 65535);
    const std::uint32_t gy = (std::uint32_t)((groups + gx - 1) / gx);
    if (gx > 0) {
        wgpuComputePassEncoderDispatchWorkgroups(pass, gx, gy, 1);
    }
}

// n workgroups as a 2D dispatch (the per-dimension limit is 65535); the shader skips the extra ones.
void dispatchGroups(WGPUComputePassEncoder pass, std::uint64_t n)
{
    const std::uint32_t gx = (std::uint32_t)std::min<std::uint64_t>(n, 65535);
    const std::uint32_t gy = (std::uint32_t)((n + gx - 1) / gx);
    if (gx > 0) {
        wgpuComputePassEncoderDispatchWorkgroups(pass, gx, gy, 1);
    }
}

} // namespace

bool available(std::string* why)
{
    Context& g = context();
    std::lock_guard<std::mutex> l(g.lock);
    if (!g.tried) {
        g.init();
    }
    if (why) {
        *why = g.device ? g.adapterName : g.initError;
    }
    return g.device != nullptr;
}

bool scatter(const Job& job, std::vector<float>& acc, std::string& err,
             const std::function<bool()>& aborted)
{
    Context& g = context();
    std::lock_guard<std::mutex> l(g.lock);
    if (!g.tried) {
        g.init();
    }
    if (!g.device) {
        err = g.initError;
        return false;
    }
    g.error.clear();
    if (job.ow <= 0 || job.oh <= 0) {
        err = "empty output";
        return false;
    }
    // DEFOCUS_GPU_TIMES=1: where the time goes, to stderr
    const bool times = std::getenv("DEFOCUS_GPU_TIMES") != nullptr;
    const auto tStart = std::chrono::steady_clock::now();
    auto since = [&tStart]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
    };
    double tUpload = 0., tArea = 0., tSlices = 0.;

    // ---- gather the targets: shapes and row starts into one buffer each
    struct TargetRef { const Target* t; std::uint32_t shapeOff, rowOff; };
    std::vector<Shape> shapes;
    std::vector<std::uint32_t> rows;
    std::vector<float> levelInside;
    std::vector<std::uint32_t> mlOff(job.slices.size(), 0);
    std::vector<TargetRef> refs;          // 4 per slice: full, fullFill, lvl, lvlFill
    std::uint64_t maxLevelCells = 1;
    for (size_t si = 0; si < job.slices.size(); ++si) {
        const Slice& s = job.slices[si];
        const Target* ts[4] = {&s.full, &s.fullFill, &s.lvl, &s.lvlFill};
        for (const Target* t : ts) {
            TargetRef r = {t, (std::uint32_t)shapes.size(), (std::uint32_t)rows.size()};
            shapes.insert( shapes.end(), t->shapes.begin(), t->shapes.end() );
            rows.insert( rows.end(), t->rowStart.begin(), t->rowStart.end() );
            refs.push_back(r);
        }
        mlOff[si] = (std::uint32_t)levelInside.size();
        levelInside.insert( levelInside.end(), s.lvlInside.begin(), s.lvlInside.end() );
        if (s.level > 0) {
            maxLevelCells = std::max<std::uint64_t>(maxLevelCells, (std::uint64_t)s.lvl.w * s.lvl.h);
        }
    }

    const std::uint64_t frameBytes = (std::uint64_t)job.ow * job.oh * kNumCh * sizeof(float);
    const std::uint64_t levelBytes = maxLevelCells * kNumCh * sizeof(float);
    const std::uint64_t shapeBytes = (std::uint64_t)shapes.size() * sizeof(Shape);
    const std::uint64_t maxBind = g.limits.maxStorageBufferBindingSize;
    if (frameBytes > maxBind || shapeBytes > maxBind || levelBytes > maxBind) {
        err = "the frame needs larger GPU buffers than this GPU allows";
        return false;
    }

    Buffers B;
    WGPUBuffer dShapes = B.keep( storage(g, shapes.data(), shapeBytes) );
    WGPUBuffer dRows = B.keep( storage(g, rows.data(), rows.size() * sizeof(std::uint32_t)) );
    WGPUBuffer dCons = B.keep( storage(g, job.constraints.data(), job.constraints.size() * sizeof(float)) );
    WGPUBuffer dRings = B.keep( storage(g, job.rings.data(), job.rings.size() * sizeof(float)) );
    WGPUBuffer dInside = B.keep( storage(g, job.inside.data(), job.inside.size() * sizeof(float)) );
    WGPUBuffer dML = B.keep( storage(g, levelInside.data(), levelInside.size() * sizeof(float)) );
    WGPUBuffer dFull = B.keep( storage(g, nullptr, frameBytes) );
    WGPUBuffer dFullF = B.keep( storage(g, nullptr, frameBytes) );
    WGPUBuffer dLvl = B.keep( storage(g, nullptr, levelBytes) );
    WGPUBuffer dLvlF = B.keep( storage(g, nullptr, levelBytes) );
    WGPUBuffer dAcc = B.keep( storage(g, nullptr, frameBytes, WGPUBufferUsage_CopySrc) );
    WGPUBufferDescriptor rd = {};
    rd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    rd.size = frameBytes;
    WGPUBuffer dRead = B.keep( wgpuDeviceCreateBuffer(g.device, &rd) );

    // ---- per-dispatch parameters: pass A, then per slice 4 x pass B + pass C
    const std::uint32_t nCons = (std::uint32_t)(job.constraints.size() / 8);
    std::vector<Params> params;
    {
        Params p = {};
        p.nShapes = (std::uint32_t)shapes.size();
        p.nCons = nCons;
        params.push_back(p);
    }
    for (size_t si = 0; si < job.slices.size(); ++si) {
        const Slice& s = job.slices[si];
        std::uint32_t flags = 0;
        for (int k = 0; k < 4; ++k) {
            const TargetRef& r = refs[si * 4 + k];
            const Target& t = *r.t;
            Params p = {};
            p.w = t.w;
            p.h = t.h;
            p.nTiles = std::max(1, (t.w + kTile - 1) / kTile);
            p.keyY0 = t.keyY0;
            p.nKeyRows = t.rowStart.empty() ? 0 : (int)t.rowStart.size() - 1;
            p.reachY = t.reachY;
            p.reachX = t.reachX;
            p.shapeOff = r.shapeOff;
            p.rowOff = r.rowOff;
            p.nCons = nCons;
            params.push_back(p);
            if (!t.empty() && t.w > 0 && t.h > 0) {
                flags |= (1u << k);
            }
        }
        Params p = {};
        p.ow = job.ow;
        p.oh = job.oh;
        p.level = s.level;
        p.sc = 1 << s.level;
        p.lw = s.lvl.w;
        p.lh = s.lvl.h;
        p.ilo = s.ilo;
        p.jlo = s.jlo;
        p.oxD = job.oxD;
        p.oyD = job.oyD;
        p.flags = flags;
        p.mlOff = mlOff[si];
        params.push_back(p);
    }
    std::vector<unsigned char> paramBytes(params.size() * kParamStride, 0);
    for (size_t i = 0; i < params.size(); ++i) {
        std::memcpy(&paramBytes[i * kParamStride], &params[i], sizeof(Params));
    }
    WGPUBufferDescriptor ud = {};
    ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ud.size = paramBytes.size();
    WGPUBuffer dParams = B.keep( wgpuDeviceCreateBuffer(g.device, &ud) );
    wgpuQueueWriteBuffer(g.queue, dParams, 0, paramBytes.data(), paramBytes.size());

    auto paramEntry = [&](std::uint32_t binding, size_t index) {
        return entry(binding, dParams, index * kParamStride, sizeof(Params));
    };
    auto group = [&](WGPUComputePipeline pipe, const WGPUBindGroupEntry* e, size_t n) {
        WGPUBindGroupDescriptor bd = {};
        bd.layout = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
        bd.entryCount = n;
        bd.entries = e;
        WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g.device, &bd);
        wgpuBindGroupLayoutRelease(bd.layout);
        B.groups.push_back(bg);
        return bg;
    };
    auto submit = [&](const std::function<void(WGPUComputePassEncoder)>& record, bool readBack) {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
        record(pass);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        if (readBack) {
            wgpuCommandEncoderCopyBufferToBuffer(enc, dAcc, 0, dRead, 0, frameBytes);
        }
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuQueueSubmit(g.queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);
        g.wait();   // one submission per slice keeps each under the OS GPU timeout
    };

    // ---- clear the accumulator, pass A
    g.wait();
    tUpload = since();
    {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        wgpuCommandEncoderClearBuffer(enc, dAcc, 0, frameBytes);
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuQueueSubmit(g.queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);
    }
    if ( !shapes.empty() ) {
        WGPUBindGroupEntry e[4] = {entry(0, dShapes), entry(1, dCons), entry(2, dRings), paramEntry(3, 0)};
        WGPUBindGroup bg = group(g.area, e, 4);
        submit([&](WGPUComputePassEncoder pass) {
            wgpuComputePassEncoderSetPipeline(pass, g.area);
            wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
            dispatch1D( pass, shapes.size() );
        }, false);
    }
    if ( !g.error.empty() ) {
        err = "GPU: " + g.error;
        return false;
    }
    tArea = since();

    // ---- slices back to front
    WGPUBuffer outs[4] = {dFull, dFullF, dLvl, dLvlF};
    for (int si = (int)job.slices.size() - 1; si >= 0; --si) {
        if ( aborted && aborted() ) {
            err = "aborted";
            return false;
        }
        const size_t pBase = 1 + (size_t)si * 5;
        const std::uint32_t flags = params[pBase + 4].flags;
        WGPUBindGroup spanGroups[4] = {nullptr, nullptr, nullptr, nullptr};
        for (int k = 0; k < 4; ++k) {
            if ( flags & (1u << k) ) {
                WGPUBindGroupEntry e[6] = {entry(0, dShapes), entry(1, dCons), entry(2, dRings),
                                           paramEntry(3, pBase + k), entry(4, dRows), entry(5, outs[k])};
                spanGroups[k] = group(g.spans, e, 6);
            }
        }
        WGPUBindGroupEntry ce[8] = {entry(0, dFull), entry(1, dFullF), entry(2, dLvl), entry(3, dLvlF),
                                    entry(4, dInside), entry(5, dML), entry(6, dAcc), paramEntry(7, pBase + 4)};
        WGPUBindGroup compGroup = group(g.composite, ce, 8);
        const bool last = si == 0;
        submit([&](WGPUComputePassEncoder pass) {
            for (int k = 0; k < 4; ++k) {
                if (!spanGroups[k]) {
                    continue;
                }
                const Params& p = params[pBase + k];
                wgpuComputePassEncoderSetPipeline(pass, g.spans);
                wgpuComputePassEncoderSetBindGroup(pass, 0, spanGroups[k], 0, nullptr);
                dispatchGroups(pass, (std::uint64_t)p.nTiles * p.h);
            }
            wgpuComputePassEncoderSetPipeline(pass, g.composite);
            wgpuComputePassEncoderSetBindGroup(pass, 0, compGroup, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(pass, (job.ow + 7) / 8, (job.oh + 7) / 8, 1);
        }, last);
        if ( !g.error.empty() ) {
            err = "GPU: " + g.error;
            return false;
        }
    }
    if ( job.slices.empty() ) {
        submit([](WGPUComputePassEncoder) {}, true);
    }

    tSlices = since();
    if (times) {
        for (size_t si = 0; si < job.slices.size() && si < 16; ++si) {
            const Slice& s = job.slices[si];
            std::fprintf(stderr, "[DefocusGpu]   slice %zu level %d:", si, s.level);
            const Target* ts[4] = {&s.full, &s.fullFill, &s.lvl, &s.lvlFill};
            const char* names[4] = {"full", "fill", "lvl", "lvlFill"};
            for (int k = 0; k < 4; ++k) {
                if (!ts[k]->empty()) {
                    std::fprintf(stderr, " %s %dx%d %zu shapes reach %d,%d;", names[k], ts[k]->w, ts[k]->h,
                                 ts[k]->shapes.size(), ts[k]->reachX, ts[k]->reachY);
                }
            }
            std::fprintf(stderr, "\n");
        }
    }

    // ---- read back
    bool mapped = false;
    WGPUBufferMapCallbackInfo mcb = {};
    mcb.mode = WGPUCallbackMode_AllowProcessEvents;
    mcb.callback = onMap;
    mcb.userdata1 = &mapped;
    wgpuBufferMapAsync(dRead, WGPUMapMode_Read, 0, frameBytes, mcb);
    for (int i = 0; !mapped && i < 1000000; ++i) {
        g.wait();
    }
    if (!mapped || !g.error.empty()) {
        err = "GPU: " + (g.error.empty() ? std::string("read back failed") : g.error);
        return false;
    }
    const float* px = (const float*)wgpuBufferGetConstMappedRange(dRead, 0, frameBytes);
    acc.assign(px, px + (size_t)job.ow * job.oh * kNumCh);
    wgpuBufferUnmap(dRead);
    if (times) {
        std::fprintf(stderr, "[DefocusGpu]   upload %.3f, area %.3f, slices %.3f, read back %.3f s (%zu shapes, %.0f MB)\n",
                     tUpload, tArea - tUpload, tSlices - tArea, since() - tSlices, shapes.size(),
                     (double)(shapeBytes + 5 * frameBytes) / (1024. * 1024.));
    }
    return true;
}

} // namespace gpu
} // namespace defocus

#endif // DEFOCUS_HAVE_WGPU
