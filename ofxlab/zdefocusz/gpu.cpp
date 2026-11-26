// ZDefocusZ: the GPU path, implemented — DECISIONS.md D-51, design notes in gpu.h.
//
// What this file is: the dlopen'd OpenCL loader, the one device this process renders on, the compiled
// program, and the methods that run extraction / convolution / composite on that device. It links nothing
// new but `-ldl` (part of libc on Linux), vendors nothing (rule 2), and is OFX-free like the rest of the
// core — which is how tests/gpu_parity.cpp drives the same methods the plugin drives, stage by stage, and
// tests/gpu_probe.sh compares the self-declared ABI in gpu.h against a real <CL/cl.h> when the machine has
// one.
//
// Failure is a string, never an exception, and never a render failure: every entry point here returns false
// with a reason, and the caller re-runs the band on the CPU (gpu.h: `renderBand`). A plugin whose checkbox
// cannot be honoured logs why and renders anyway.

#include "gpu.h"

#include <dlfcn.h>

#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <mutex>

namespace zd {
namespace gpu {

namespace {

// The ICD loader, tried in the order a host with GPU support has already opened it. `libOpenCL.so.1` is the
// SONAME of the Khronos ICD loader on every Linux distribution that ships one; the vendor ICDs
// (/etc/opencl.cfg, OCL_ICD_VENDORS) are enumerated *inside* it, so this file never has to know what GPU
// the host happens to have — which is the other half of why the route is dlopen rather than a link.
#ifdef _WIN32
const char *const kLibraryNames[] = { "OpenCL.dll", 0 };
#else
const char *const kLibraryNames[] = { "libOpenCL.so.1", "libOpenCL.so", 0 };
#endif

// The entry points, in the names the ICD exports, in the order of the Loader's members. Pairing them by
// order is what makes a missing one a reportable name instead of a null call.
const char *const kEntryNames[] = {
    "clGetPlatformIDs",        "clGetDeviceIDs",            "clGetDeviceInfo",           "clCreateContext",
    "clCreateCommandQueue",    "clCreateProgramWithSource", "clBuildProgram",            "clGetProgramBuildInfo",
    "clCreateKernel",          "clSetKernelArg",            "clEnqueueNDRangeKernel",    "clCreateBuffer",
    "clEnqueueWriteBuffer",    "clEnqueueReadBuffer",       "clReleaseMemObject",        "clReleaseKernel",
    "clReleaseProgram",        "clReleaseCommandQueue",     "clReleaseContext",          "clFlush",
    "clFinish",
};
const int kNumEntries = 21;

// A cl_int as the sentence a user can act on. The names are <CL/cl.h>'s, which is the list
// tests/gpu_probe.sh re-derives; a code outside it prints as a number rather than a guess.
const char *statusName(cl_int e)
{
  switch (e) {
  case 0: return "CL_SUCCESS";
  case -1: return "CL_DEVICE_NOT_FOUND";
  case -2: return "CL_DEVICE_NOT_AVAILABLE";
  case -3: return "CL_COMPILER_NOT_AVAILABLE";
  case -4: return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
  case -5: return "CL_OUT_OF_RESOURCES";
  case -6: return "CL_OUT_OF_HOST_MEMORY";
  case -11: return "CL_BUILD_PROGRAM_FAILURE";
  case -12: return "CL_MAP_FAILURE";
  case -30: return "CL_INVALID_VALUE";
  case -31: return "CL_INVALID_DEVICE_TYPE";
  case -32: return "CL_INVALID_PLATFORM";
  case -33: return "CL_INVALID_DEVICE";
  case -34: return "CL_INVALID_CONTEXT";
  case -36: return "CL_INVALID_COMMAND_QUEUE";
  case -37: return "CL_INVALID_HOST_PTR";
  case -38: return "CL_INVALID_MEM_OBJECT";
  case -44: return "CL_INVALID_PROGRAM";
  case -46: return "CL_INVALID_KERNEL_NAME";
  case -47: return "CL_INVALID_KERNEL_DEFINITION";
  case -48: return "CL_INVALID_KERNEL";
  case -50: return "CL_INVALID_ARG_VALUE";
  case -51: return "CL_INVALID_ARG_SIZE";
  case -52: return "CL_INVALID_KERNEL_ARGS";
  case -54: return "CL_INVALID_WORK_GROUP_SIZE";
  case -61: return "CL_INVALID_BUFFER_SIZE";
  case -63: return "CL_INVALID_GLOBAL_WORK_SIZE";
  default: return 0;
  }
}

std::string statusText(cl_int e)
{
  const char *n = statusName(e);
  if (n)
    return std::string(n);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "OpenCL error %d", (int)e);
  return std::string(buf);
}

/** @brief A byte count in the unit a device report is usually read in. Named the way it is divided: the
 * divisor is 1024 cubed, so a device with 16611827712 bytes of global memory is 15.47 GiB, not 16.61 GB, and
 * a message that says GB next to a GiB number is a message a user cannot compare with a spec sheet. */
std::string mebibytes(cl_ulong bytes)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f GiB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
  return std::string(buf);
}

// One string, for the host log and for a test to print: what the device is, and what it promised.
std::string describeDevice(const DeviceInfo &d)
{
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "compute units %llu, max work group %zu, global memory %s, max single allocation %s, local memory %lu B",
                (unsigned long long)d.maxComputeUnits, d.maxWorkGroupSize, mebibytes(d.globalMemory).c_str(),
                mebibytes(d.maxAllocation).c_str(), (unsigned long)d.localMemory);
  std::string s = d.name;
  s += " (";
  s += d.vendor;
  s += ", ";
  s += d.version;
  s += ", driver ";
  s += d.driver;
  s += ") — ";
  s += buf;
  return s;
}

// dlopen + dlsym, once per process. `why` says which library opened and which entry point it lacked: the
// two facts that tell a user whether the machine has no ICD at all or has one older than the 1.2 host ABI
// this renderer asks for.
bool loadLoader(Loader &out, std::string &why)
{
  void *h = 0;
  for (int i = 0; kLibraryNames[i]; ++i) {
    h = dlopen(kLibraryNames[i], RTLD_LAZY);
    if (h) {
      out.library = kLibraryNames[i];
      break;
    }
  }
  if (!h) {
    why = std::string("no OpenCL ICD loader to open (tried ") + kLibraryNames[0] + "): " + (dlerror() ? dlerror() : "no such library") +
          " — this machine has no GPU compute path, so the render stays on the CPU";
    return false;
  }

  /* Every entry point is read as a raw address first, so one loop can report which name the loader
     lacks, and only a complete set is written into the typed slots. */
  void *slots[kNumEntries];
  std::string missing;
  for (int i = 0; i < kNumEntries; ++i) {
    slots[i] = dlsym(h, kEntryNames[i]);
    if (!slots[i] && missing.empty())
      missing = kEntryNames[i];
  }
  if (!missing.empty()) {
    out.missing = missing;
    why = "the ICD loader " + out.library + " does not export " + missing +
          " — this renderer needs the OpenCL 1.2 host ABI, so the render stays on the CPU";
    return false;
  }
  out.getPlatformIDs = (FnGetPlatformIDs)slots[0];
  out.getDeviceIDs = (FnGetDeviceIDs)slots[1];
  out.getDeviceInfo = (FnGetDeviceInfo)slots[2];
  out.createContext = (FnCreateContext)slots[3];
  out.createCommandQueue = (FnCreateCommandQueue)slots[4];
  out.createProgramWithSource = (FnCreateProgramWithSource)slots[5];
  out.buildProgram = (FnBuildProgram)slots[6];
  out.getProgramBuildInfo = (FnGetProgramBuildInfo)slots[7];
  out.createKernel = (FnCreateKernel)slots[8];
  out.setKernelArg = (FnSetKernelArg)slots[9];
  out.enqueueNDRangeKernel = (FnEnqueueNDRangeKernel)slots[10];
  out.createBuffer = (FnCreateBuffer)slots[11];
  out.enqueueWriteBuffer = (FnEnqueueWriteBuffer)slots[12];
  out.enqueueReadBuffer = (FnEnqueueReadBuffer)slots[13];
  out.releaseMemObject = (FnReleaseMemObject)slots[14];
  out.releaseKernel = (FnReleaseKernel)slots[15];
  out.releaseProgram = (FnReleaseProgram)slots[16];
  out.releaseCommandQueue = (FnReleaseCommandQueue)slots[17];
  out.releaseContext = (FnReleaseContext)slots[18];
  out.flush = (FnFlush)slots[19];
  out.finish = (FnFinish)slots[20];
  return true;
}

// The process-wide state: one loader, one device, one context, one queue, one program. The mutex is the
// one deliberate exception to "no shared mutable state" (rule 3), and what it buys is stated in D-51: a
// host renders tiles and frames from several threads, a compiled program and a context are per-process
// objects that must not be built twice, and GPU renders therefore serialise across those threads while the
// CPU path does not. Nothing here is a render-threading API; it is an initialisation guard.
struct Cache {
  std::mutex mutex;     // guards the loader, the probe, the context and the program: who initialises them
  std::mutex bandMutex; // guards *using* them: one band's device buffers on the queue at a time (see Session)
  Loader loader;
  bool loaderDone;
  std::string why;

  DeviceInfo info;
  bool probed;

  cl_context ctx;
  cl_device_id dev;
  cl_platform_id platform; // the platform @c dev belongs to: clCreateContext wants that, not the device
  cl_command_queue queue;
  bool ctxDone;

  Program prog;
  bool progDone;

  Cache()
      : loaderDone(false), probed(false), ctx(0), dev(0), platform(0), queue(0), ctxDone(false), progDone(false)
  {
  }
};

Cache &cache()
{
  static Cache c;
  return c;
}

// A device info string, read whatever the vendor did or did not report.
std::string stringInfo(Loader &api, cl_device_id dev, cl_uint name)
{
  size_t n = 0;
  if (api.getDeviceInfo(dev, name, 0, 0, &n) != kCL_SUCCESS || n == 0)
    return std::string();
  std::vector<char> buf(n + 1, 0);
  if (api.getDeviceInfo(dev, name, n, &buf[0], 0) != kCL_SUCCESS)
    return std::string();
  return std::string(&buf[0]);
}

cl_ulong intInfo(Loader &api, cl_device_id dev, cl_uint name, cl_ulong fallback)
{
  cl_ulong v = 0;
  if (api.getDeviceInfo(dev, name, sizeof(v), &v, 0) != kCL_SUCCESS)
    return fallback;
  return v;
}

size_t sizeInfo(Loader &api, cl_device_id dev, cl_uint name)
{
  size_t v = 0;
  if (api.getDeviceInfo(dev, name, sizeof(v), &v, 0) != kCL_SUCCESS)
    return 0;
  return v;
}

// A query whose size in <CL/cl.h> is cl_uint, not cl_ulong: the parameter size has to be the size the
// driver expects or it answers with CL_INVALID_VALUE and the number is simply lost, which is how a probe
// ends up reporting a vector width of nothing.
cl_uint uintInfo(Loader &api, cl_device_id dev, cl_uint name, cl_uint fallback)
{
  cl_uint v = 0;
  if (api.getDeviceInfo(dev, name, sizeof(v), &v, 0) != kCL_SUCCESS)
    return fallback;
  return v;
}

// The probe: enumerate platforms, keep the *GPU* devices only (a CPU through the same ICD is the CPU path
// with extra transfers, which is never a win), require the device to say it is available and to do 64-bit
// floats (gpu.h's numerical promise is a double-accumulating kernel: a device without them would be a
// second precision regime, and therefore a second set of tolerances to test), and prefer the device with
// the largest single allocation — the ceiling LayeredRender's band splitting has to live under.
//
// Every rejection is written into `why`, because "the checkbox did nothing" is a question a user can only
// ask the log.
void probe(Cache &c)
{
  if (c.probed)
    return;
  c.probed = true;

  if (!c.loaderDone)
    c.loaderDone = loadLoader(c.loader, c.why);
  if (!c.loaderDone || !c.loader.complete())
    return;
  Loader &api = c.loader;

  cl_uint numPlatforms = 0;
  if (api.getPlatformIDs(0, 0, &numPlatforms) != kCL_SUCCESS || numPlatforms == 0) {
    c.why = "the ICD reports no OpenCL platform (no vendor ICD installed) — the render stays on the CPU";
    return;
  }
  std::vector<cl_platform_id> platforms(numPlatforms, 0);
  if (api.getPlatformIDs(numPlatforms, &platforms[0], 0) != kCL_SUCCESS) {
    c.why = "clGetPlatformIDs failed: " + statusText(api.getPlatformIDs(numPlatforms, &platforms[0], 0)) + " — CPU path";
    return;
  }

  cl_device_id best = 0;
  cl_platform_id bestPlatform = 0; // a device is only useful together with the platform that reported it
  cl_ulong bestAlloc = 0;
  DeviceInfo bestInfo;
  std::string rejected;
  bool sawGpu = false;

  for (cl_uint p = 0; p < numPlatforms; ++p) {
    cl_uint n = 0;
    if (api.getDeviceIDs(platforms[p], kCL_DEVICE_TYPE_GPU, 0, 0, &n) != kCL_SUCCESS || n == 0)
      continue;
    std::vector<cl_device_id> devs(n, 0);
    if (api.getDeviceIDs(platforms[p], kCL_DEVICE_TYPE_GPU, n, &devs[0], 0) != kCL_SUCCESS)
      continue;

    for (cl_uint i = 0; i < n; ++i) {
      sawGpu = true;
      DeviceInfo d;
      d.name = stringInfo(api, devs[i], kCL_DEVICE_NAME);
      d.vendor = stringInfo(api, devs[i], kCL_DEVICE_VENDOR);
      d.driver = stringInfo(api, devs[i], kCL_DRIVER_VERSION);
      d.profile = stringInfo(api, devs[i], kCL_DEVICE_PROFILE);
      d.version = stringInfo(api, devs[i], kCL_DEVICE_VERSION);
      d.maxComputeUnits = uintInfo(api, devs[i], kCL_DEVICE_MAX_COMPUTE_UNITS, 0); // cl_uint in <CL/cl.h>
      d.maxWorkGroupSize = sizeInfo(api, devs[i], kCL_DEVICE_MAX_WORK_GROUP_SIZE);
      {
        // A size_t per work dimension, and the number of dimensions is itself a query: asking for three
        // when the device reports two would read past what it wrote.
        const cl_uint dims = uintInfo(api, devs[i], kCL_DEVICE_MAX_WORK_ITEM_DIMENSIONS, 0);
        if (dims > 0) {
          std::vector<size_t> items((size_t)dims, 0u);
          if (api.getDeviceInfo(devs[i], kCL_DEVICE_MAX_WORK_ITEM_SIZES, items.size() * sizeof(size_t), &items[0], 0) ==
              kCL_SUCCESS) {
            for (cl_uint k = 0; k < dims && k < 3u; ++k)
              d.maxWorkItem[k] = items[k];
          }
        }
      }
      d.maxAllocation = intInfo(api, devs[i], kCL_DEVICE_MAX_MEM_ALLOC_SIZE, 0);
      d.globalMemory = intInfo(api, devs[i], kCL_DEVICE_GLOBAL_MEM_SIZE, 0);
      d.localMemory = intInfo(api, devs[i], kCL_DEVICE_LOCAL_MEM_SIZE, 0);
      d.vectorWidthFloat = uintInfo(api, devs[i], kCL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT, 1);
      const cl_uint avail = uintInfo(api, devs[i], kCL_DEVICE_AVAILABLE, 0);
      // cl_khr_fp64 reports itself as a bitfield of the 64-bit operations this device can do; zero means
      // none of them, which is the extension being absent (the extension's own name is what clGetExtension
      // FunctionList would say, but the bitfield is the answer the plugin needs).
      cl_device_fp_config fp = 0;
      api.getDeviceInfo(devs[i], kCL_DEVICE_DOUBLE_FP_CONFIG, sizeof(fp), &fp, 0);
      d.doubleSupport = (fp != 0);

      if (!avail) {
        rejected += "device \"" + (d.name.empty() ? std::string("(unnamed)") : d.name) + "\" is not available to this process; ";
        continue;
      }
      if (!d.doubleSupport) {
        rejected += "device \"" + (d.name.empty() ? std::string("(unnamed)") : d.name) + "\" reports no 64-bit floats; ";
        continue;
      }
      if (d.maxAllocation > bestAlloc) {
        bestAlloc = d.maxAllocation;
        best = devs[i];
        bestPlatform = platforms[p];
        d.ok = true;
        bestInfo = d;
      }
    }
  }

  if (!best) {
    if (!sawGpu)
      c.why = "the ICD reports no GPU device (CPU and accelerator devices only, and a CPU through this path is the CPU path with extra "
              "transfers) — the render stays on the CPU";
    else
      c.why = "no GPU device this renderer accepts: " + (rejected.empty() ? std::string("none reported") : rejected) + " — CPU path";
    return;
  }

  c.dev = best;
  c.platform = bestPlatform;
  c.info = bestInfo;
  c.why = "GPU: " + describeDevice(c.info);
}

} // namespace

/** @brief Every entry point this file calls, checked in one place. The alternative is calling a null
 * function pointer in the middle of a stage, which is a crash rather than a fallback — and a crash inside
 * an OFX entry point is a host crash (AGENTS rule 1), so this is the load-bearing version of the check. */
bool Loader::complete() const
{
  return getPlatformIDs && getDeviceIDs && getDeviceInfo && createContext && createCommandQueue && createProgramWithSource &&
         buildProgram && getProgramBuildInfo && createKernel && setKernelArg && enqueueNDRangeKernel && createBuffer &&
         enqueueWriteBuffer && enqueueReadBuffer && releaseMemObject && releaseKernel && releaseProgram && releaseCommandQueue &&
         releaseContext && flush && finish;
}

const std::string &probeMessage()
{
  Cache &c = cache();
  std::lock_guard<std::mutex> guard(c.mutex);
  probe(c);
  return c.why;
}

const DeviceInfo &device()
{
  Cache &c = cache();
  std::lock_guard<std::mutex> guard(c.mutex);
  probe(c);
  return c.info;
}

bool available()
{
  Cache &c = cache();
  std::lock_guard<std::mutex> guard(c.mutex);
  probe(c);
  return c.loader.complete() && c.info.ok;
}

size_t maxAllocation()
{
  Cache &c = cache();
  std::lock_guard<std::mutex> guard(c.mutex);
  probe(c);
  if (!c.info.ok)
    return 0;
  // The device's own ceiling, and half the card: the other half is the driver, the display and whatever
  // else is competing for this GPU. A plugin that allocated up to maxAlloc on a card driving the viewer's
  // own window is a plugin that fails on the machine it is being used on.
  cl_ulong half = c.info.globalMemory / 2;
  cl_ulong cap = c.info.maxAllocation;
  if (half > 0 && half < cap)
    cap = half;
  return (size_t)cap;
}

// The context and queue, and the program, built once. The build options are deliberately empty: a
// relaxed-math build is what would turn gpu.h's "the two paths differ by accumulation order" into
// something nobody can characterise.
Program *program()
{
  Cache &c = cache();
  std::lock_guard<std::mutex> guard(c.mutex);
  probe(c);
  if (!c.loader.complete() || !c.info.ok)
    return 0;
  if (c.progDone)
    return c.prog.ok ? &c.prog : 0;

  c.progDone = true;
  Loader &api = c.loader;
  cl_int err = kCL_SUCCESS;

  cl_context_properties props[3];
  props[0] = kCL_CONTEXT_PLATFORM;
  props[1] = (cl_context_properties)c.platform;
  props[2] = 0;
  c.ctx = api.createContext(props, 1, &c.dev, 0, 0, &err);
  if (!c.ctx) {
    c.why = "clCreateContext failed: " + statusText(err) + " — CPU path";
    return 0;
  }
  c.queue = api.createCommandQueue(c.ctx, c.dev, 0, &err);
  if (!c.queue) {
    c.why = "clCreateCommandQueue failed: " + statusText(err) + " — CPU path";
    api.releaseContext(c.ctx);
    c.ctx = 0;
    return 0;
  }
  c.ctxDone = true;

  const char *src = kernelSource();
  const size_t len = std::strlen(src);
  c.prog.program = api.createProgramWithSource(c.ctx, 1, &src, &len, &err);
  if (!c.prog.program) {
    c.why = "clCreateProgramWithSource failed: " + statusText(err) + " — CPU path";
    return 0;
  }
  err = api.buildProgram(c.prog.program, 0, 0, "", 0, 0);
  if (err != kCL_SUCCESS) {
    size_t n = 0;
    api.getProgramBuildInfo(c.prog.program, c.dev, kCL_PROGRAM_BUILD_LOG, 0, 0, &n);
    if (n) {
      std::vector<char> log(n + 1, 0);
      api.getProgramBuildInfo(c.prog.program, c.dev, kCL_PROGRAM_BUILD_LOG, n, &log[0], 0);
      c.prog.log = std::string(&log[0]);
    }
    c.why = "the GPU kernels would not build on this device (" + statusText(err) + "): " + c.prog.log + " — CPU path";
    return 0;
  }

  static const char *const names[5] = { "zd_extract_direct", "zd_extract_partial", "zd_extract_cells", "zd_convolve", "zd_composite" };
  cl_kernel *out[5] = { &c.prog.kExtractDirect, &c.prog.kExtractPartial, &c.prog.kExtractCells, &c.prog.kConvolve, &c.prog.kComposite };
  for (int i = 0; i < 5; ++i) {
    *out[i] = api.createKernel(c.prog.program, names[i], &err);
    if (!*out[i]) {
      c.why = std::string("clCreateKernel(") + names[i] + ") failed: " + statusText(err) + " — CPU path";
      return 0;
    }
  }
  c.prog.api = &c.loader;
  c.prog.ok = true;
  return &c.prog;
}

// ---------------------------------------------------------------------------
// The kernels
// ---------------------------------------------------------------------------
// One source string, compiled once per process by @ref program. Every device function here is a
// transcription of the CPU function it is named after, in the same order of operations:
//
//   zdBracket / zdSpacingAt / zdPixelHash / zdDitheredCoC / zdDampedCoC / zdPixelPair
//                      LevelSet::bracket, zd::pixelHash (lowbias32), zd::ditheredCoC,
//                      LayeredRender's dampedCoC and pixelPair — the level assignment, transcribed so a
//                      pixel joins the same levels with the same weights on either path
//   zdExtractSums      extractRows' inner loop: the same clips, the same skip tests, the same order
//   zd_convolve        blurRows' tap loop: same tap order, same "outside the bokeh's support" skip
//   zd_composite       compositeRows: same bilinear weights, same over-blend, same clamps
//
// What is *not* transcribed: the accumulation. The device sums in double where the CPU sums a row of
// float32 (blurRows) and tiles a cell into sub-sums where the CPU walks it one scanline at a time
// (extractRows). That is the whole numerical promise, and tests/gpu_parity.cpp measures it instead of
// assuming it. The build options are empty, so no compiler flag re-writes the arithmetic either.
const char *kernelSource()
{
  static const char *const src = R"CLC(
#ifdef cl_khr_fp64
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#else
#error "ZDefocusZ's GPU kernels accumulate in double: this device offers no cl_khr_fp64"
#endif

/* floor() of a quotient, rounding toward -infinity: layered.cpp's floorDiv, and it has to be this one
   too because a render region may sit at negative coordinates. */
long long zdFloorDiv(long long a, long long b)
{
  long long q = a / b;
  if (q * b > a)
    --q;
  return q;
}

typedef struct { int i0, i1; double t; } ZdBracket;
typedef struct { int i0, i1; double w0, w1; } ZdPair;
typedef struct { double r, g, b, c, w; } ZdSums;

/* LevelSet::bracket: the two levels a CoC brackets into, and how far along the seam it sits. */
ZdBracket zdBracket(__global const double *radii, int n, double coc)
{
  ZdBracket out;
  out.i0 = 0;
  out.i1 = 0;
  out.t = 0.0;
  if (n == 0)
    return out;
  if (!(coc > 0.0))
    return out; /* the sharp level, and nothing else */

  int lo = 0, hi = n - 1; /* largest i with radii[i] <= coc */
  while (hi - lo > 1) {
    const int mid = lo + (hi - lo) / 2;
    if (radii[mid] <= coc)
      lo = mid;
    else
      hi = mid;
  }
  if (coc >= radii[hi]) { out.i1 = out.i0 = hi; return out; } /* at or above the top level */
  if (coc == radii[lo]) { out.i1 = out.i0 = lo; return out; } /* exactly on a level: no seam */
  out.i0 = lo;
  out.i1 = hi;
  const double span = radii[hi] - radii[lo];
  out.t = span > 0.0 ? (coc - radii[lo]) / span : 0.0;
  return out;
}

/* LevelSet::spacingAt: the width of the seam a CoC sits in, which is the amplitude the dither swings
   by, so it has to be the same seam for the same reason. */
double zdSpacingAt(__global const double *radii, int n, double coc)
{
  const ZdBracket b = zdBracket(radii, n, coc);
  if (b.i1 <= b.i0)
    return 0.0;
  return radii[b.i1] - radii[b.i0];
}

/* zd::pixelHash, lowbias32: functions of (x, y) alone, so a pixel gets the same value whatever band
   it is rendered in - which is what makes a banded render and an unbanded one agree. */
double zdPixelHash(int x, int y)
{
  uint h = (uint)x * 374761393u + (uint)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (double)h * (1.0 / 4294967296.0);
}

/* zd::ditheredCoC, with the enable test folded into `ditherScale` (the host passes
   LayerOptions::dither / kDitherMax, and 0 means the knob is off). */
double zdDitheredCoC(double coc, int x, int y, __global const double *radii, int n)
{
  if (!(coc > 0.0) || n == 0)
    return coc; /* an in-focus pixel is never dithered *into* a blur */
  const double amp = zdSpacingAt(radii, n, coc);
  if (!(amp > 0.0))
    return coc;
  double v = coc + (zdPixelHash(x, y) - 0.5) * amp; /* +- half a step, locally */
  if (v < 0.0)
    v = 0.0;
  const double top = radii[n - 1];
  return v > top ? top : v;
}

/* layered.cpp's dampedCoC: the knob scales the swing around the undithered value. */
double zdDampedCoC(double coc, int x, int y, __global const double *radii, int n, double scale)
{
  if (!(scale > 0.0))
    return coc;
  const double d = zdDitheredCoC(coc, x, y, radii, n);
  return coc + (d - coc) * scale;
}

/* pixelPair: the pair of levels a pixel contributes to and the share each one owns. The weights sum to
   1 by construction, which is what makes a fully in-focus render its input exactly. */
ZdPair zdPixelPair(double coc, int x, int y, __global const double *radii, int n, double scale)
{
  const ZdBracket b = zdBracket(radii, n, zdDampedCoC(coc, x, y, radii, n, scale));
  ZdPair p;
  p.i0 = b.i0;
  p.i1 = b.i1;
  p.w0 = 1.0 - b.t;
  p.w1 = (b.i1 == b.i0) ? 0.0 : b.t;
  return p;
}

/* layered.cpp's straightLuma: what the bloom threshold compares against. */
double zdStraightLuma(double r, double g, double b)
{
  const double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  return l > 0.0 ? l : 0.0;
}

ZdSums zdZeroSums(void)
{
  ZdSums s;
  s.r = 0.0;
  s.g = 0.0;
  s.b = 0.0;
  s.c = 0.0;
  s.w = 0.0;
  return s;
}

/* The AoS grid LayeredRender uses: five planes interleaved per cell, rows major, addressed from
   (gX0, gY0) — layered.cpp's cellAt, so a device cell *is* the CPU cell at the same coordinates. */
__global float *zdCell(__global float *g, int gX0, int gY0, int gCols, int col, int row)
{
  return g + (((size_t)(row - gY0) * (size_t)gCols + (size_t)(col - gX0)) * 5u);
}

/* extractRows' inner two loops, over one rectangle of pixels. Same tests in the same order:
   `rowOk` stands for the delivered scanline the plane hands `img.row(y)` (a row the host does not
   deliver holds neither light nor coverage, and the host stages that flag rather than a row), the side
   plane is `m_coc.sideAt` — staged, so outside the prepared map it reads *far* exactly as CoCMap::sideAt
   does — `cocP` is `m_coc.cocAt` (0 outside the map), the pair is `pixelPair`, the component read is the
   `stride >= 4 / == 3 / else` branch, then the alpha and finiteness guards, then bloom, then the five
   sums. `seed` lets a caller continue another item's partial sum. */
ZdSums zdExtractRect(__global const float *img, __global const float *cocP, __global const uchar *sideP,
                     __global const uchar *rowOk, __global const double *radii, int numLevels, int winX0, int winY0,
                     int winW, int comp, int x0, int x1, int y0, int y1, int level, int sideFilter, double ditherScale,
                     int bloomOn, double linThreshold, double linGain, ZdSums seed)
{
  ZdSums s = seed;
  for (int y = y0; y < y1; ++y) {
    const int rowOff = (y - winY0) * winW;
    if (!rowOk[y - winY0])
      continue;
    for (int x = x0; x < x1; ++x) {
      const int off = rowOff + (x - winX0);
      if (sideFilter >= 0 && sideP[off] != (uchar)sideFilter)
        continue; /* this pixel belongs to the other ladder */
      const ZdPair pr = zdPixelPair((double)cocP[off], x, y, radii, numLevels, ditherScale);
      double w = 0.0;
      if (pr.i0 == level)
        w = pr.w0;
      else if (pr.i1 == level)
        w = pr.w1;
      if (!(w > 0.0))
        continue;
      const float *p = &img[off * comp];
      double r, g, b, a;
      if (comp >= 4) {
        r = p[0];
        g = p[1];
        b = p[2];
        a = p[3];
      } else if (comp == 3) {
        r = p[0];
        g = p[1];
        b = p[2];
        a = 1.0; /* an RGB source has no alpha channel to be transparent with */
      } else {
        r = g = b = (double)p[0];
        a = 1.0;
      }
      if (!(a > 0.0) || !isfinite(r) || !isfinite(g) || !isfinite(b))
        continue;
      if (a > 1.0)
        a = 1.0;
      if (bloomOn && zdStraightLuma(r, g, b) > linThreshold) {
        r *= linGain;
        g *= linGain;
        b *= linGain;
      }
      const double pm = a * w;
      s.r += r * pm;
      s.g += g * pm;
      s.b += b * pm;
      s.c += pm;
      s.w += w;
    }
  }
  return s;
}

/* One cell, one work item, its pixels walked in the CPU's own order (rows ascending, columns ascending
   across the cell) — which is why this path reproduces extractRows *exactly*, bit for bit. Used for
   every cell a single item can cover, i.e. `cell <= tileSize`. */
__kernel void zd_extract_direct(__global float *ga, __global const float *img, __global const float *cocP,
                                __global const uchar *sideP, __global const uchar *rowOk, __global const double *radii,
                                int numLevels, int winX0, int winY0, int winW, int winH, int comp, int gX0, int gY0,
                                int gCols, int cxLo, int cyLo, int cell, int xLo, int xHi, int level, int sideFilter,
                                double ditherScale, int bloomOn, double linThreshold, double linGain)
{
  const int cx = cxLo + (int)get_global_id(0);
  const int cy = cyLo + (int)get_global_id(1);
  const int cxs = cx * cell, cys = cy * cell;
  const int x0 = max(cxs, xLo), x1 = min(cxs + cell, xHi);
  const int y0 = max(cys, winY0), y1 = min(cys + cell, winY0 + winH);
  if (x1 <= x0 || y1 <= y0)
    return;
  const ZdSums s = zdExtractRect(img, cocP, sideP, rowOk, radii, numLevels, winX0, winY0, winW, comp, x0, x1, y0, y1,
                                 level, sideFilter, ditherScale, bloomOn, linThreshold, linGain, zdZeroSums());
  if (s.r == 0.0 && s.g == 0.0 && s.b == 0.0 && s.c == 0.0 && s.w == 0.0)
    return; /* extractRows' "an all-zero cell is already zero" — the host uploaded a zeroed grid */
  const double invArea = 1.0 / ((double)cell * (double)cell);
  __global float *d = zdCell(ga, gX0, gY0, gCols, cx, cy);
  d[0] = (float)(s.r * invArea);
  d[1] = (float)(s.g * invArea);
  d[2] = (float)(s.b * invArea);
  d[3] = (float)(s.c * invArea);
  d[4] = (float)(s.w * invArea);
}

/* One tile of one cell, for a cell wider than a work item should walk. The host splits every cell into
   `tilesX * tilesY` tiles of `tileSize` pixels — the last one of a row or column is narrower — so the
   tiles cover the cell exactly and this item's rectangle is always inside it. Every tile writes its
   slot, zero or not, which is why the partial buffer needs no clearing pass and why there is no sixth
   kernel: the slots that hold nothing hold zeros. The slot is addressed by the linear index of the cell
   within the box, which @ref zd_extract_cells recomposes as `row * nCols + col`; the two must agree,
   because a partial kernel that numbers its cells differently from its combiner is a render built out of
   the wrong cells' sums. */
__kernel void zd_extract_partial(__global double *part, __global const float *img, __global const float *cocP,
                                 __global const uchar *sideP, __global const uchar *rowOk, __global const double *radii,
                                 int numLevels, int winX0, int winY0, int winW, int winH, int comp, int nCols, int cxLo,
                                 int cyLo, int cell, int xLo, int xHi, int tilesX, int tilesY, int tileSize, int level,
                                 int sideFilter, double ditherScale, int bloomOn, double linThreshold, double linGain)
{
  const int cellIdx = (int)get_global_id(0);
  const int tile = (int)get_global_id(1);
  const int cx = cxLo + cellIdx % nCols;
  const int cy = cyLo + cellIdx / nCols;
  const int tx = tile % tilesX, ty = tile / tilesX;
  const int cxs = cx * cell, cys = cy * cell;
  const int xs0 = cxs + tx * tileSize, xs1 = min(cxs + min((tx + 1) * tileSize, cell), cxs + cell);
  const int x0 = max(xs0, xLo), x1 = min(xs1, xHi);
  const int ys0 = cys + ty * tileSize, ys1 = min(cys + min((ty + 1) * tileSize, cell), cys + cell);
  const int y0 = max(ys0, winY0), y1 = min(ys1, winY0 + winH);
  const ZdSums s = zdExtractRect(img, cocP, sideP, rowOk, radii, numLevels, winX0, winY0, winW, comp, x0, x1, y0, y1,
                                 level, sideFilter, ditherScale, bloomOn, linThreshold, linGain, zdZeroSums());
  __global double *d = part + ((size_t)cellIdx * (size_t)(tilesX * tilesY) + (size_t)tile) * 5u;
  d[0] = s.r;
  d[1] = s.g;
  d[2] = s.b;
  d[3] = s.c;
  d[4] = s.w;
}

/* The combiner: the tiles' sub-sums, added in tile order (rows of tiles ascending, left to right — the
   order the host launched them in, so the grouping is reproducible even though it is not the CPU's
   pixel-at-a-time grouping), then scaled by the same 1/(cell*cell) and stored as float. The same
   all-zero skip, for the same reason: the grid was uploaded zeroed. */
__kernel void zd_extract_cells(__global float *ga, __global const double *part, int gX0, int gY0, int gCols, int nCols,
                               int cxLo, int cyLo, int cell, int tiles)
{
  const int col = (int)get_global_id(0), row = (int)get_global_id(1); /* 0..nCols-1, 0..nRows-1 */
  const int cx = cxLo + col, cy = cyLo + row;
  /* The linear index of this cell inside the box, which is what @ref zd_extract_partial decomposes the
     other way (`cx = cxLo + cellIdx % nCols`, `cy = cyLo + cellIdx / nCols`). Both are *relative* to the
     launched box: cxLo and cyLo are absolute cell coordinates, so subtracting them here would hand every
     cell another cell's partial sums - and, at the far corner of the box, an index past the slots the
     partial kernel ever wrote. */
  const size_t cellIdx = (size_t)row * (size_t)nCols + (size_t)col;
  ZdSums s = zdZeroSums();
  const __global double *p = part + cellIdx * (size_t)tiles * 5u;
  for (int t = 0; t < tiles; ++t) {
    s.r += p[0];
    s.g += p[1];
    s.b += p[2];
    s.c += p[3];
    s.w += p[4];
    p += 5;
  }
  if (s.r == 0.0 && s.g == 0.0 && s.b == 0.0 && s.c == 0.0 && s.w == 0.0)
    return;
  const double invArea = 1.0 / ((double)cell * (double)cell);
  __global float *d = zdCell(ga, gX0, gY0, gCols, cx, cy);
  d[0] = (float)(s.r * invArea);
  d[1] = (float)(s.g * invArea);
  d[2] = (float)(s.b * invArea);
  d[3] = (float)(s.c * invArea);
  d[4] = (float)(s.w * invArea);
}

/* blurRows: one convolved cell per work item. Same tap order (rows then columns of the stencil), the same
   skip of the taps outside the bokeh's support, the same five planes, and the same identity copy for a
   single tap of 1. The ring the grid reserves (margin = dilate + half + 2, wider than the `half` this
   stage reads outside the convolved box) means the tap loop reads no bounds test here either — a cell
   outside the grid is not read at all, because the grid does not reach it, exactly as on the CPU. The
   parameter is called kHalf because `half` is itself a type name in OpenCL C (cl_khr_fp16), and a
   declaration that reads `int half` is a syntax error a vendor compiler reports as one. */
__kernel void zd_convolve(__global const float *ga, __global float *gb, __global const float *taps, int span,
                          int kHalf, int identity, int gX0, int gY0, int gCols, int gRows, int convC0,
                          int convC1, int rLo, int rHi, int kp0, int kp1, int kp2, int kp3, int kp4)
{
  const int c = convC0 + (int)get_global_id(0);
  const int r = rLo + (int)get_global_id(1);
  if (r < gY0 || r >= gY0 + gRows)
    return; /* blurRows' row skip, for a row the grid does not cover */
  __global float *d = gb + (((size_t)(r - gY0) * (size_t)gCols + (size_t)(c - gX0)) * 5u);
  d[0] = d[1] = d[2] = d[3] = d[4] = 0.0f; /* every convolved cell starts empty */
  if (identity) {
    const __global float *srow = ga + (((size_t)(r - gY0) * (size_t)gCols + (size_t)(c - gX0)) * 5u);
    for (int p = 0; p < 5; ++p)
      d[p] = srow[p]; /* the same cell of the extracted grid: bit-identical, as on the CPU */
    return;
  }
  const __global float *k0 = taps + (size_t)kp0 * (size_t)span * (size_t)span;
  const __global float *k1 = taps + (size_t)kp1 * (size_t)span * (size_t)span;
  const __global float *k2 = taps + (size_t)kp2 * (size_t)span * (size_t)span;
  const __global float *kc = taps + (size_t)kp3 * (size_t)span * (size_t)span;
  const size_t cBase = (size_t)(c - gX0) - (size_t)kHalf; /* the leftmost tap column, as a grid column */
  double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, a4 = 0.0;
  for (int dy = -kHalf; dy <= kHalf; ++dy) {
    const __global float *src = ga + (((size_t)(r + dy - gY0) * (size_t)gCols + cBase) * 5u);
    const int trow = (dy + kHalf) * span;
    const __global float *kr0 = k0 + trow, *kr1 = k1 + trow, *kr2 = k2 + trow, *krc = kc + trow;
    for (int dx = -kHalf; dx <= kHalf; ++dx) {
      const float tc = krc[dx + kHalf];
      const float t0 = kr0[dx + kHalf], t1 = kr1[dx + kHalf], t2 = kr2[dx + kHalf];
      if (t0 == 0.0f && t1 == 0.0f && t2 == 0.0f && tc == 0.0f)
        continue; /* a bokeh's support is not a square */
      const __global float *s = src + (size_t)(dx + kHalf) * 5u;
      a0 += (double)t0 * (double)s[0];
      a1 += (double)t1 * (double)s[1];
      a2 += (double)t2 * (double)s[2];
      a3 += (double)tc * (double)s[3];
      a4 += (double)tc * (double)s[4];
    }
  }
  d[0] = (float)a0;
  d[1] = (float)a1;
  d[2] = (float)a2;
  d[3] = (float)a3;
  d[4] = (float)a4;
}

/* compositeRows: one output pixel per work item, reading the four convolved cells its bilinear weights
   land on and blending this level over whatever the earlier levels left in the accumulator. Pixels are
   independent *within* a visit — the accumulator is shared between visits, not between pixels — which is
   what lets the accumulator stay on the device for the whole band and come back once (@ref Session).
   The cell and weight maths, the coverage clamp, the source-over rule and the float stores are the
   CPU's, line for line; the accumulator is read as float and computed in double, which is the difference
   the parity suite measures. */
__kernel void zd_composite(__global const float *gb, __global float *acc, __global float *accW, int gX0, int gY0,
                           int gCols, int gRows, int cell, int compX0, int compX1, int yA, int yB, int rectX0,
                           int rectY0, int rectW)
{
  const int x = compX0 + (int)get_global_id(0);
  const int y = yA + (int)get_global_id(1);
  const long long den = 2LL * (long long)cell;
  const long long ny = 2LL * (long long)y + 1 - (long long)cell;
  const long long r0 = zdFloorDiv(ny, den); /* the cell whose centre is at or below this pixel's */
  if (r0 < gY0 || r0 + 1 >= gY0 + gRows)
    return; /* the level's grid does not cover this row */
  const long long nx = 2LL * (long long)x + 1 - (long long)cell;
  const long long c0 = zdFloorDiv(nx, den);
  const double dden = (double)den;
  const double ty = (double)(ny - r0 * (long long)cell * 2) / dden;
  const double tx = (double)(nx - c0 * (long long)cell * 2) / dden;
  const __global float *grow0 = gb + (size_t)(r0 - gY0) * (size_t)gCols * 5u;
  const __global float *grow1 = grow0 + (size_t)gCols * 5u;
  const __global float *p00 = grow0 + (size_t)(c0 - gX0) * 5u;
  const __global float *p10 = p00 + 5u, *p01 = grow1 + (size_t)(c0 - gX0) * 5u, *p11 = p01 + 5u;
  const double w00 = (1.0 - tx) * (1.0 - ty), w10 = tx * (1.0 - ty), w01 = (1.0 - tx) * ty, w11 = tx * ty;
  const double cr = w00 * p00[0] + w10 * p10[0] + w01 * p01[0] + w11 * p11[0];
  const double cg = w00 * p00[1] + w10 * p10[1] + w01 * p01[1] + w11 * p11[1];
  const double cb = w00 * p00[2] + w10 * p10[2] + w01 * p01[2] + w11 * p11[2];
  double ca = w00 * p00[3] + w10 * p10[3] + w01 * p01[3] + w11 * p11[3];
  const double cw = w00 * p00[4] + w10 * p10[4] + w01 * p01[4] + w11 * p11[4];
  if (ca < 0.0)
    ca = 0.0; /* an ulp of drift in a convolved plane must not make a layer subtract coverage */
  else if (ca > 1.0)
    ca = 1.0;
  const double tr = 1.0 - ca;
  const size_t pix = ((size_t)(y - rectY0) * (size_t)rectW) + (size_t)(x - rectX0);
  __global float *a = acc + pix * 4u;
  __global float *aw = accW + pix * 2u;
  a[0] = (float)(cr + (double)a[0] * tr);
  a[1] = (float)(cg + (double)a[1] * tr);
  a[2] = (float)(cb + (double)a[2] * tr);
  a[3] = (float)((double)a[3] + ca * (1.0 - (double)a[3]));
  aw[0] += (float)ca;
  aw[1] += (float)cw;
}
)CLC";
  return src;
}

// ---------------------------------------------------------------------------
// One band's device state
// ---------------------------------------------------------------------------

namespace {

/** @brief The tiles a cell of this width is split into: `ceil(cell / kGpuTile)` by the same number, so
 * the tiles cover the cell exactly and the last row and column of tiles are the narrower ones. Square
 * because the cell is square (gpu.h's kGpuTile note explains why tiles at all). */
void tilesFor(int cell, int *tilesX, int *tilesY)
{
  const int n = (cell + kGpuTile - 1) / kGpuTile;
  *tilesX = n < 1 ? 1 : n;
  *tilesY = n < 1 ? 1 : n;
}

/** @brief The widest cell of the level set: the coarsest lattice any level of this render gathers at.
 *
 * It exists because @ref RenderPlan::maxReach is a margin measured in *cells of each level's own lattice*,
 * while a level's extraction box is that box rounded outwards to whole cells: @c pxX0 is
 * <c>cellStart(convC0 - half - dilate, cell)</c>, and @c convC0 sits two cells inside the level's composite
 * box (`readCellLo` plus @c pad), so the box reaches @c (levelMargin + 1) * cell pixels outside the render
 * rect rather than @c levelMargin * cell. A coarse lattice is therefore the one case where the window would
 * otherwise stop a level short — up to @c cell - 1 px of it — which at CoC 512 (cell 73) is most of a cell
 * and, since extraction is 79 % of a CPU render at that radius (DESIGN §7), the difference between the GPU
 * path running where it matters and it falling back exactly where it would help most. The extra cell is
 * *not* added to @c maxReach: what the plugin asks its host for stays the number the offscreen probe
 * measures, and a window may only read pixels the host delivered anyway (it is clipped to the plane). */
int maxLatticeCell(const LevelSet &levels, double aspect, double cap)
{
  int cell = 1;
  const int L = levels.count();
  for (int i = 0; i < L; ++i) {
    const int c = cellForRadius(cappedRadius(levels.radius[(size_t)i], cap), aspect);
    if (c > cell)
      cell = c;
  }
  return cell;
}

/** @brief @ref LayeredRender's `floorDiv` (layered.cpp, file-local there): floor of a quotient, rounding
 * toward -infinity, because a render region may sit at negative coordinates and a bilinear cell choice has
 * to land in the same cell on both sides of zero. */
long long floorDivLL(long long a, long long b)
{
  const long long q = a / b;
  return q * b > a ? q - 1 : q;
}

/** @brief Kernel arguments, written in the order the kernel declares them, one `<<` per parameter.
 * Chaining it keeps the host-side argument list in the same *visual* order as the kernel's own, which is
 * the only defence there is against the classic OpenCL bug of a wrong-sized or misplaced argument: an
 * argument that does not fit is a CL_INVALID_ARG_SIZE at enqueue time rather than a wrong render. The
 * kernel's name is carried so the message names the stage that failed. */
struct ArgWriter {
  Loader &api;
  cl_kernel k;
  const char *name;
  cl_uint index;
  bool ok;
  std::string &why;

  ArgWriter(Loader &a, cl_kernel kernel, const char *kernelName, std::string &reason)
      : api(a), k(kernel), name(kernelName), index(0), ok(true), why(reason)
  {
  }

  template <typename T> ArgWriter &operator<<(const T &value)
  {
    if (!ok)
      return *this; // the first failure stops the chain; the caller checks ok and returns false
    const cl_int err = api.setKernelArg(k, index, sizeof(T), &value);
    if (err != kCL_SUCCESS) {
      ok = false;
      why = std::string("clSetKernelArg(") + name + ", arg " + std::to_string((int)index) + "): " + statusText(err);
    }
    ++index;
    return *this;
  }
};

/** @brief A byte count as the number a log reader can compare against a knob: KiB below a megabyte, MB
 * up to a gigabyte, GB beyond that. Buffer messages use it because the interesting failures happen at
 * both ends of that range — a 4 KB window and a 3 GB one are refused for opposite reasons. */
std::string sizeText(size_t bytes)
{
  const double b = (double)bytes;
  char buf[64];
  if (bytes < (size_t)1024u)
    std::snprintf(buf, sizeof(buf), "%zu B", bytes);
  else if (bytes < (size_t)1024u * 1024u)
    std::snprintf(buf, sizeof(buf), "%.1f KiB", b / 1024.0);
  else if (bytes < (size_t)1024u * 1024u * 1024u)
    std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
  else
    std::snprintf(buf, sizeof(buf), "%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
  return std::string(buf);
}

/** @brief One device allocation. Returns 0 with the reason when the device refused it, which is the
 * point: the plugin's fallback is a *band on the CPU*, and a band that fails on an allocation has to
 * know that before it has half-composited something. */
cl_mem allocBuffer(Loader &api, cl_context ctx, size_t bytes, cl_mem_flags flags, const char *what, std::string &why)
{
  if (bytes == 0) {
    why = std::string("a ") + what + " buffer of 0 bytes was asked for — CPU path";
    return 0;
  }
  cl_int err = kCL_SUCCESS;
  cl_mem buf = api.createBuffer(ctx, flags, bytes, 0, &err);
  if (!buf) {
    why = std::string("clCreateBuffer(") + what + ", " + sizeText(bytes) + "): " + statusText(err) + " — CPU path";
    return 0;
  }
  return buf;
}

/** @brief A blocking copy host -> device, then device -> host. Blocking because a Session's stages are
 * strictly ordered (extract, fill, convolve, composite) and the whole point of the round trip is that the
 * CPU sees the device's answer *now*; the cost of that is what the parity suite measures against the CPU's
 * own stage times, not something this file guesses at. */
bool writeBuf(Loader &api, cl_command_queue queue, cl_mem buf, size_t offset, size_t bytes, const void *ptr, const char *what,
              std::string &why)
{
  const cl_int err = api.enqueueWriteBuffer(queue, buf, 1, offset, bytes, ptr, 0, 0, 0);
  if (err != kCL_SUCCESS) {
    why = std::string("clEnqueueWriteBuffer(") + what + ", " + sizeText(bytes) + "): " + statusText(err) + " — CPU path";
    return false;
  }
  return true;
}

bool readBuf(Loader &api, cl_command_queue queue, cl_mem buf, size_t offset, size_t bytes, void *ptr, const char *what, std::string &why)
{
  const cl_int err = api.enqueueReadBuffer(queue, buf, 1, offset, bytes, ptr, 0, 0, 0);
  if (err != kCL_SUCCESS) {
    why = std::string("clEnqueueReadBuffer(") + what + ", " + sizeText(bytes) + "): " + statusText(err) + " — CPU path";
    return false;
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Session: construction, the band's buffers, launching
// ---------------------------------------------------------------------------

Session::Session()
    : m_api(0), m_prog(0), m_rend(0), m_queue(0), m_cap(0), m_used(0), m_img(0), m_coc(0), m_side(0), m_rowOk(0), m_imgBytes(0),
      m_cocBytes(0),
      m_sideBytes(0), m_rowOkBytes(0), m_winX0(0), m_winY0(0), m_winW(0), m_winH(0), m_imgComp(4), m_acc(0), m_accW(0), m_accBytes(0),
      m_accWBytes(0), m_rectX0(0), m_rectW(0), m_rectY0(0), m_gridA(0), m_gridB(0), m_part(0), m_taps(0), m_gridCells(0),
      m_partCount(0), m_tapsCount(0), m_levelsBuf(0), m_levelsBytes(0), m_numLevels(0), m_convDone(false), m_convLevel(-1),
      m_convSide(-1), m_convCols(0), m_convRows(0), m_convRowLo(1), m_convRowHi(0), m_band(false)
{
}

Session::~Session()
{
  if (!m_api)
    return; // nothing was ever allocated, because start() never got a program
  Loader &api = *m_api;
  api.releaseMemObject(m_img);
  api.releaseMemObject(m_coc);
  api.releaseMemObject(m_side);
  api.releaseMemObject(m_rowOk);
  api.releaseMemObject(m_acc);
  api.releaseMemObject(m_accW);
  api.releaseMemObject(m_gridA);
  api.releaseMemObject(m_gridB);
  api.releaseMemObject(m_part);
  api.releaseMemObject(m_taps);
  api.releaseMemObject(m_levelsBuf);
  m_band = false;
}

/** @brief The running total of this band's buffers, checked against the device's own ceiling
 * (@ref maxAllocation, which is the smaller of `CL_DEVICE_MAX_MEM_ALLOC_SIZE` and half the card). The
 * plugin caps its *own* budget the same way (zdefocusz.cpp), so band splitting has already happened
 * before a Session exists: what this check catches is the rarer case — a device with a small ceiling
 * rather than a small render. */
bool Session::budget(size_t bytes, const char *what, std::string &why)
{
  if (bytes == 0)
    return true;
  if (m_used + bytes > m_cap) {
    why = std::string("this band needs ") + sizeText(m_used + bytes) + " of device buffers (" + what +
          ") and this device hands out at most " + sizeText(m_cap) + " — CPU path";
    return false;
  }
  m_used += bytes;
  return true;
}

/** @brief The current level's two grids, grown when this level needs more cells than the last one did.
 * Never *shrunk* between levels: the kernels are told this level's own `gCols`/`gY0`, so a buffer that is
 * bigger than this level's grid is slack rather than a different render, and a reallocation per level
 * would be a free allocation failure waiting for a ladder that alternates wide and narrow. */
bool Session::ensureGrid(size_t cells, std::string &why)
{
  if (cells == 0) {
    why = "a level grid of 0 cells was asked for — CPU path";
    return false;
  }
  if (cells <= m_gridCells)
    return true;
  m_used -= m_gridCells * 5u * sizeof(float) * 2u;
  m_gridCells = 0;
  if (!budget(cells * 5u * sizeof(float) * 2u, "the level's extracted and convolved grids", why))
    return false;
  const size_t bytes = cells * 5u * sizeof(float);
  cl_context ctx = cache().ctx;
  cl_mem a = allocBuffer(*m_api, ctx, bytes, kCL_MEM_READ_WRITE, "grid A", why);
  if (!a) {
    m_used -= cells * 5u * sizeof(float) * 2u;
    return false;
  }
  cl_mem b = allocBuffer(*m_api, ctx, bytes, kCL_MEM_READ_WRITE, "grid B", why);
  if (!b) {
    m_api->releaseMemObject(a);
    m_used -= cells * 5u * sizeof(float) * 2u;
    return false;
  }
  if (m_gridA)
    m_api->releaseMemObject(m_gridA);
  if (m_gridB)
    m_api->releaseMemObject(m_gridB);
  m_gridA = a;
  m_gridB = b;
  m_gridCells = cells;
  return true;
}

bool Session::bandLive(LayeredRender &rend, const char *what, std::string &why)
{
  if (!m_band || !m_rend || m_rend != &rend || !m_api || !m_prog) {
    why = std::string("the ") + what + " was asked for on a session that has not started this band — CPU path";
    return false;
  }
  return true;
}

bool Session::needGrid(const LevelView &lv, std::string &why)
{
  if (!lv.valid) {
    why = "a grid was asked for with no current level — CPU path";
    return false;
  }
  return ensureGrid((size_t)lv.gCols * (size_t)lv.gRows, why);
}

/** @brief The extraction partials: 5 doubles per tile of every cell the tiled path splits. Allocated
 * per level, because the count depends on that level's cell and its cell box; a level that extracts
 * directly never asks for one. */
bool Session::ensurePartials(size_t slots, std::string &why)
{
  if (slots == 0)
    return true;
  if (slots <= m_partCount)
    return true;
  m_used -= m_partCount * 5u * sizeof(double);
  m_partCount = 0;
  const size_t bytes = slots * 5u * sizeof(double);
  if (!budget(bytes, "extraction's partial sums", why))
    return false;
  cl_mem p = allocBuffer(*m_api, cache().ctx, bytes, kCL_MEM_READ_WRITE, "extraction partials", why);
  if (!p) {
    m_used -= bytes;
    return false;
  }
  if (m_part)
    m_api->releaseMemObject(m_part);
  m_part = p;
  m_partCount = slots;
  return true;
}

/** @brief This band's accumulator, the device's copy of @ref LayeredRender::accum / @ref
 * LayeredRender::accumWeight: 4 floats per rect pixel and 2, laid out exactly as the CPU lays them out so
 * the round trip is one copy in each direction. Uploaded from the CPU's *zeroed* pair (@ref start),
 * downloaded once (@ref finish) — which is what a level visit's pixels being independent of each other
 * makes possible, and what a GPU render spends on the bus. */
bool Session::ensureAccum(size_t pixels, std::string &why)
{
  if (pixels == 0) {
    why = "an accumulator for 0 pixels was asked for — CPU path";
    return false;
  }
  const size_t bytes = pixels * 4u * sizeof(float), wbytes = pixels * 2u * sizeof(float);
  if (!budget(bytes + wbytes, "the composite accumulator", why))
    return false;
  cl_context ctx = cache().ctx;
  cl_mem a = allocBuffer(*m_api, ctx, bytes, kCL_MEM_READ_WRITE, "accumulator", why);
  if (!a) {
    m_used -= bytes + wbytes;
    return false;
  }
  cl_mem w = allocBuffer(*m_api, ctx, wbytes, kCL_MEM_READ_WRITE, "accumulator weights", why);
  if (!w) {
    m_api->releaseMemObject(a);
    m_used -= bytes + wbytes;
    return false;
  }
  m_acc = a;
  m_accW = w;
  m_accBytes = bytes;
  m_accWBytes = wbytes;
  return true;
}

/** @brief The two windows this band reads, assembled on the host and written once.
 *
 * Their extent is the point of the whole arrangement, so it is derived from the renderer's own numbers
 * rather than guessed: `prepare()` pads the render rect by `ceil(plan().maxReach)` (@ref
 * RenderPlan::maxReach, which is every level's `levelMargin` in pixels — kernel half-span, fill radius and
 * the bilinear read-back, each in that level's own cell), and a level's extraction box is its convolved box
 * widened by `half + dilate` cells and then rounded out to whole cells, which is that same margin plus one
 * cell of that level's own lattice (@ref maxLatticeCell, which the pad below adds). A coarse lattice is the
 * case this matters for: at CoC 512 the level's cell is 73 px, and stopping a cell short there would send the
 * widest blurs to the CPU — the ones extraction dominates (DESIGN §7). Those pixels sit outside the prepared
 * CoC map, where @ref CoCMap::cocAt gives CoC 0 and @ref CoCMap::sideAt *far* — which is what `extractRows`
 * reads there through those same accessors, so widening the window cannot change the picture: a CoC of 0
 * assigns such a pixel to the in-focus level with weight 1 and to every blurred level with weight 0, and no
 * blurred level reaches this far. So every pixel any level of this
 * band can gather lies inside that padded area — and clipping it to the delivered plane is what keeps the
 * device reading *exactly* the pixels the CPU stage reads, because `extractRows` reads through
 * `img.row(y)` and clips its columns to `[img.x1, img.x2)`. A row the clip removes is checked per visit
 * (@ref stageExtract), so a plane that delivered something the window does not hold is a fallback, not a
 * quieter render.
 *
 * The CoC and side values are staged through @ref CoCMap::cocAt / @ref CoCMap::sideAt, so outside the
 * prepared map the device reads what the CPU reads: CoC 0 and *far*. */
bool Session::ensureWindow(LayeredRender &rend, std::string &why)
{
  const Plane &img = rend.image();
  const CoCMap &coc = rend.cocMap();
  const RenderRect &rect = rend.rect();
  m_imgComp = rend.imageComponentCount();

  const int pad = (int)std::ceil(rend.plan().maxReach) + maxLatticeCell(rend.levelSet(), rend.aspect(), rend.layerOptions().spriteRadiusCap);
  m_winX0 = std::max(img.x1, rect.x1 - pad);
  m_winY0 = std::max(img.y1, rect.y1 - pad);
  m_winW = std::min(img.x2, rect.x2 + pad) - m_winX0;
  m_winH = std::min(img.y2, rect.y2 + pad) - m_winY0;
  if (m_winW <= 0 || m_winH <= 0) {
    m_winW = m_winH = 0;
    why = "the gather window this band needs is empty (the delivered plane and the padded render rect do "
          "not overlap) — CPU path";
    return false;
  }

  const size_t pixels = (size_t)m_winW * (size_t)m_winH;
  const size_t imgBytes = pixels * (size_t)m_imgComp * sizeof(float);
  const size_t cocBytes = pixels * sizeof(float);
  const size_t sideBytes = pixels;
  const size_t rowBytes = (size_t)m_winH;
  if (!budget(imgBytes + cocBytes + sideBytes + rowBytes, "the gathered picture, CoC map, side flags and row flags", why))
    return false;

  m_imgHost.assign(imgBytes, 0.0f);
  m_cocHost.assign(pixels, 0.0f);
  m_sideHost.assign(pixels, 0u);
  std::vector<unsigned char> rowFlags(m_winH, 0u);
  for (int y = m_winY0; y < m_winY0 + m_winH; ++y) {
    const int r = y - m_winY0;
    const float *src = img.empty() ? 0 : img.row(y);
    rowFlags[(size_t)r] = src ? 1u : 0u;
    for (int x = m_winX0; x < m_winX0 + m_winW; ++x) {
      const size_t off = (size_t)r * (size_t)m_winW + (size_t)(x - m_winX0);
      m_cocHost[off] = coc.cocAt(x, y);
      m_sideHost[off] = (unsigned char)coc.sideAt(x, y);
      if (!src)
        continue; // the row flag already says there is nothing here; the pixels stay zero
      const float *p = &src[(size_t)(x - img.x1) * (size_t)m_imgComp];
      for (int k = 0; k < m_imgComp; ++k)
        m_imgHost[off * (size_t)m_imgComp + (size_t)k] = p[k];
    }
  }

  cl_context ctx = cache().ctx;
  m_cocBytes = cocBytes;
  m_sideBytes = sideBytes;
  m_rowOkBytes = rowBytes;
  m_imgBytes = imgBytes;
  m_img = allocBuffer(*m_api, ctx, imgBytes, kCL_MEM_READ_ONLY, "gathered picture", why);
  m_coc = allocBuffer(*m_api, ctx, cocBytes, kCL_MEM_READ_ONLY, "CoC window", why);
  m_side = allocBuffer(*m_api, ctx, sideBytes, kCL_MEM_READ_ONLY, "side window", why);
  m_rowOk = allocBuffer(*m_api, ctx, rowBytes, kCL_MEM_READ_ONLY, "delivered-row flags", why);
  if (!m_img || !m_coc || !m_side || !m_rowOk)
    return false;
  if (!writeBuf(*m_api, m_queue, m_img, 0, imgBytes, &m_imgHost[0], "gathered picture", why))
    return false;
  if (!writeBuf(*m_api, m_queue, m_coc, 0, cocBytes, &m_cocHost[0], "CoC window", why))
    return false;
  if (!writeBuf(*m_api, m_queue, m_side, 0, sideBytes, &m_sideHost[0], "side window", why))
    return false;
  if (!writeBuf(*m_api, m_queue, m_rowOk, 0, rowBytes, &rowFlags[0], "delivered-row flags", why))
    return false;
  std::vector<float>().swap(m_imgHost); // the staging is a few tens of MB at a wide CoC: it has done its job
  std::vector<float>().swap(m_cocHost);
  std::vector<unsigned char>().swap(m_sideHost);
  return true;
}

/** @brief This level's aperture, uploaded. A kernel is a few KB of taps whatever the CoC is (a wide blur
 * has a *coarse lattice*, so its tap count stays small), which is why the taps are re-uploaded for every
 * visit rather than cached by identity: a capped ladder shares kernels, an uncapped one does not, and the
 * difference costs nothing measurable. @ref stageConvolve calls it, which is why @ref visit does not have
 * to: the stage that reads the taps is the stage that makes sure they are on the device, and a convolve
 * launched against taps that were never uploaded is a kernel reading a null buffer. */
bool Session::uploadKernel(LayeredRender &rend, const LevelView &lv, std::string &why)
{
  /* A LevelView is a copy, so it can go stale if a caller kept one across a beginLevel. The stages of this
   * file take their own, so this catches the parity suite handing a session the geometry of a level it is no
   * longer on — which would otherwise upload one level's taps and launch another level's convolution. */
  const LevelView now = rend.levelView();
  if (!lv.valid || lv.level != now.level || lv.side != now.side || lv.cell != now.cell || lv.gCols != now.gCols ||
      lv.gRows != now.gRows || lv.kernel != now.kernel) {
    why = "the level geometry the GPU path was handed is not the renderer's current level — CPU path";
    return false;
  }
  if (!lv.kernel) {
    why = "the current level has no kernel — CPU path";
    return false;
  }
  const size_t count = lv.kernel->taps.size();
  if (count == 0) {
    why = "the current level's kernel has no taps — CPU path";
    return false;
  }
  const size_t bytes = count * sizeof(float);
  if (count > m_tapsCount) {
    m_used -= m_tapsCount * sizeof(float);
    m_tapsCount = 0;
    if (!budget(bytes, "the level's kernel taps", why))
      return false;
    cl_mem t = allocBuffer(*m_api, cache().ctx, bytes, kCL_MEM_READ_ONLY, "kernel taps", why);
    if (!t) {
      m_used -= bytes;
      return false;
    }
    if (m_taps)
      m_api->releaseMemObject(m_taps);
    m_taps = t;
    m_tapsCount = count;
  }
  return writeBuf(*m_api, m_queue, m_taps, 0, bytes, &lv.kernel->taps[0], "kernel taps", why);
}

bool Session::launch(cl_kernel kernel, const char *name, size_t n0, size_t n1, const size_t *local, std::string &why)
{
  if (n0 == 0 || n1 == 0)
    return true; // a stage with no work in it is not an error, and a zero-sized NDRange *is* one
  const size_t g[2] = { n0, n1 };
  const cl_int err = m_api->enqueueNDRangeKernel(m_queue, kernel, 2, 0, g, local, 0, 0, 0);
  if (err != kCL_SUCCESS) {
    why = std::string("clEnqueueNDRangeKernel(") + name + ", " + std::to_string((long long)n0) + " x " +
          std::to_string((long long)n1) + "): " + statusText(err) + " — CPU path";
    return false;
  }
  /* Flush, not finish: a band queues several stages, and waiting after each one would serialise the device for
   * the sake of a nicer message. What that costs is attribution — a fault on the device is noticed by the next
   * call, so the reason string names a readback rather than the kernel that caused it. @ref sync is the way a
   * caller gets the attribution back, and @c tests/gpu_parity.cpp uses it for exactly that. Either way the
   * result is the same: the stage returns false and the band is rendered on the CPU. */
  if (m_api->flush(m_queue) != kCL_SUCCESS) {
    why = std::string("clFlush after ") + name + " — CPU path";
    return false;
  }
  return true;
}

/** @brief One synchronisation point, on request.
 *
 * @ref launch flushes but does not wait, because a band that runs on the device has several stages queued and
 * waiting after each one is a serialisation point in the middle of the work. The price of that is that a
 * device-side fault is *noticed* by the next call rather than by the stage that caused it — which still ends
 * as a CPU render, but names the readback in the reason rather than the kernel. This function is the way a
 * caller asks for the attribution instead: @c tests/gpu_parity.cpp calls it between stages precisely so that a
 * failing comparison can say which stage's kernels faulted. Nothing in a render needs it, and calling it is
 * not a state change — the queue is drained either way. */
bool Session::sync(std::string &why)
{
  if (!m_api || !m_queue) {
    why = "there is no queue to synchronise — CPU path";
    return false;
  }
  if (m_api->flush(m_queue) != kCL_SUCCESS) {
    why = "clFlush — CPU path";
    return false;
  }
  if (m_api->finish(m_queue) != kCL_SUCCESS) {
    why = "clFinish: a kernel this session queued faulted on the device — CPU path";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// The band: start, one visit, finish
// ---------------------------------------------------------------------------

/** @brief Hand the device this band's inputs.
 *
 * Everything here is a copy of something the CPU path already has, which is the whole design of the GPU
 * half: @ref LayeredRender::prepare has built the CoC map, allocated and zeroed the accumulator, and
 * decided the level order; the device is given *those* numbers and the pixels they read, and never derives
 * a schedule, a lattice or a reach of its own (gpu.h). What @ref start spends is one upload per band; what
 * it saves is @ref visit running the two heavy stages on the card.
 *
 * The order is the order the failures are reported in: a machine with no ICD, then one with no GPU, then a
 * device whose ceiling this band does not fit under. Each says which of those it is, and each leaves the
 * renderer exactly as @ref prepare left it, because nothing in a Session is handed back to the renderer
 * before @ref finish. */
bool Session::start(LayeredRender &rend, std::string &why)
{
  m_prog = program(); // the process-wide context and program, or 0 with the reason in the cache
  if (!m_prog) {
    why = probeMessage();
    if (why.empty())
      why = "this machine has no OpenCL device this renderer can use — CPU path";
    return false;
  }
  m_api = m_prog->api;
  m_queue = cache().queue;
  m_rend = &rend;

  m_cap = maxAllocation();
  if (m_cap == 0) {
    why = "the device reports no allocation this band could fit in — CPU path";
    return false;
  }

  /* The level set's radius table, because the level assignment is computed *inside* the kernels: a pixel's
   * CoC is bracketed against these radii (with the dither, as zd.h and layered.cpp do it) exactly as the
   * CPU's extractRows brackets it. They are the ascending, *uncapped* radii — spriteRadiusCap changes which
   * kernel a level is built for, not which level a pixel joins (D-45) — and a handful of doubles. */
  const LevelSet &levels = rend.levelSet();
  m_numLevels = levels.count();
  if (m_numLevels <= 0 || levels.radius.empty()) {
    why = "the renderer has no levels to bracket against — CPU path";
    return false;
  }
  const size_t levelsBytes = (size_t)m_numLevels * sizeof(double);
  if (!budget(levelsBytes, "the level set's radius table", why))
    return false;
  m_levelsBuf = allocBuffer(*m_api, cache().ctx, levelsBytes, kCL_MEM_READ_ONLY, "level radii", why);
  if (!m_levelsBuf) {
    m_used -= levelsBytes;
    return false;
  }
  m_levelsBytes = levelsBytes;
  if (!writeBuf(*m_api, m_queue, m_levelsBuf, 0, levelsBytes, &levels.radius[0], "level radii", why))
    return false;

  if (!ensureWindow(rend, why))
    return false;

  /* The accumulator, uploaded from the CPU's zeroed pair. A visit's pixels are independent of each other
   * and a visit's composite only ever *adds* into the accumulator, so the device can hold it for the whole
   * band and hand it back once, which is the difference between one round trip per band and one per level.
   * The size audit is not paranoia: the kernels address it from rect().x1/y1 with rect().width() as the
   * pitch, so a buffer that is not the rect's own size is a device writing outside it. */
  const RenderRect &rect = rend.rect();
  m_rectX0 = rect.x1;
  m_rectY0 = rect.y1;
  m_rectW = rect.width();
  const size_t pixels = (size_t)rect.width() * (size_t)rect.height();
  if (rect.empty() || rend.accum().size() != pixels * 4u || rend.accumWeight().size() != pixels * 2u) {
    why = "the accumulator the renderer holds is not the size its own render rect implies — CPU path";
    return false;
  }
  if (!ensureAccum(pixels, why))
    return false;
  if (!writeBuf(*m_api, m_queue, m_acc, 0, m_accBytes, &rend.accum()[0], "accumulator", why))
    return false;
  if (!writeBuf(*m_api, m_queue, m_accW, 0, m_accWBytes, &rend.accumWeight()[0], "accumulator weights", why))
    return false;

  m_band = true;
  return true;
}

/** @brief Extraction: this level's cells, summed out of the window the device already holds.
 *
 * The cell box is the CPU's, computed by the CPU's own function: @ref LayeredRender::cellRowsFor with the
 * halo @ref LayeredRender::extractRows uses (`half + dilate`), the columns `convC0 - half - dilate` to
 * `convC1 + half + dilate`, and the pixel columns clipped to the delivered plane the same way. Those numbers
 * are the launch geometry, so the device writes exactly the cells the CPU stage would have written, into
 * cells addressed by the same absolute lattice (gpu.h's "a device with its own boxes is a device running a
 * different render").
 *
 * The two audits below are what makes that claim checked rather than assumed. The window was clipped to the
 * delivered plane at @ref ensureWindow; if this level's clipped columns reach outside it, or if a row the
 * window cuts out is a row the host *did* deliver, then the device would be summing a different set of
 * pixels than @ref extractRows does — and the answer is the CPU path, not a quieter render. */
bool Session::stageExtract(LayeredRender &rend, int rowStart, int rowEnd, std::string &why)
{
  if (!bandLive(rend, "extraction stage", why))
    return false;
  const LevelView lv = rend.levelView();
  if (!lv.valid) {
    why = "extraction was asked for with no current level — CPU path";
    return false;
  }
  const Plane &img = rend.image();
  const int cell = lv.cell;
  if (!needGrid(lv, why)) // extraction writes this level's grid, so it allocates it
    return false;
  m_convDone = false; // whatever was convolved out of this grid is stale from here on (see stageComposite)

  int cyLo = 0, cyHi = 0;
  rend.cellRowsFor(rowStart, rowEnd, lv.half + lv.dilate, &cyLo, &cyHi);
  if (cyHi < cyLo)
    return true; // extractRows' own early return: this row range needs no cells of this level

  const int cxLo = lv.convC0 - lv.half - lv.dilate, cxHi = lv.convC1 + lv.half + lv.dilate;
  const int nCols = cxHi - cxLo + 1, nRows = cyHi - cyLo + 1;
  const int xLo = std::max(lv.pxX0, img.x1), xHi = std::min(lv.pxX1, img.x2);
  if (nCols <= 0 || xHi <= xLo)
    return true; // the same test extractRows makes, for the same reason

  if (xLo < m_winX0 || xHi > m_winX0 + m_winW) {
    why = "this level reads columns the gather window does not hold — CPU path";
    return false;
  }
  for (int y = cyLo * cell; y <= cyHi * cell + cell - 1; ++y) {
    if (y >= m_winY0 && y < m_winY0 + m_winH)
      continue; // inside the window: the device has this row, delivered or not, and the row flag says which
    if (img.row(y)) {
      why = "this level reads a row the gather window cuts out — CPU path";
      return false;
    }
  }

  /* The knobs extraction reads, resolved exactly as extractRows resolves them: once per stage, never per
   * pixel. The dither scale is the knob's percent of zd.h's full swing, and the bloom pair is the gamma
   * reading the CPU takes (layered.cpp's note on bloom_gamma), so a highlight is boosted by the same factor
   * on both devices before either convolves it. */
  const LayerOptions &opt = rend.layerOptions();
  const bool bloom = opt.bloom && opt.bloomGain != 1.0;
  const double linThreshold = opt.bloomGamma ? std::pow(opt.bloomThreshold, kBloomGamma) : opt.bloomThreshold;
  const double linGain = opt.bloomGamma ? std::pow(opt.bloomGain, 1.0 / kBloomGamma) : opt.bloomGain;
  const double ditherScale = opt.dither / kDitherMax;
  const int sideFilter = lv.side; // eSideAny is -1, which the kernel reads as "both ladders"

  if (cell <= kGpuTile) {
    ArgWriter w(*m_api, m_prog->kExtractDirect, "zd_extract_direct", why);
    w << m_gridA << m_img << m_coc << m_side << m_rowOk << m_levelsBuf << m_numLevels << m_winX0 << m_winY0 << m_winW
      << m_winH << m_imgComp << lv.gX0 << lv.gY0 << lv.gCols << cxLo << cyLo << cell << xLo << xHi << lv.level
      << sideFilter << ditherScale << (bloom ? 1 : 0) << linThreshold << linGain;
    if (!w.ok)
      return false;
    return launch(m_prog->kExtractDirect, "zd_extract_direct", (size_t)nCols, (size_t)nRows, 0, why);
  }

  int tilesX = 1, tilesY = 1;
  tilesFor(cell, &tilesX, &tilesY);
  const size_t slots = (size_t)nCols * (size_t)nRows * (size_t)tilesX * (size_t)tilesY;
  if (!ensurePartials(slots, why))
    return false;

  ArgWriter wp(*m_api, m_prog->kExtractPartial, "zd_extract_partial", why);
  wp << m_part << m_img << m_coc << m_side << m_rowOk << m_levelsBuf << m_numLevels << m_winX0 << m_winY0 << m_winW
     << m_winH << m_imgComp << nCols << cxLo << cyLo << cell << xLo << xHi << tilesX << tilesY << kGpuTile
     << lv.level
     << sideFilter << ditherScale << (bloom ? 1 : 0) << linThreshold << linGain;
  if (!wp.ok)
    return false;
  if (!launch(m_prog->kExtractPartial, "zd_extract_partial", (size_t)nCols * (size_t)nRows, (size_t)tilesX * (size_t)tilesY, 0, why))
    return false;

  ArgWriter wc(*m_api, m_prog->kExtractCells, "zd_extract_cells", why);
  wc << m_gridA << m_part << lv.gX0 << lv.gY0 << lv.gCols << nCols << cxLo << cyLo << cell << tilesX * tilesY;
  if (!wc.ok)
    return false;
  return launch(m_prog->kExtractCells, "zd_extract_cells", (size_t)nCols, (size_t)nRows, 0, why);
}

namespace {

/** @brief Which of a kernel's planes a logical plane reads — @ref LayeredRender's `kernelPlane`, transcribed
 * because that one is file-local to layered.cpp and the convolution kernel needs the same answer. A bokeh
 * picture with fewer than four planes repeats its last one, which is how a single-channel bokeh still fills
 * every plane the grid has (D-37). */
int planeFor(const Kernel &k, int p)
{
  return p < k.planes - 1 ? p : k.planes - 1;
}

} // namespace

/** @brief The convolution: the extracted grid in, the convolved grid out, one cell per work item.
 *
 * The cell box is again the CPU's own — @ref LayeredRender::cellRowsFor with a halo of 0, which is what
 * @ref LayeredRender::blurRows asks for, and the columns are the level's convolved box. The ring the grid
 * already has (@ref LevelView::margin, `dilate + half + 2` cells) is what lets the tap loop read `half` cells
 * outside that box without a bounds test, on the device exactly as on the CPU: outside the box is *no
 * content*, not the nearest value (D-31).
 *
 * The one difference is deliberate and measured rather than assumed: the CPU accumulates its taps in
 * @c float, this kernel accumulates in @c double, so the device's convolved grid is the more accurate of the
 * two by an amount tests/gpu_parity.cpp reports. The *taps*, their order and the all-zero tap skip are the
 * CPU's, line for line, and the identity case — a single tap of 1, which is what the in-focus level and a
 * zero-area bokeh both come down to — copies the cell instead of convolving it, which is bit-identical on
 * both devices. */
bool Session::stageConvolve(LayeredRender &rend, int rowStart, int rowEnd, std::string &why)
{
  if (!bandLive(rend, "convolution stage", why))
    return false;
  const LevelView lv = rend.levelView();
  if (!lv.valid || !lv.kernel) {
    why = "the convolution was asked for with no current level — CPU path";
    return false;
  }
  if (!needGrid(lv, why)) // the convolution reads grid A and writes grid B
    return false;
  if (!uploadKernel(rend, lv, why)) // and it reads this level's taps, so it uploads them
    return false;
  int rLo = 0, rHi = 0;
  rend.cellRowsFor(rowStart, rowEnd, 0, &rLo, &rHi);
  if (rHi < rLo)
    return true; // blurRows' own early return

  const Kernel &k = *lv.kernel;
  int identity = (k.span == 1) ? 1 : 0; // the same two tests blurRows runs
  if (identity) {
    for (int p = 0; p < k.planes; ++p) {
      if (k.plane(p)[0] != 1.0f) {
        identity = 0;
        break;
      }
    }
  }

  const int kp[5] = { planeFor(k, 0), planeFor(k, 1), planeFor(k, 2), planeFor(k, 3), planeFor(k, 4) };
  ArgWriter w(*m_api, m_prog->kConvolve, "zd_convolve", why);
  w << m_gridA << m_gridB << m_taps << k.span << lv.half << identity << lv.gX0 << lv.gY0 << lv.gCols << lv.gRows
    << lv.convC0 << lv.convC1 << rLo << rHi << kp[0] << kp[1] << kp[2] << kp[3] << kp[4];
  if (!w.ok)
    return false;
  if (!launch(m_prog->kConvolve, "zd_convolve", (size_t)(lv.convC1 - lv.convC0 + 1), (size_t)(rHi - rLo + 1), 0, why))
    return false;
  m_convDone = true; // what @ref stageComposite is now allowed to read, in cell rows of this level
  m_convLevel = lv.level;
  m_convSide = lv.side;
  m_convCols = lv.gCols;
  m_convRows = lv.gRows;
  m_convRowLo = rLo;
  m_convRowHi = rHi;
  return true;
}

/** @brief The composite: this level's convolved cells, bilinear-read and blended over whatever the earlier
 * levels left in the device's accumulator.
 *
 * One output pixel per work item, because pixels are independent *within* a visit — only visits are ordered,
 * and their order is the plugin's, one @ref visit at a time. The cell maths, the coverage clamp, the
 * source-over rule and the float stores are @ref LayeredRender::compositeRows' line for line; the accumulator
 * is read as the float it is stored as and computed in double, which is the other difference the parity suite
 * measures.
 *
 * The accumulator is addressed from the band rect, as @ref LayeredRender::accum is, so a level whose box
 * begins inside the rect composites into the same pixels it would on the CPU — including the case the CPU
 * comment warns about, where a rect-wide level makes the offset look zero. */
bool Session::stageComposite(LayeredRender &rend, int rowStart, int rowEnd, std::string &why)
{
  if (!bandLive(rend, "composite stage", why))
    return false;
  const LevelView lv = rend.levelView();
  if (!lv.valid) {
    why = "the composite was asked for with no current level — CPU path";
    return false;
  }
  if (!needGrid(lv, why)) // the composite reads this level's grid B
    return false;
  const RenderRect &rect = rend.rect();
  if (lv.compX0 < rect.x1 || lv.compX1 > rect.x2 || lv.compY0 < rect.y1 || lv.compY1 > rect.y2) {
    why = "this level composites pixels outside the band's accumulator — CPU path";
    return false;
  }
  const int yA = std::max(rowStart, lv.compY0), yB = std::min(rowEnd, lv.compY1);
  if (yB <= yA || lv.compX1 <= lv.compX0)
    return true; // compositeRows' own early return

  /* Grid B has no clearing pass, and that is a decision rather than an oversight: the convolution writes
   * every cell of the box it convolves — a zero first, then the taps — and the composite reads nothing
   * outside that box. On the CPU a read outside it would land on a cell @ref LayeredRender::beginLevel
   * zeroed; on the device it would land on whatever the previous level left in the reused buffer, which is
   * a different picture. The invariant that keeps the two readings the same is that this composite's
   * bilinear cells are inside the cells the convolution of the *same row range* writes — the comp box sits
   * inside the convolved box widened by @ref LevelView::pad, and that pad *is* the bilinear read-back. It is
   * checked here instead of assumed, because a stage API can be driven in an order @ref visit is not, and
   * the cost of being wrong here is a render built on stale cells rather than a failure. */
  int convLo = 1, convHi = 0;
  rend.cellRowsFor(rowStart, rowEnd, 0, &convLo, &convHi); // exactly what @ref stageConvolve computed
  const long long den = 2LL * (long long)lv.cell;
  const long long rA = floorDivLL(2LL * (long long)yA + 1 - (long long)lv.cell, den);
  const long long rB = floorDivLL(2LL * (long long)(yB - 1) + 1 - (long long)lv.cell, den);
  const long long cA = floorDivLL(2LL * (long long)lv.compX0 + 1 - (long long)lv.cell, den);
  const long long cB = floorDivLL(2LL * (long long)(lv.compX1 - 1) + 1 - (long long)lv.cell, den);
  if (convHi < convLo || rA < (long long)convLo || rB + 1 > (long long)convHi || cA < (long long)lv.convC0 ||
      cB + 1 > (long long)lv.convC1) {
    why = "this composite reads convolved cells its own convolution does not write — CPU path";
    return false;
  }
  /* The same invariant as a fact rather than as a formula: @ref stageConvolve records the cell rows it
   * convolved, and @ref stageExtract and @ref uploadGridA invalidate that record, so a composite asked for on
   * rows this level never convolved — or on a level other than the one that was convolved — refuses here
   * instead of blending the previous level's leftover cells into the accumulator. A whole-band render never
   * hits it (@ref visit convolves and composites the same rows), but a stage API that can be driven one stage
   * at a time is a stage API that can be driven in the wrong order, and this is the check that makes the
   * wrong order a fallback rather than a picture. */
  if (!m_convDone || m_convLevel != lv.level || m_convSide != lv.side || m_convCols != lv.gCols || m_convRows != lv.gRows ||
      convLo < m_convRowLo || convHi > m_convRowHi) {
    why = "this composite reads cells this level's convolution has not written — CPU path";
    return false;
  }

  ArgWriter w(*m_api, m_prog->kComposite, "zd_composite", why);
  w << m_gridB << m_acc << m_accW << lv.gX0 << lv.gY0 << lv.gCols << lv.gRows << lv.cell << lv.compX0 << lv.compX1
    << yA << yB << m_rectX0 << m_rectY0 << m_rectW;
  if (!w.ok)
    return false;
  return launch(m_prog->kComposite, "zd_composite", (size_t)(lv.compX1 - lv.compX0), (size_t)(yB - yA), 0, why);
}

// ---------------------------------------------------------------------------
// The grid round trip, and the whole band
// ---------------------------------------------------------------------------

/** @brief Device grid A -> the CPU's extracted grid, so @ref LayeredRender::fillLevel can run over it.
 *
 * The pitch is this level's own @ref LevelView, never the pitch of the buffer that happens to be allocated:
 * the grids grow only, so a level with a smaller grid reuses a bigger buffer, and the only thing that makes
 * that slack invisible is that every read and write of it is told the current level's `gX0`/`gY0`/`gCols`.
 * The transfer covers exactly the cells this level's grid has, which is what the kernels touched. */
bool Session::downloadGridA(LayeredRender &rend, std::string &why)
{
  if (!bandLive(rend, "grid readback", why))
    return false;
  const LevelView lv = rend.levelView();
  if (!lv.valid) {
    why = "a grid readback was asked for with no current level — CPU path";
    return false;
  }
  if (!needGrid(lv, why))
    return false;
  const size_t cells = (size_t)lv.gCols * (size_t)lv.gRows;
  if (cells > m_gridCells || rend.gridA().size() != cells * 5u) {
    why = "the device grid and the CPU grid this level has are not the same size — CPU path";
    return false;
  }
  return readBuf(*m_api, m_queue, m_gridA, 0, cells * 5u * sizeof(float), &rend.gridA()[0], "extracted grid", why);
}

/** @brief The CPU's extracted grid -> device grid A.
 *
 * Called twice per visit: once straight after @ref LayeredRender::beginLevel, which hands a grid of exact
 * zeros, and once after @ref LayeredRender::fillLevel. The first one is what the extraction kernels' "an
 * all-zero cell is already zero" skip depends on — a cell they leave alone has to be *zero*, not what the
 * previous level left in that slot — which is why there is no clear kernel anywhere in this file. */
bool Session::uploadGridA(LayeredRender &rend, std::string &why)
{
  if (!bandLive(rend, "grid upload", why))
    return false;
  const LevelView lv = rend.levelView();
  if (!lv.valid) {
    why = "a grid upload was asked for with no current level — CPU path";
    return false;
  }
  if (!needGrid(lv, why))
    return false;
  const size_t cells = (size_t)lv.gCols * (size_t)lv.gRows;
  if (cells > m_gridCells || rend.gridA().size() != cells * 5u) {
    why = "the device grid and the CPU grid this level has are not the same size — CPU path";
    return false;
  }
  m_convDone = false; // a grid upload means this level's convolved cells are about to be written anew
  return writeBuf(*m_api, m_queue, m_gridA, 0, cells * 5u * sizeof(float), &rend.gridA()[0], "extracted grid", why);
}

/** @brief Device grid B -> a caller's buffer. Nothing in a render needs this — the composite reads grid B on
 * the device — so it exists for @c tests/gpu_parity.cpp, which compares one stage's output against the CPU's
 * same stage at a time rather than only comparing final pictures. */
bool Session::downloadGridB(LayeredRender &rend, std::vector<float> &out, std::string &why)
{
  if (!bandLive(rend, "grid readback", why))
    return false;
  const LevelView lv = rend.levelView();
  if (!lv.valid) {
    why = "a grid readback was asked for with no current level — CPU path";
    return false;
  }
  if (!needGrid(lv, why))
    return false;
  const size_t cells = (size_t)lv.gCols * (size_t)lv.gRows;
  if (cells > m_gridCells) {
    why = "the device grid is smaller than this level's grid — CPU path";
    return false;
  }
  out.assign(cells * 5u, 0.0f);
  if (cells == 0)
    return true;
  return readBuf(*m_api, m_queue, m_gridB, 0, cells * 5u * sizeof(float), &out[0], "convolved grid", why);
}

/** @brief The device's accumulator -> the CPU's, once per band.
 *
 * Read into a staging pair and swapped in only on success: a read that failed partway would otherwise leave
 * the CPU's accumulator holding whatever arrived, and the CPU re-run of this band adds its composites on top
 * of those values instead of on top of the zeros @ref LayeredRender::prepare wrote. Swapping costs nothing and
 * keeps the CPU's own addressing intact. */
bool Session::downloadAccum(LayeredRender &rend, std::string &why)
{
  if (!bandLive(rend, "accumulator readback", why))
    return false;
  if (!m_acc || !m_accW) {
    why = "the band has no device accumulator to hand back — CPU path";
    return false;
  }
  std::vector<float> acc(m_accBytes / sizeof(float), 0.0f), accW(m_accWBytes / sizeof(float), 0.0f);
  if (!readBuf(*m_api, m_queue, m_acc, 0, m_accBytes, &acc[0], "accumulator", why))
    return false;
  if (!readBuf(*m_api, m_queue, m_accW, 0, m_accWBytes, &accW[0], "accumulator weights", why))
    return false;
  rend.accum().swap(acc);
  rend.accumWeight().swap(accW);
  return true;
}

/** @brief One whole level visit: the CPU's geometry, the device's two heavy stages, the CPU's max-filter.
 *
 * The order is the plugin's band loop's order, and so are the row ranges — the whole band on both sides —
 * because @ref LayeredRender::extractRows and @ref LayeredRender::blurRows are called with the band's rows by
 * the plugin itself. What is not the same is *where* the work happens, which is the only thing this file
 * changes (gpu.h). */
bool Session::visit(const LevelVisit &v, std::string &why)
{
  if (!m_band || !m_rend) {
    why = "a visit was asked for on a session that never started — CPU path";
    return false;
  }
  LayeredRender &rend = *m_rend;
  if (!rend.beginLevel(v))
    return true; // the plan holds no box for this visit: the CPU path's `continue`, for the same test
  const LevelView lv = rend.levelView();
  if (!lv.valid) {
    why = "beginLevel accepted a visit and then reported no level — CPU path";
    return false;
  }
  if (!needGrid(lv, why))
    return false;
  if (!uploadGridA(rend, why)) // beginLevel's grid of zeros, which the extraction's skip depends on
    return false;

  const int rowStart = rend.rect().y1, rowEnd = rend.rect().y2;
  if (!stageExtract(rend, rowStart, rowEnd, why))
    return false;
  if (!downloadGridA(rend, why))
    return false;
  rend.fillLevel(); // stays on the CPU grid, which is what the round trip above is for
  if (!uploadGridA(rend, why))
    return false;
  if (!stageConvolve(rend, rowStart, rowEnd, why))
    return false;
  if (!stageComposite(rend, rowStart, rowEnd, why))
    return false;
  rend.finishLevel();
  return true;
}

/** @brief Hand the band's composite back. After this, @ref LayeredRender::writeOutput runs on the CPU exactly
 * as it does when the checkbox is off — the premultiplication, the mask and mix dissolve and the row emission
 * were never the GPU's to begin with. */
bool Session::finish(std::string &why)
{
  if (!m_band || !m_rend) {
    why = "a session was finished without being started — CPU path";
    return false;
  }
  if (!downloadAccum(*m_rend, why))
    return false;
  m_band = false;
  return true;
}

/** @brief The whole GPU half of one band.
 *
 * Resolves the program first — that is the part worth having failed *before* a band commits to a device, and
 * the reason a machine with no ICD says so once rather than once per band — and then takes the band lock for
 * everything that uses the shared queue. GPU renders are serialised across host threads on purpose: a Natron
 * render can have several threads asking for bands of the same frame, and the alternative to a band lock is
 * one queue with two bands' kernels and buffers interleaved on it.
 *
 * The lock order is the one worth keeping straight: @ref program and @ref maxAllocation take the cache's own
 * mutex *inside* their calls and release it before returning, and no thread holds that mutex while waiting
 * for the band lock (this function takes the band lock only after @ref program has returned). So a Session
 * taking the cache mutex inside @ref start, while holding the band lock, cannot close a cycle.
 *
 * A false return means the band is untouched: nothing in a Session is written back to the renderer before
 * @ref finish, so the plugin's CPU loop starts from the same zeroed accumulator @ref LayeredRender::prepare
 * left behind. The renderer's own level state may be mid-visit, which is what @ref LayeredRender::beginLevel
 * resetting it on every visit is for. */
bool renderBand(LayeredRender &rend, std::string &why)
{
  Program *prog = program();
  if (!prog) {
    why = probeMessage();
    if (why.empty())
      why = "this machine has no OpenCL device this renderer can use — CPU path";
    return false;
  }

  std::lock_guard<std::mutex> band(cache().bandMutex);
  Session session;
  if (!session.start(rend, why))
    return false;
  const std::vector<LevelVisit> &order = rend.levelOrder();
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (!session.visit(order[i], why))
      return false; // the whole band goes back to the CPU, not the half of it that failed
  }
  return session.finish(why);
}

} // namespace gpu
} // namespace zd
