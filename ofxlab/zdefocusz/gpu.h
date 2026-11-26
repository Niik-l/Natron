// ZDefocusZ: the GPU path — the same render, executed on a compute device (DECISIONS.md D-51).
//
// OFX-free like zd.h / bokeh.h / layered.h (D-34), for the same reason it gives: what this file
// promises is that a stage run here *is* the stage the CPU runs, and that promise is checked
// headlessly — tests/gpu_parity.cpp against the CPU stages themselves and the cost ladder's own
// stage timings, tests/gpu_probe.sh against a real <CL/cl.h> — before a host is asked to tick a box.
//
// ---------------------------------------------------------------------------
// Route: OpenCL, taken from the ICD the host has already loaded
// ---------------------------------------------------------------------------
// The plugin links nothing new and vendors nothing. It `dlopen`s `libOpenCL.so.1` — the ICD loader
// any host with GPU support has already loaded — and `dlsym`s the entry points it calls, with the
// types and enum values of the OpenCL host ABI declared below. That is what rule 2 (the vendored SDK
// is the only dependency) forces: an OpenCL header would be a new vendored tree, and linking
// `-lOpenCL` would turn the bundle into a *load* failure on a machine with no ICD, where the right
// answer is the CPU path and not a plugin that will not open. `-ldl` is the only addition to the
// link line, and on Linux it is part of libc.
//
// Why not the OFX 1.4 GL render suite, which a host genuinely does own: it forks image delivery into
// a GL context the host owns, so every premult rule, mask dissolve and RoI promise this plugin has
// would have to be re-made inside that context; it is opted into at describe time, so it would change
// what every host sees of the node for a feature that has to stay a checkbox; and Natron's GL floor is
// 2.0 with its own separate GL image path behind it. A plugin that keeps its own buffers keeps the
// renderer it already has — which is why the renderer is one class with typed stages and not a shader.
//
// ---------------------------------------------------------------------------
// What the GPU changes, and everything it does not
// ---------------------------------------------------------------------------
// Only *which device executes a stage*. The plan, the level set, the lattices, the visit order, the
// RoI/RoD padding and the band splitting stay LayeredRender's, because they are the part of a render a
// host measures (D-31's window-independence, D-38's padding, D-40's memory) and a device must not be
// allowed to disagree about them. Three stages run on the device: extraction, convolution, composite.
// Three do not:
//
//   prepare()     it owns the plan, and every number a device kernel is launched with comes from it.
//   fillLevel()   the fill costs 3.7 % of a CPU render at the finest lattice the cost ladder prices
//                 (section F) and is the one stage whose exact semantics — three passes borrowing gB,
//                 a centred max over four direction families, a write box cut to the delivered plate
//                 (D-47, D-48) — would have to be re-derived on a device to be worth moving. So the
//                 extracted grid round-trips device → CPU → device between extraction and blur, and the
//                 fill stays where its tests already pin it.
//   writeOutput() plus the previews and the mask/mix dissolve: they touch the host's plane, one row at
//                 a time, and the plugin's own emitRow is the only place its premult rules live.
//
// Per band the device is handed the picture and the CoC map **once** — the window every level of this
// band gathers from, clipped to what the host delivered — and gets back the accumulator once, after the
// last visit. The per-level transfers are the grids, which are cell-reduced. Re-uploading a window per
// level costs more than the stages save, and the parity suite's timings are what says so.
//
// ---------------------------------------------------------------------------
// The numerical promise
// ---------------------------------------------------------------------------
// The device accumulates in 64-bit floats where the CPU accumulates a row of 32-bit ones (blurRows),
// and it sums a cell's pixels in a different grouping (extraction tiles a cell; the CPU walks a cell
// row at a time). So the two paths are *not* bit-identical, and are not meant to be: the tap order and
// the tap-skip rule are the CPU's, the bilinear weights and the over-blend are computed in double the
// CPU's way, the accumulator stays 32-bit storage exactly as D-49 requires, and everything the two
// paths disagree about is an accumulation-order difference at the last digits of a float32 result.
// tests/gpu_parity.cpp measures that difference against the CPU's own error budget rather than
// assuming it, and re-asserts the structural checks (D-47's silhouette, D-48/D-49's bound on what a
// fill may write) on the GPU's plate. A device with no 64-bit float support is refused (the kernels
// ask for it), because a second precision regime is a second set of tolerances to test.

#ifndef OFXLAB_ZDEFOCUSZ_GPU_H
#define OFXLAB_ZDEFOCUSZ_GPU_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "layered.h"

namespace zd {
namespace gpu {

// ---------------------------------------------------------------------------
// The OpenCL host ABI, self-declared
// ---------------------------------------------------------------------------
// The OpenCL 1.2 host ABI as <CL/cl.h> defines it, with the values tests/gpu_probe.sh re-derives from
// a real copy of that header when the machine has one — which is what keeps a self-declared ABI honest.
// No shipped object includes a Khronos header (rule 2), and nothing here is part of any bundle's
// dependencies.

typedef int cl_int;
typedef unsigned int cl_uint;
typedef uint64_t cl_ulong;   // cl.h: cl_ulong is a 64-bit unsigned, so every cl_bitfield is too
typedef intptr_t cl_context_properties;
typedef cl_uint cl_bool;     // cl.h: cl_bool is a cl_uint, *not* the size of a kernel's bool
typedef cl_ulong cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_bitfield cl_device_fp_config;
typedef cl_bitfield cl_mem_flags;
typedef cl_bitfield cl_command_queue_properties;

typedef struct _cl_platform_id *cl_platform_id;
typedef struct _cl_device_id *cl_device_id;
typedef struct _cl_context *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_mem *cl_mem;
typedef struct _cl_program *cl_program;
typedef struct _cl_kernel *cl_kernel;
typedef struct _cl_event *cl_event; // this plugin never uses events, but the enqueue prototypes below name
                                                          // them, and naming them the way <CL/cl.h> does is what
                                                          // lets tests/gpu_probe.cpp compare the two prototypes

// The subset of the enumerations this file passes. Values from <CL/cl.h>.
const cl_device_type kCL_DEVICE_TYPE_GPU = (cl_device_type)(1 << 2); // 0x4

const cl_uint kCL_DEVICE_MAX_COMPUTE_UNITS = 0x1002;
const cl_uint kCL_DEVICE_MAX_WORK_ITEM_DIMENSIONS = 0x1003;
const cl_uint kCL_DEVICE_MAX_WORK_GROUP_SIZE = 0x1004;
const cl_uint kCL_DEVICE_MAX_WORK_ITEM_SIZES = 0x1005; // an array query, sized by the dimension count
const cl_uint kCL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT = 0x100A;
const cl_uint kCL_DEVICE_MAX_MEM_ALLOC_SIZE = 0x1010;
const cl_uint kCL_DEVICE_GLOBAL_MEM_SIZE = 0x101F;
const cl_uint kCL_DEVICE_LOCAL_MEM_SIZE = 0x1023;
const cl_uint kCL_DEVICE_AVAILABLE = 0x1027;
const cl_uint kCL_DEVICE_NAME = 0x102B;
const cl_uint kCL_DEVICE_VENDOR = 0x102C;
const cl_uint kCL_DRIVER_VERSION = 0x102D; // spelled CL_DRIVER_VERSION in <CL/cl.h>, even though it is a *device* info parameter
const cl_uint kCL_DEVICE_PROFILE = 0x102E;
const cl_uint kCL_DEVICE_VERSION = 0x102F;
const cl_uint kCL_DEVICE_DOUBLE_FP_CONFIG = 0x1032;

const cl_context_properties kCL_CONTEXT_PLATFORM = 0x1084;

const cl_mem_flags kCL_MEM_READ_WRITE = (cl_mem_flags)(1 << 0);
const cl_mem_flags kCL_MEM_READ_ONLY = (cl_mem_flags)(1 << 2);
const cl_mem_flags kCL_MEM_COPY_HOST_PTR = (cl_mem_flags)(1 << 5);

const cl_uint kCL_PROGRAM_BUILD_LOG = 0x1183;

const cl_int kCL_SUCCESS = 0;

/** @brief The entry points this file calls, in the shape cl_function_types.h gives them (CL_API_CALL
 * and CL_CALLBACK are both empty on Linux, so these are plain function pointers). Every one is taken
 * from the ICD loader by name; a loader that does not export one is reported, and the render takes the
 * CPU path. */
typedef cl_int (*FnGetPlatformIDs)(cl_uint num_entries, cl_platform_id *platforms, cl_uint *num_platforms);
typedef cl_int (*FnGetDeviceIDs)(cl_platform_id platform, cl_device_type device_type, cl_uint num_entries, cl_device_id *devices,
                                 cl_uint *num_devices);
typedef cl_int (*FnGetDeviceInfo)(cl_device_id device, cl_uint param_name, size_t param_value_size, void *param_value,
                                  size_t *param_value_size_ret);
typedef cl_context (*FnCreateContext)(const cl_context_properties *properties, cl_uint num_devices, const cl_device_id *devices,
                                      void (*pfn_notify)(const char *ecp, const void *private_info, size_t cb, void *user_data),
                                      void *user_data, cl_int *errcode_ret);
typedef cl_command_queue (*FnCreateCommandQueue)(cl_context context, cl_device_id device, cl_command_queue_properties properties,
                                                  cl_int *errcode_ret);
typedef cl_program (*FnCreateProgramWithSource)(cl_context context, cl_uint count, const char **strings, const size_t *lengths,
                                                 cl_int *errcode_ret);
typedef cl_int (*FnBuildProgram)(cl_program program, cl_uint num_devices, const cl_device_id *device_list, const char *options,
                                 void (*pfn_notify)(cl_program program, void *user_data), void *user_data);
typedef cl_int (*FnGetProgramBuildInfo)(cl_program program, cl_device_id device, cl_uint param_name, size_t param_value_size,
                                        void *param_value, size_t *param_value_size_ret);
typedef cl_kernel (*FnCreateKernel)(cl_program program, const char *kernel_name, cl_int *errcode_ret);
typedef cl_int (*FnSetKernelArg)(cl_kernel kernel, cl_uint arg_index, size_t arg_size, const void *arg_value);
typedef cl_int (*FnEnqueueNDRangeKernel)(cl_command_queue queue, cl_kernel kernel, cl_uint work_dim, const size_t *global_work_offset,
                                         const size_t *global_work_size, const size_t *local_work_size, cl_uint num_events_in_wait_list,
                                         const cl_event *event_wait_list, cl_event *event);
typedef cl_mem (*FnCreateBuffer)(cl_context context, cl_mem_flags flags, size_t size, void *host_ptr, cl_int *errcode_ret);
typedef cl_int (*FnEnqueueWriteBuffer)(cl_command_queue queue, cl_mem buffer, cl_bool blocking, size_t offset, size_t size,
                                       const void *ptr, cl_uint num_events_in_wait_list, const cl_event *event_wait_list, cl_event *event);
typedef cl_int (*FnEnqueueReadBuffer)(cl_command_queue queue, cl_mem buffer, cl_bool blocking, size_t offset, size_t size, void *ptr,
                                      cl_uint num_events_in_wait_list, const cl_event *event_wait_list, cl_event *event);
typedef cl_int (*FnReleaseMemObject)(cl_mem mem);
typedef cl_int (*FnReleaseKernel)(cl_kernel kernel);
typedef cl_int (*FnReleaseProgram)(cl_program program);
typedef cl_int (*FnReleaseCommandQueue)(cl_command_queue queue);
typedef cl_int (*FnReleaseContext)(cl_context context);
typedef cl_int (*FnFlush)(cl_command_queue queue);
typedef cl_int (*FnFinish)(cl_command_queue queue);

/** @brief The loader, once per process: the dlopen'd ICD and the entry points it exported. Held behind
 * the process-wide cache in gpu.cpp, which is also what makes it safe when a host renders tiles from
 * several threads (D-51: the one deliberate exception to "no shared mutable state", and the mutex is
 * what the exception buys). */
struct Loader {
  FnGetPlatformIDs getPlatformIDs;
  FnGetDeviceIDs getDeviceIDs;
  FnGetDeviceInfo getDeviceInfo;
  FnCreateContext createContext;
  FnCreateCommandQueue createCommandQueue;
  FnCreateProgramWithSource createProgramWithSource;
  FnBuildProgram buildProgram;
  FnGetProgramBuildInfo getProgramBuildInfo;
  FnCreateKernel createKernel;
  FnSetKernelArg setKernelArg;
  FnEnqueueNDRangeKernel enqueueNDRangeKernel;
  FnCreateBuffer createBuffer;
  FnEnqueueWriteBuffer enqueueWriteBuffer;
  FnEnqueueReadBuffer enqueueReadBuffer;
  FnReleaseMemObject releaseMemObject;
  FnReleaseKernel releaseKernel;
  FnReleaseProgram releaseProgram;
  FnReleaseCommandQueue releaseCommandQueue;
  FnReleaseContext releaseContext;
  FnFlush flush;
  FnFinish finish;

  std::string library; // what was opened, for the log
  std::string missing; // the first entry point it did not export

  Loader()
      : getPlatformIDs(0), getDeviceIDs(0), getDeviceInfo(0), createContext(0), createCommandQueue(0), createProgramWithSource(0),
        buildProgram(0), getProgramBuildInfo(0), createKernel(0), setKernelArg(0), enqueueNDRangeKernel(0), createBuffer(0),
        enqueueWriteBuffer(0), enqueueReadBuffer(0), releaseMemObject(0), releaseKernel(0), releaseProgram(0), releaseCommandQueue(0),
        releaseContext(0), flush(0), finish(0)
  {
  }

  bool complete() const; // every entry point present
};

/** @brief What a device said about itself, read once and reported: a plugin that silently picks a
 * device is a plugin a user cannot debug, so every number here is a line in the host's log, and
 * @ref maxAllocation is the ceiling the plugin's own memory estimate is clamped to (D-51, and the
 * reason D-40's band splitting matters on a GPU too — a level's grid is an allocation the device
 * has to survive). */
struct DeviceInfo {
  bool ok;
  std::string name, vendor, profile, version, driver;
  cl_ulong maxComputeUnits;
  size_t maxWorkGroupSize;
  size_t maxWorkItem[3]; // CL_DEVICE_MAX_WORK_ITEM_SIZES, the first three dimensions, read through the
                         // dimension count @c CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS so the query is not sized
                         // past what the device wrote. Reported for diagnosis: every launch gpu.cpp makes is
                         // two-dimensional with @c local_work_size = NULL, so the device chooses its own work
                         // groups and these are the limits it chose within.
  cl_ulong maxAllocation; // CL_DEVICE_MAX_MEM_ALLOC_SIZE
  cl_ulong globalMemory;
  cl_ulong localMemory;   // what one kernel's `__local` scratch may hold
  cl_uint vectorWidthFloat;
  bool doubleSupport; // cl_khr_fp64: the kernels accumulate in double, so a device without it is refused

  DeviceInfo()
      : ok(false), maxComputeUnits(0), maxWorkGroupSize(0), maxAllocation(0), globalMemory(0), localMemory(0), vectorWidthFloat(0),
        doubleSupport(false)
  {
    maxWorkItem[0] = maxWorkItem[1] = maxWorkItem[2] = 0;
  }
};

/** @brief Human-readable reason the device was refused, for the host log. Never an exception: an
 * unavailable device is the CPU path, and a render must not fail because a checkbox could not be
 * honoured (that is why `useGPU` is allowed to be ignored, and why it is the *host log* that says so). */
const std::string &probeMessage();

/** @brief The device this process renders on, or a DeviceInfo with `ok == false`. Probed once, on the
 * first render that asks for it, under the same mutex as the queue cache. */
const DeviceInfo &device();

/** @brief True when a render on this device is possible at all: loader complete, a GPU device present,
 * it is available, and it has 64-bit floats. */
bool available();

/** @brief Bytes a single device allocation may take, derived from the device's own ceiling: the smaller
 * of its max allocation and half its global memory (the other half is the host's driver, the display
 * and whatever else is on that card). The plugin clamps its memory budget to this when the checkbox is
 * on, so the band splitting D-40 does is what keeps a GPU render off an allocation failure rather than
 * a kOfxStatErrMemory deep inside a stage. Returns 0 when there is no device. */
size_t maxAllocation();

// ---------------------------------------------------------------------------
// One band's device state
// ---------------------------------------------------------------------------
/** @brief How wide one extraction tile is, in pixels. A cell no wider than this is summed by a single
 * work item, walking its pixels in the CPU's own order, so that path is bit-identical to
 * @ref LayeredRender::extractRows. A wider cell is split into `ceil(cell/kGpuTile)` by
 * `ceil(cell/kGpuTile)` square tiles (the last row and column of tiles are narrower, so the tiles cover
 * the cell exactly), each writing 5 doubles into a slot the combiner then adds in tile order. Both
 * numbers are passed to the kernel rather than baked into it, so the constant exists once. The partial
 * buffer costs `5 doubles × tiles per cell`, i.e. about 40 bytes per 256 gathered pixels, whatever the
 * cell is, so tiling is not what a memory estimate is about. Tiles rather than whole cells, or rather
 * than strips of a cell, because of the device's own geometry: a 585 px cell is 37×37 = 1369 work items
 * rather than 37 items each walking 9360 pixels serially, and 37 items on a device with 84 compute units
 * is a serial CPU loop in a GPU register rather than a wavefront. A device's geometry, not a knob: it is
 * not in DESIGN.md §2, and changing it changes only the grouping of the sums, which is the numerical
 * promise, not the render. */
const int kGpuTile = 16;

/** @brief The compiled program and the kernels it exports: the expensive part of the path, so it is
 * built once per process and borrowed by every Session. Built lazily, under the same mutex as the
 * context and queue, from @ref kernelSource. @ref log holds the compiler's own report when the build
 * failed — which is the only way a user can be told *why* the checkbox did nothing. */
struct Program {
  Loader *api;
  cl_program program;
  cl_kernel kExtractDirect, kExtractPartial, kExtractCells, kConvolve, kComposite;
  std::string log;
  bool ok;

  Program()
      : api(0), program(0), kExtractDirect(0), kExtractPartial(0), kExtractCells(0), kConvolve(0), kComposite(0), ok(false)
  {
  }
};

/** @brief The process-wide program, built on first use. Returns 0 (with the reason in
 * @ref probeMessage) when the source would not build or a kernel is missing, which sends the render to
 * the CPU path. */
Program *program();

/** @brief The methods the GPU path is made of, one per CPU stage, so that a stage run on a device and
 * the same stage run in LayeredRender can be compared *individually* — which is what
 * tests/gpu_parity.cpp does, and what makes a difference a stage's fault rather than a mystery.
 *
 * A Session owns this band's buffers and nothing else: the context, queue and compiled program are
 * process-wide (they are the expensive part, and a plugin instance per host graph is not a reason to
 * compile 40 KB of source again). @ref start hands the device the two windows the band reads — the
 * picture and the CoC map, each covering the area every level of this band gathers from, clipped to
 * what the host delivered — plus the side plane and the one byte per row that stands for the scanlines
 * the host did or did not deliver, and the accumulator, which the CPU's @ref LayeredRender::prepare has
 * already zeroed. Then one @ref visit per entry of @ref LayeredRender::levelOrder, in that order, and
 * @ref finish hands the accumulator back.
 *
 * Every method returns false with a reason in @p why on the first failure, and a session that has
 * failed is not used again for that band: the caller re-runs the band on the CPU (see
 * @ref renderBand), which is why a GPU failure can never be a render failure. */
class Session {
public:
  Session();
  ~Session(); // releases this band's buffers; the queue and program stay for the next render

  bool start(LayeredRender &rend, std::string &why);

  /** @brief One whole level visit, mirroring what the plugin's band loop does for it:
   * @ref LayeredRender::beginLevel, extraction, (@ref LayeredRender::fillLevel on the CPU grid, through
   * the round trip), the convolution, the composite, @ref LayeredRender::finishLevel. Returns true when
   * the visit had no content anywhere — the same @p beginLevel false sends the CPU path past. */
  bool visit(const LevelVisit &v, std::string &why);
  bool finish(std::string &why);

  // The stages, individually, with the row ranges the CPU stages take. Each takes the *current* level of
  // @p rend (@ref LayeredRender::levelView is its geometry), each allocates what it reads or writes (the
  // grids, this level's taps), and each leaves the device holding what the CPU stage of the same name
  // leaves in the corresponding buffer, so a stage's output can be read back and compared one stage at a
  // time (tests/gpu_parity.cpp). That last part is not a convenience for the suite: a stage that assumed a
  // resource another call had prepared would launch a kernel against a null buffer, and on a device a null
  // buffer is a fault rather than an error code.
  bool stageExtract(LayeredRender &rend, int rowStart, int rowEnd, std::string &why);   // -> device grid A
  bool stageConvolve(LayeredRender &rend, int rowStart, int rowEnd, std::string &why);  // device A -> device B
  bool stageComposite(LayeredRender &rend, int rowStart, int rowEnd, std::string &why); // device B + acc -> acc

  // The grid round trip that lets fillLevel() stay on the CPU grid (see the note at the top of this
  // file): download the extracted field, run the CPU pass over it, upload the field the pass wrote.
  bool downloadGridA(LayeredRender &rend, std::string &why);
  bool uploadGridA(LayeredRender &rend, std::string &why);
  bool downloadGridB(LayeredRender &rend, std::vector<float> &out, std::string &why); // the parity suite's readback
  bool downloadAccum(LayeredRender &rend, std::string &why);

  /** @brief Upload @p lv's kernel taps, after checking that @p lv *is* the renderer's current level.
   *
   * Public for the same reason the stages are, and this one is the stage the check exists for: a @ref
   * LevelView is a copy of plain integers plus a kernel pointer, so the way to hand a session the geometry of
   * a level it is no longer on is to keep a view across a @ref LayeredRender::beginLevel — which is what a
   * test can do deliberately, and what would otherwise upload one level's taps and launch another level's
   * convolution. @ref visit takes its own view immediately after @c beginLevel, so a whole-band render never
   * reaches this with anything but the current level. */
  bool uploadKernel(LayeredRender &rend, const LevelView &lv, std::string &why);

  /** @brief Drain the queue: flush, then wait for everything this session queued to finish.
   *
   * @ref stageConvolve ensures this level's taps of its own accord, so a whole-band render uploads them once
   * per visit and never calls this. What this is for is attribution: a launch flushes rather than waits, so a
   * fault on the device is noticed by the *next* call, and a stage-by-stage comparison would otherwise blame a
   * readback for a kernel. Calling it between stages is how @c tests/gpu_parity.cpp says which stage faulted;
   * it changes no state and nothing in a render needs it. */
  bool sync(std::string &why);

  bool started() const { return m_band; }

private:
  /** @brief The guard every public stage runs behind.
   *
   * The stages are public so a test can drive a band one stage at a time, and a test is exactly where a
   * stage gets called out of order or against a different renderer than the one @ref start was handed.
   * Before @ref start there is no queue, no program and no buffer to enqueue into, so the alternative to
   * this check is dereferencing a null API table. */
  bool bandLive(LayeredRender &rend, const char *what, std::string &why);

  bool ensureWindow(LayeredRender &rend, std::string &why);
  bool ensureAccum(size_t pixels, std::string &why);
  bool ensureGrid(size_t cells, std::string &why);

  /** @brief Make sure this band's grids can hold the *current* level's grid.
   *
   * Every stage that touches a grid asks for it, not only @ref visit: the stages are public precisely so the
   * parity suite can drive a band one stage at a time, and a stage that assumed a grid somebody else had
   * allocated would make that suite test a different sequence than the plugin runs. The call is idempotent
   * and its fast path is a comparison, so a whole-band render pays it @c levels.count() times per band and
   * nothing else. The level view it is handed is the one the calling stage read from its own
   * renderer, which is why this needs no renderer of its own. */
  bool needGrid(const LevelView &lv, std::string &why);
  bool ensurePartials(size_t partials, std::string &why);
  bool budget(size_t bytes, const char *what, std::string &why);
  bool launch(cl_kernel kernel, const char *name, size_t n0, size_t n1, const size_t *local, std::string &why);

  Loader *m_api;
  Program *m_prog; // the process-wide one, borrowed
  LayeredRender *m_rend; // the renderer start() was handed: visit() runs the CPU halves of a band through it
  cl_command_queue m_queue;

  // the device's own ceiling for one allocation, and what this band's buffers take: a stage that would
  // ask for more than the card can hand out returns false *before* asking, so the plugin's CPU fallback
  // is chosen by arithmetic rather than by a failed clCreateBuffer deep inside a stage.
  size_t m_cap, m_used;

  // this band's windows
  cl_mem m_img, m_coc, m_side, m_rowOk;
  size_t m_imgBytes, m_cocBytes, m_sideBytes, m_rowOkBytes;
  int m_winX0, m_winY0, m_winW, m_winH; // absolute pixel origin and size of both windows
  int m_imgComp;

  // this band's accumulator, mirroring m_acc (4 floats/pixel) and m_accW (2) rather than interleaving
  // them, so the round trip is a memcpy in each direction
  cl_mem m_acc, m_accW;
  size_t m_accBytes, m_accWBytes;
  int m_rectX0, m_rectW, m_rectY0; // the accumulator is addressed from the rect's first pixel, as m_acc is

  cl_mem m_gridA, m_gridB, m_part, m_taps;
  size_t m_gridCells, m_partCount, m_tapsCount; // m_gridCells is the *allocated* grid, which only ever grows;
                                                // every read and write of it uses the current level's own pitch

  // the level set, as the kernels see it: the ascending radius table @ref zd::LevelSet holds, uploaded
  // once per band, and the host-side staging the window is assembled in before it is written
  cl_mem m_levelsBuf;
  size_t m_levelsBytes;
  int m_numLevels;
  std::vector<float> m_imgHost, m_cocHost;
  std::vector<unsigned char> m_sideHost;

  // What the last stageConvolve of the current level actually convolved, in cell rows. @ref stageComposite
  // checks its own read range against it, which is what makes the stage API safe to drive one stage at a
  // time: a composite asked for on rows this level's convolution never wrote would read whatever the
  // previous level left in the reused grid B, and that is a wrong picture rather than a failure.
  bool m_convDone;
  int m_convLevel, m_convSide, m_convCols, m_convRows, m_convRowLo, m_convRowHi;

  bool m_band; // between start() and finish(): this band's buffers are live

  Session(const Session &);
  Session &operator=(const Session &);
};

/** @brief The whole GPU half of one band: a Session, this renderer's level order, and the accumulator
 * handed back into @p rend so that @ref LayeredRender::writeOutput can run on the CPU exactly as it
 * does when the checkbox is off.
 *
 * Returns false when the device could not do it — and then, and only then, does the plugin run the band
 * with the CPU stages. The band is retried as a unit rather than continued halfway on the CPU because
 * the accumulator is the one piece of state that every visit has already contributed to: a partial GPU
 * composite followed by CPU composites would be an ordering the tests do not describe. */
bool renderBand(LayeredRender &rend, std::string &why);

/** @brief The kernels this file compiles, for tests/gpu_probe.sh to print and for the parity suite to
 * name the stage it is timing. One source string, five kernels: extraction's direct tile (a cell one
 * tile covers writes the AoS grid itself, summing its pixels in the CPU's own row-major order, so that
 * path is bit-identical), extraction's partial tile (a wider cell is split into `ceil(cell/kGpuTile)` by
 * the same number of square tiles, each writing 5 doubles — every slot written, so no buffer needs
 * clearing first), the cell combiner (the tiles' sub-sums added in tile order and scaled by invArea, with
 * the CPU's own "an all-zero cell is already zero" skip), the convolution, the composite. */
const char *kernelSource();

} // namespace gpu
} // namespace zd

#endif // OFXLAB_ZDEFOCUSZ_GPU_H
