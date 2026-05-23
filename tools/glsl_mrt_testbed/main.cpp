// ============================================================================
// Phase 3 feasibility test: standalone GLSL 3.3 core + MRT FBO offscreen render.
//
// Compiles a vertex+fragment shader pair, sets up a 2-attachment MRT FBO, renders
// one triangle, reads back both color buffers, writes them to EXR via OIIO.
//
// If this program runs to completion and produces sensible output EXRs, the
// platform supports the OpenGL features Phase 3 of the ScanlineRender migration
// needs. If it fails, we learn the failure mode BEFORE touching Natron-side code.
//
// Build:  cmake -DBUILD_GLSL_TESTBED=ON ..
//         mingw32-make glsl_mrt_testbed
//
// Run:    ./tools/glsl_mrt_testbed/glsl_mrt_testbed.exe
//         oiiotool --info -v glsl_test_attachment0.exr
//         oiiotool --info -v glsl_test_attachment1.exr
//
// Expected output (success):
//   - attachment0.exr: 256x256 RGBA, triangle with per-vertex color interpolation
//     (red bottom-left → blue bottom-right → yellow top), black outside triangle.
//   - attachment1.exr: 256x256 RGBA, solid green where triangle covers, black
//     elsewhere. Proves MRT writes both attachments simultaneously.
//   - stderr ends with "[SUCCESS] GLSL 3.3 + MRT works on this platform."
// ============================================================================

#include <iostream>
#include <memory>
#include <vector>

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>

#include <OpenImageIO/imageio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ------- Shader sources (GLSL 330 core) -----------------------------------

static const char* kVertShader = R"GLSL(
#version 330 core
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec3 in_color;
out vec3 v_color;
void main() {
    v_color = in_color;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
)GLSL";

static const char* kFragShader = R"GLSL(
#version 330 core
in vec3 v_color;
layout(location = 0) out vec4 out_attachment0;  // attachment 0: per-vertex color
layout(location = 1) out vec4 out_attachment1;  // attachment 1: solid green marker
void main() {
    out_attachment0 = vec4(v_color, 1.0);
    out_attachment1 = vec4(0.0, 1.0, 0.0, 1.0);
}
)GLSL";

// ------- Helpers ----------------------------------------------------------

static bool
checkShader(QOpenGLFunctions_3_3_Core* gl, GLuint shader, const char* what)
{
    GLint ok = 0;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        gl->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(logLen + 1, 0);
        gl->glGetShaderInfoLog(shader, logLen, nullptr, log.data());
        std::cerr << "[FAIL] " << what << " compile:\n" << log.data() << "\n";
        return false;
    }
    return true;
}

static bool
checkProgram(QOpenGLFunctions_3_3_Core* gl, GLuint program)
{
    GLint ok = 0;
    gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        gl->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(logLen + 1, 0);
        gl->glGetProgramInfoLog(program, logLen, nullptr, log.data());
        std::cerr << "[FAIL] program link:\n" << log.data() << "\n";
        return false;
    }
    return true;
}

static bool
saveEXR(const std::string& path, const std::vector<float>& pixels, int w, int h, int channels)
{
    using namespace OIIO;
    std::unique_ptr<ImageOutput> out = ImageOutput::create(path);
    if (!out) {
        std::cerr << "[FAIL] OIIO can't create output for " << path << "\n";
        return false;
    }
    ImageSpec spec(w, h, channels, TypeDesc::FLOAT);
    if (!out->open(path, spec)) {
        std::cerr << "[FAIL] OIIO can't open " << path << ": " << out->geterror() << "\n";
        return false;
    }
    out->write_image(TypeDesc::FLOAT, pixels.data());
    out->close();
    return true;
}

// ------- main -------------------------------------------------------------

int
main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    // Request a GL 3.3 core profile context. This is universally supported
    // on hardware from ~2010 onward (D3D10-class GPUs).
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);

    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    if (!surface.isValid()) {
        std::cerr << "[FAIL] could not create QOffscreenSurface\n";
        return 1;
    }

    QOpenGLContext ctx;
    ctx.setFormat(format);
    if (!ctx.create()) {
        std::cerr << "[FAIL] could not create QOpenGLContext (GL 3.3 core)\n";
        return 1;
    }
    if (!ctx.makeCurrent(&surface)) {
        std::cerr << "[FAIL] could not makeCurrent\n";
        return 1;
    }

    // Qt6 API: QOpenGLVersionFunctionsFactory::get<>() replaces Qt5's
    // QOpenGLContext::versionFunctions<>() which was removed.
    QOpenGLFunctions_3_3_Core* gl =
        QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(&ctx);
    if (!gl) {
        std::cerr << "[FAIL] driver does not expose GL 3.3 Core functions\n";
        return 1;
    }
    gl->initializeOpenGLFunctions();

    std::cerr << "[OK ] GL version:    "  << gl->glGetString(GL_VERSION)                  << "\n";
    std::cerr << "[OK ] GLSL version:  "  << gl->glGetString(GL_SHADING_LANGUAGE_VERSION) << "\n";
    std::cerr << "[OK ] Renderer:      "  << gl->glGetString(GL_RENDERER)                 << "\n";
    std::cerr << "[OK ] Vendor:        "  << gl->glGetString(GL_VENDOR)                   << "\n";

    const int W = 256, H = 256;

    // --- Compile + link shaders ---
    GLuint vs = gl->glCreateShader(GL_VERTEX_SHADER);
    gl->glShaderSource(vs, 1, &kVertShader, nullptr);
    gl->glCompileShader(vs);
    if (!checkShader(gl, vs, "vertex shader")) return 1;

    GLuint fs = gl->glCreateShader(GL_FRAGMENT_SHADER);
    gl->glShaderSource(fs, 1, &kFragShader, nullptr);
    gl->glCompileShader(fs);
    if (!checkShader(gl, fs, "fragment shader")) return 1;

    GLuint prog = gl->glCreateProgram();
    gl->glAttachShader(prog, vs);
    gl->glAttachShader(prog, fs);
    gl->glLinkProgram(prog);
    if (!checkProgram(gl, prog)) return 1;
    std::cerr << "[OK ] Shaders compiled and linked\n";

    // --- Set up VAO + VBO with one triangle (pos.xy, color.rgb interleaved) ---
    float verts[] = {
        // x      y      r     g     b
        -0.6f, -0.5f,  1.0f, 0.0f, 0.0f,   // bottom-left  RED
         0.6f, -0.5f,  0.0f, 0.0f, 1.0f,   // bottom-right BLUE
         0.0f,  0.6f,  1.0f, 1.0f, 0.0f,   // top          YELLOW
    };

    GLuint vao = 0, vbo = 0;
    gl->glGenVertexArrays(1, &vao);
    gl->glBindVertexArray(vao);
    gl->glGenBuffers(1, &vbo);
    gl->glBindBuffer(GL_ARRAY_BUFFER, vbo);
    gl->glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    gl->glEnableVertexAttribArray(1);
    gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));

    // --- Set up MRT FBO with 2 RGBA32F color attachments ---
    GLuint fbo = 0, tex0 = 0, tex1 = 0;
    gl->glGenFramebuffers(1, &fbo);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    gl->glGenTextures(1, &tex0);
    gl->glBindTexture(GL_TEXTURE_2D, tex0);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, W, H, 0, GL_RGBA, GL_FLOAT, nullptr);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex0, 0);

    gl->glGenTextures(1, &tex1);
    gl->glBindTexture(GL_TEXTURE_2D, tex1);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, W, H, 0, GL_RGBA, GL_FLOAT, nullptr);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, tex1, 0);

    GLenum drawBuffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    gl->glDrawBuffers(2, drawBuffers);

    GLenum fboStatus = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[FAIL] MRT FBO not complete (status=0x" << std::hex << fboStatus
                  << "). 2-attachment MRT is unsupported on this driver/hardware.\n";
        return 1;
    }
    std::cerr << "[OK ] MRT FBO created with 2 RGBA32F attachments\n";

    // --- Render the triangle ---
    gl->glViewport(0, 0, W, H);
    gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glUseProgram(prog);
    gl->glBindVertexArray(vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glFinish();

    GLenum err = gl->glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "[FAIL] glGetError after draw: 0x" << std::hex << err << "\n";
        return 1;
    }
    std::cerr << "[OK ] Triangle drawn into MRT\n";

    // --- Read back both attachments ---
    std::vector<float> pixels0(W * H * 4);
    std::vector<float> pixels1(W * H * 4);

    gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl->glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, pixels0.data());

    gl->glReadBuffer(GL_COLOR_ATTACHMENT1);
    gl->glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, pixels1.data());

    err = gl->glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "[FAIL] glGetError after readback: 0x" << std::hex << err << "\n";
        return 1;
    }

    // --- Save as EXRs ---
    if (!saveEXR("glsl_test_attachment0.exr", pixels0, W, H, 4)) return 1;
    if (!saveEXR("glsl_test_attachment1.exr", pixels1, W, H, 4)) return 1;

    std::cerr << "[OK ] Wrote glsl_test_attachment0.exr (per-vertex color interpolation)\n";
    std::cerr << "[OK ] Wrote glsl_test_attachment1.exr (solid green where triangle covers)\n";
    std::cerr << "\n[SUCCESS] GLSL 3.3 + MRT works on this platform. Phase 3 is feasible.\n";

    return 0;
}

// MinGW with -municode links in the windowed-unicode CRT bootstrap
// (crtexewin.o), which calls wWinMain on startup. Since we're really a
// console app, we provide a wWinMain shim that forwards to our main()
// above. Without this the link fails with "undefined reference to wWinMain".
#ifdef _WIN32
extern "C" int WINAPI
wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    return main(__argc, __argv);
}
#endif
