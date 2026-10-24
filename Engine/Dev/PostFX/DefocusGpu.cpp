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

// Pass S, one thread per sample (procedural lens): its shape(s), ProceduralLens::shape in f32, and
// the value per channel group with axial colour (scatterWithCA). Field position from the target
// the sample is in (binary search over the targets' first samples).
const char* kShapeWgsl = R"WGSL(
struct Sample { kx: i32, ky: i32, r: f32, c: array<f32, 5> };
struct TInfo { fx0: f32, fy0: f32, fstep: f32, gs: f32, first: u32, count: u32, shapeOff: u32, p1: u32 };
struct LensU {
  sqX: f32, sqY: f32, swirl: f32, catEye: f32,
  catEyeStart: f32, cx0: f32, cy0: f32, invField: f32,
  caPx: f32, farOff: u32, farN: u32, nearOff: u32,
  nearN: u32, nCh: u32, nSamples: u32, nTargets: u32,
};
@group(0) @binding(0) var<storage, read_write> shapes: array<Shape>;
@group(0) @binding(1) var<storage, read> samples: array<Sample>;
@group(0) @binding(2) var<storage, read> tinfo: array<TInfo>;
@group(0) @binding(3) var<uniform> L: LensU;

const PI: f32 = 3.14159265358979;

fn make(r: f32, fx: f32, fy: f32, kx: i32, ky: i32, v: ptr<function, array<f32, 7>>) -> Shape {
  var sh: Shape;
  let rad = abs(r);
  var m0 = rad * L.sqX;
  var m1 = 0.0;
  var m2 = 0.0;
  var m3 = rad * L.sqY;
  let rho = sqrt(fx * fx + fy * fy);
  var c = 1.0;
  var s = 0.0;
  if (rho > 1e-9) { c = fx / rho; s = fy / rho; }
  let sgn = select(1.0, -1.0, r < 0.0);
  if (L.swirl != 0.0 && rho > 1e-6) {
    // stretch along the tangential direction: R(phi) diag(1, t) R(-phi)
    let t = clamp(1.0 + L.swirl * rho * rho * sgn, 0.25, 4.0);
    let a00 = c * c + t * s * s;
    let a01 = (1.0 - t) * c * s;
    let a11 = s * s + t * c * c;
    let n0 = a00 * m0 + a01 * m2;
    let n1 = a00 * m1 + a01 * m3;
    let n2 = a01 * m0 + a11 * m2;
    let n3 = a01 * m1 + a11 * m3;
    m0 = n0; m1 = n1; m2 = n2; m3 = n3;
  }
  if (r < 0.0) { m0 = -m0; m1 = -m1; m2 = -m2; m3 = -m3; }
  let det = m0 * m3 - m1 * m2;
  var flags = 0u;
  if (sqrt(abs(det)) < 0.1) {
    flags = 1u;
  } else {
    sh.inv[0] = m3 / det;
    sh.inv[1] = -m1 / det;
    sh.inv[2] = -m2 / det;
    sh.inv[3] = m0 / det;
  }
  sh.ext = sqrt(m2 * m2 + m3 * m3);
  sh.energy = 1.0;
  if (L.catEye > 0.0 && rho > L.catEyeStart) {
    let o = min(1.9, L.catEye * (rho - L.catEyeStart) / max(1e-6, 1.0 - L.catEyeStart));
    if (o > 1e-4) {
      flags = flags | (1u << 1u);
      sh.clip[0] = o * c;
      sh.clip[1] = o * s;
      sh.clip[2] = 1.0;
      // two unit discs offset by o: the light kept
      sh.energy = (2.0 * acos(o / 2.0) - (o / 2.0) * sqrt(4.0 - o * o)) / PI;
    }
  }
  if (r < 0.0) {
    sh.ringOff = L.nearOff;
    sh.nRings = L.nearN;
  } else {
    sh.ringOff = L.farOff;
    sh.nRings = L.farN;
  }
  sh.bx = f32(kx);
  sh.by = f32(ky);
  sh.kx = kx;
  sh.flags = flags;
  for (var i = 0u; i < 7u; i++) { sh.v[i] = (*v)[i]; }
  return sh;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nw: vec3<u32>) {
  let i = gid.x + gid.y * nw.x * 64u;
  if (i >= L.nSamples) { return; }
  var lo = 0u;
  var hi = L.nTargets;
  while (lo + 1u < hi) {
    let mid = (lo + hi) / 2u;
    if (tinfo[mid].first <= i) { lo = mid; } else { hi = mid; }
  }
  let T = tinfo[lo];
  let smp = samples[i];
  let fx = (T.fx0 + f32(smp.kx) * T.fstep - L.cx0) * L.invField;
  let fy = (T.fy0 + f32(smp.ky) * T.fstep - L.cy0) * L.invField;
  let out = T.shapeOff + (i - T.first) * L.nCh;
  var v: array<f32, 7>;
  if (L.nCh == 1u) {
    for (var k = 0u; k < 5u; k++) { v[k] = smp.c[k]; }
    v[5] = smp.c[3];
    v[6] = smp.c[3];
    shapes[out] = make(smp.r / T.gs, fx, fy, smp.kx, smp.ky, &v);
    return;
  }
  // axial colour: red with its own radius and alpha, green with the rest, blue with its own
  for (var ch = 0u; ch < 3u; ch++) {
    for (var k = 0u; k < 7u; k++) { v[k] = 0.0; }
    v[ch] = smp.c[ch];
    if (ch == 0u) { v[5] = smp.c[3]; }
    if (ch == 1u) { v[3] = smp.c[3]; v[4] = smp.c[4]; }
    if (ch == 2u) { v[6] = smp.c[3]; }
    let rr = smp.r + L.caPx * (1.0 - f32(ch));
    shapes[out + ch] = make(rr / T.gs, fx, fy, smp.kx, smp.ky, &v);
  }
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
  let add = (P.flags & 1u) != 0u;   // a second target into the same buffer (cells after highlights)
  for (var i = li; i < tw * 7u; i += 64u) {
    let v = bitcast<f32>(atomicLoad(&cells[i]));
    if (add) { outb[rowBase + i] += v; } else { outb[rowBase + i] = v; }
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
    WGPUComputePipeline shapeGen = nullptr, area = nullptr, spans = nullptr, composite = nullptr;

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
        shapeGen = pipeline(common + kShapeWgsl);
        area = pipeline(common + kAreaWgsl + kClipWgsl);
        spans = pipeline(common + kSpanWgsl + kClipWgsl);
        composite = pipeline(common + kCompositeWgsl);
        wait();
        if ( !shapeGen || !area || !spans || !composite || !error.empty() ) {
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

    // ---- gather the targets: shapes (or samples, built into shapes on the GPU) and row starts
    // into one buffer each. Shape offsets count lens.nCh shapes per sample.
    const bool gen = job.lens.onGpu;
    const std::uint32_t nCh = gen ? (std::uint32_t)std::max(1, job.lens.nCh) : 1u;
    struct TargetRef { const Target* t; std::uint32_t shapeOff, rowOff; };
    struct TInfo { float fx0, fy0, fstep, gs; std::uint32_t first, count, shapeOff, p1; };
    std::vector<Shape> shapes;
    std::vector<Sample> samples;
    std::vector<TInfo> tinfo;
    std::vector<std::uint32_t> rows;
    std::vector<float> levelInside;
    std::vector<std::uint32_t> mlOff(job.slices.size(), 0);
    std::vector<TargetRef> refs;          // eTargetCount per slice
    std::uint64_t maxLevelCells = 1;
    std::uint64_t nShapes = 0;
    for (size_t si = 0; si < job.slices.size(); ++si) {
        const Slice& s = job.slices[si];
        for (int k = 0; k < eTargetCount; ++k) {
            const Target& t = s.t[k];
            TargetRef r = {&t, (std::uint32_t)nShapes, (std::uint32_t)rows.size()};
            if (gen) {
                if ( !t.samples.empty() ) {
                    TInfo ti = {t.fx0, t.fy0, t.fstep, t.gridScale, (std::uint32_t)samples.size(),
                                (std::uint32_t)t.samples.size(), (std::uint32_t)nShapes, 0};
                    tinfo.push_back(ti);
                    samples.insert( samples.end(), t.samples.begin(), t.samples.end() );
                }
                nShapes += (std::uint64_t)t.samples.size() * nCh;
                for (std::uint32_t v : t.rowStart) {
                    rows.push_back(v * nCh);
                }
            } else {
                shapes.insert( shapes.end(), t.shapes.begin(), t.shapes.end() );
                nShapes += t.shapes.size();
                rows.insert( rows.end(), t.rowStart.begin(), t.rowStart.end() );
            }
            refs.push_back(r);
        }
        mlOff[si] = (std::uint32_t)levelInside.size();
        levelInside.insert( levelInside.end(), s.lvlInside.begin(), s.lvlInside.end() );
        if (s.level > 0) {
            maxLevelCells = std::max<std::uint64_t>(maxLevelCells, (std::uint64_t)s.t[eTargetLevel].w * s.t[eTargetLevel].h);
        }
    }

    const std::uint64_t frameBytes = (std::uint64_t)job.ow * job.oh * kNumCh * sizeof(float);
    const std::uint64_t levelBytes = maxLevelCells * kNumCh * sizeof(float);
    const std::uint64_t shapeBytes = nShapes * sizeof(Shape);
    const std::uint64_t maxBind = g.limits.maxStorageBufferBindingSize;
    if (frameBytes > maxBind || shapeBytes > maxBind || levelBytes > maxBind) {
        err = "the frame needs larger GPU buffers than this GPU allows";
        return false;
    }

    Buffers B;
    WGPUBuffer dShapes = B.keep( storage(g, gen ? nullptr : shapes.data(), shapeBytes) );
    WGPUBuffer dSamples = B.keep( storage(g, samples.data(), samples.size() * sizeof(Sample)) );
    WGPUBuffer dTInfo = B.keep( storage(g, tinfo.data(), tinfo.size() * sizeof(TInfo)) );
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

    // the procedural lens for pass S (struct LensU in the WGSL)
    struct LensU {
        float sqX, sqY, swirl, catEye, catEyeStart, cx0, cy0, invField, caPx;
        std::uint32_t farOff, farN, nearOff, nearN, nCh, nSamples, nTargets;
    } lu = {job.lens.sqX, job.lens.sqY, job.lens.swirl, job.lens.catEye, job.lens.catEyeStart, job.lens.cx0,
            job.lens.cy0, job.lens.invField, job.lens.caPx, job.lens.farOff, job.lens.farN, job.lens.nearOff,
            job.lens.nearN, nCh, (std::uint32_t)samples.size(), (std::uint32_t)tinfo.size()};
    static_assert(sizeof(LensU) == 64, "LensU must match the WGSL struct");
    WGPUBufferDescriptor lud = {};
    lud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    lud.size = sizeof(LensU);
    WGPUBuffer dLens = B.keep( wgpuDeviceCreateBuffer(g.device, &lud) );
    wgpuQueueWriteBuffer(g.queue, dLens, 0, &lu, sizeof(LensU));

    // ---- per-dispatch parameters: pass A, then per slice eTargetCount x pass B + pass C
    const std::uint32_t nCons = (std::uint32_t)(job.constraints.size() / 8);
    const size_t perSlice = eTargetCount + 1;
    std::vector<Params> params;
    {
        Params p = {};
        p.nShapes = (std::uint32_t)nShapes;
        p.nCons = nCons;
        params.push_back(p);
    }
    for (size_t si = 0; si < job.slices.size(); ++si) {
        const Slice& s = job.slices[si];
        bool has[eTargetCount];
        for (int k = 0; k < eTargetCount; ++k) {
            const TargetRef& r = refs[si * eTargetCount + k];
            const Target& t = *r.t;
            has[k] = !t.empty() && t.w > 0 && t.h > 0;
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
            // level-0 cells add to the highlights already in the full-res buffer
            p.flags = (k == eTargetCells && has[eTargetHighlights]) ? 1u : 0u;
            params.push_back(p);
        }
        Params p = {};
        p.ow = job.ow;
        p.oh = job.oh;
        p.level = s.level;
        p.sc = 1 << s.level;
        p.lw = s.t[eTargetLevel].w;
        p.lh = s.t[eTargetLevel].h;
        p.ilo = s.ilo;
        p.jlo = s.jlo;
        p.oxD = job.oxD;
        p.oyD = job.oyD;
        p.flags = ( (has[eTargetHighlights] || has[eTargetCells]) ? eHasFull : 0u ) |
                  ( has[eTargetFills] ? eHasFullFill : 0u ) | ( has[eTargetLevel] ? eHasLvl : 0u ) |
                  ( has[eTargetLevelFills] ? eHasLvlFill : 0u );
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

    // ---- clear the accumulator; pass S (shapes from samples), pass A (areas)
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
    if (nShapes > 0) {
        WGPUBindGroup sg = nullptr;
        if (gen) {
            WGPUBindGroupEntry e[4] = {entry(0, dShapes), entry(1, dSamples), entry(2, dTInfo), entry(3, dLens)};
            sg = group(g.shapeGen, e, 4);
        }
        WGPUBindGroupEntry e[4] = {entry(0, dShapes), entry(1, dCons), entry(2, dRings), paramEntry(3, 0)};
        WGPUBindGroup bg = group(g.area, e, 4);
        submit([&](WGPUComputePassEncoder pass) {
            if (sg) {
                wgpuComputePassEncoderSetPipeline(pass, g.shapeGen);
                wgpuComputePassEncoderSetBindGroup(pass, 0, sg, 0, nullptr);
                dispatch1D( pass, samples.size() );
            }
            wgpuComputePassEncoderSetPipeline(pass, g.area);
            wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
            dispatch1D(pass, nShapes);
        }, false);
    }
    if ( !g.error.empty() ) {
        err = "GPU: " + g.error;
        return false;
    }
    tArea = since();

    // ---- slices back to front
    WGPUBuffer outs[eTargetCount] = {dFull, dFull, dFullF, dLvl, dLvlF};
    for (int si = (int)job.slices.size() - 1; si >= 0; --si) {
        if ( aborted && aborted() ) {
            err = "aborted";
            return false;
        }
        const size_t pBase = 1 + (size_t)si * perSlice;
        WGPUBindGroup spanGroups[eTargetCount];
        for (int k = 0; k < eTargetCount; ++k) {
            spanGroups[k] = nullptr;
            const Target& t = job.slices[si].t[k];
            if ( !t.empty() && t.w > 0 && t.h > 0 ) {
                WGPUBindGroupEntry e[6] = {entry(0, dShapes), entry(1, dCons), entry(2, dRings),
                                           paramEntry(3, pBase + k), entry(4, dRows), entry(5, outs[k])};
                spanGroups[k] = group(g.spans, e, 6);
            }
        }
        WGPUBindGroupEntry ce[8] = {entry(0, dFull), entry(1, dFullF), entry(2, dLvl), entry(3, dLvlF),
                                    entry(4, dInside), entry(5, dML), entry(6, dAcc), paramEntry(7, pBase + eTargetCount)};
        WGPUBindGroup compGroup = group(g.composite, ce, 8);
        const bool last = si == 0;
        submit([&](WGPUComputePassEncoder pass) {
            for (int k = 0; k < eTargetCount; ++k) {
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
        const std::uint64_t upBytes = gen ? samples.size() * sizeof(Sample) : shapeBytes;
        std::fprintf(stderr, "[DefocusGpu]   upload %.3f, shapes+area %.3f, slices %.3f, read back %.3f s "
                     "(%llu shapes%s, %.0f MB uploaded)\n", tUpload, tArea - tUpload, tSlices - tArea,
                     since() - tSlices, (unsigned long long)nShapes, gen ? " built on the GPU" : "",
                     (double)upBytes / (1024. * 1024.));
    }
    return true;
}

} // namespace gpu
} // namespace defocus

#endif // DEFOCUS_HAVE_WGPU
