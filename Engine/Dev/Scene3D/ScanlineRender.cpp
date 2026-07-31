/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ScanlineRender.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>   // fprintf/fflush — used by Phase 3 GLSL helpers
#include <cstring>
#include <vector>
#include <unordered_map>

#include "RotationConventions.h"

#include "../../../Global/GLIncludes.h"

#include "../../AppInstance.h"
#include "../../Format.h"
#include "../../Project.h"
#include "../../AppManager.h"
#include "CameraMath.h"
#include "CameraProvider.h"
#include "../DotUtils.h"
#include "Card3D.h"
#include "../../GLShader.h"
#include "Light3D.h"
#include "../Particles/ParticleData.h"
#include "../Particles/ParticleInstance.h"
#include "../Particles/ParticleProvider.h"
#include "ReadVDB.h"
#include "ReadAlembicArchive.h"
#include "Volume3D.h"
#include "Cube3D.h"
#include "Cylinder3D.h"
#include "Scene3D.h"
#include "../../GPUContextPool.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../OSGLContext.h"
#include "ReadGeo.h"
#include "../../CameraTrackerNode.h"
#include "SceneGraph.h"
#include "Sphere3D.h"
#include "UVProject.h"
#include "Project3D.h"
#include "MergeMat.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

struct ScanlineRenderPrivate
{
    // Output
    KnobIntWPtr outputWidth, outputHeight;
    KnobButtonWPtr syncToProject; // copies project default format → width/height

    // Particle rendering
    KnobChoiceWPtr particleMode;   // Point, Disc, Sphere, Sprite
    KnobChoiceWPtr particleBlend;  // Additive, Over
    KnobBoolWPtr   particleSolid;  // edge alpha = p.a (true) vs fade-to-0 (false)
    KnobDoubleWPtr particleScale;  // global size multiplier
    KnobIntWPtr    trailLength;      // Trail mode: past frames spanned
    KnobDoubleWPtr trailHeadWidth;   // Trail mode: width at the particle
    KnobDoubleWPtr trailTailWidth;   // Trail mode: width at the oldest point
    KnobDoubleWPtr trailTailFade;    // Trail mode: alpha at the tail
    KnobColorWPtr  trailTailTint;    // Trail mode: color multiply at the tail
    KnobDoubleWPtr particleMotionBlur; // velocity stretch amount (legacy cheat mode)

    // Global multi-sample motion blur — applies to every geo + particle +
    // camera transform path. Knobs live on the Output tab in a "Motion Blur"
    // group; they cover the whole scene rather than the particle pass alone.
    KnobIntWPtr    motionSamples;        // number of sub-frame samples (1 = off)
    KnobDoubleWPtr motionShutter;        // shutter open fraction (0-1, default 0.5)
    KnobChoiceWPtr shutterOffset;        // Centered / Start / End / Custom
    KnobDoubleWPtr shutterCustomOffset;  // frames added to shutter start when offset = Custom
    KnobDoubleWPtr temporalJitter;       // randomize sample timing within shutter (0-1)

    // Shading mode for mesh geometry. 0 = Shaded (N.L diffuse + ambient,
    // using Light3D if connected), 1 = Flat (no lighting, just texture/color),
    // 2 = Wireframe (solid white lines from triangle edges).
    KnobChoiceWPtr shadingMode;

    // Global ambient fill colour added to surfaces in Shaded mode (so unlit /
    // back-facing areas aren't pure black). Multiplies the surface colour, like
    // the previously hard-coded 0.15 grey. Default 0.15 grey preserves that.
    KnobColorWPtr ambient;

    // Transparency: when on (default), surfaces respect their alpha (alpha < 1
    // is see-through, blended over what's behind). When off, geometry is forced
    // opaque (output alpha = 1 wherever a surface is hit).
    KnobBoolWPtr transparency;

    // Spatial antialiasing level — maps to MSAA sample count
    // (None=1, Low=2, Medium=4, High=8), clamped to the driver's GL_MAX_SAMPLES.
    KnobChoiceWPtr antialiasing;

    // Overscan: extra pixels rendered beyond each edge of the Width x Height
    // frame. The output RoD grows by this amount on all four sides and the
    // frustum widens proportionally (so the extra pixels reveal more of the
    // scene rather than zooming). 0 = no overscan.
    KnobIntWPtr overscan;

    // Projection mode: 0 = Perspective (pinhole, default), 1 = Orthographic
    // (parallel rays; extents from Ortho Width below). UV / Spherical to follow.
    KnobChoiceWPtr projectionMode;
    KnobDoubleWPtr orthoWidth;  // orthographic frustum width in world units

    // Phase 3D/3E — per-pixel AOV outputs. The GLSL/MRT pipeline is mandatory
    // since Phase 3E; AOVs only depend on their own knobs being on.
    KnobBoolWPtr outputDepth;     // depth.Z plane (linear camera-space distance)
    KnobBoolWPtr outputPosition;  // world_position.xyz plane (reconstructed from depth)
    KnobBoolWPtr outputNormal;
    KnobBoolWPtr outputUV;
    KnobBoolWPtr outputPref;
    KnobBoolWPtr outputVelocity;
};


ScanlineRender::ScanlineRender(NodePtr node)
    : EffectInstance(node)
    , _imp(new ScanlineRenderPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

ScanlineRender::~ScanlineRender()
{
}

std::string
ScanlineRender::getPluginDescription() const
{
    return tr("Render a 3D scene through a camera to a 2D image.\n\n"
              "Input 0 (bg): Optional background image (composited behind)\n"
              "Input 1 (obj/scn): 3D geometry (Sphere3D, Card3D, ReadGeo)\n"
              "Input 2 (cam): Camera (Camera3D or ReadAlembicCamera)\n\n"
              "The geometry's img input provides the texture.\n"
              "The camera defines the viewpoint.\n\n"
              "Comparable to scanline-render nodes found in other compositing DCCs.").toStdString();
}

std::string
ScanlineRender::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "bg";
        case 1: return "obj/scn";
        case 2: return "cam";
        default: return "";
    }
}

bool
ScanlineRender::isInputOptional(int inputNb) const
{
    // bg and cam are optional; obj/scn is required
    return (inputNb == 0 || inputNb == 2);
}

void
ScanlineRender::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    // RGBA for beauty. RGB for world_position / Normal / Pref / Velocity / UV
    // (3-channel AOVs). Alpha (1-channel) for depth. Without these, Natron may
    // refuse to allocate the AOV planes with correct bounds.
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
ScanlineRender::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
ScanlineRender::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
ScanlineRender::initializeKnobs()
{
    KnobPagePtr outPage = AppManager::createKnob<KnobPage>(this, tr("Output"));

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Width"));
        k->setName("outputWidth"); k->setDefaultValue(1920);
        k->setMinimum(1); k->setDisplayMinimum(320); k->setDisplayMaximum(4096);
        outPage->addKnob(k); _imp->outputWidth = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Height"));
        k->setName("outputHeight"); k->setDefaultValue(1080);
        k->setMinimum(1); k->setDisplayMinimum(240); k->setDisplayMaximum(4096);
        outPage->addKnob(k); _imp->outputHeight = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Sync to Project"));
        k->setName("syncToProject");
        k->setHintToolTip(tr("Copy the current project default format's width and "
                             "height into the Width/Height knobs above."));
        outPage->addKnob(k); _imp->syncToProject = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Shading Mode"));
        k->setName("shadingMode"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Shaded", "", "N.L diffuse lighting (uses Light3D if connected, else top-right default)"));
        entries.push_back(ChoiceOption("Flat", "", "No lighting — texture or per-vertex color only"));
        entries.push_back(ChoiceOption("Wireframe", "", "Solid white edges derived from triangle indices"));
        k->populateChoices(entries);
        k->setDefaultValue(0); // Shaded
        k->setHintToolTip(tr("Mesh display mode. "
                              "Shaded: N.L diffuse lighting (uses Light3D if connected, otherwise a default top-right light). "
                              "Flat: no lighting — just texture or per-vertex color. "
                              "Wireframe: solid white edges derived from triangle indices."));
        outPage->addKnob(k); _imp->shadingMode = k;
    }
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Ambient"), 3);
        k->setName("ambient");
        k->setDefaultValue(0.15, 0); k->setDefaultValue(0.15, 1); k->setDefaultValue(0.15, 2);
        k->setHintToolTip(tr("Global ambient fill colour added to surfaces in Shaded mode, so "
                              "unlit or back-facing areas are not pure black. It multiplies the "
                              "surface colour (ambient * albedo). Default 0.15 grey matches the "
                              "previous fixed ambient; set to black for no fill, or tint for a "
                              "coloured ambient. No effect in Flat or Wireframe mode."));
        outPage->addKnob(k); _imp->ambient = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Transparency"));
        k->setName("transparency"); k->setDefaultValue(true);
        k->setHintToolTip(tr("When on, surfaces respect their alpha — areas where alpha is "
                              "less than 1 are see-through (blended over whatever is behind). "
                              "When off, geometry renders opaque (output alpha forced to 1 "
                              "where a surface is hit), ignoring texture/material alpha."));
        outPage->addKnob(k); _imp->transparency = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Projection Mode"));
        k->setName("projectionMode"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Perspective",  "perspective",  "Pinhole projection from the camera's focal length + aperture"));
        entries.push_back(ChoiceOption("Orthographic", "orthographic", "Parallel projection — no perspective foreshortening; extents from Ortho Width"));
        entries.push_back(ChoiceOption("UV",           "uv",           "Bake to UV space — rasterize each surface at its UV coords. Lit texture + Normal / Pref (object-space position) / UV AOVs bake into the texture map layout. (World Position / Velocity / Depth AOVs are not meaningful in this mode.)"));
        entries.push_back(ChoiceOption("Spherical",    "spherical",    "360 degree equirectangular (lat-long) render from the camera position. Per-vertex projection: coarse geometry near the poles or the +/-180 longitude seam will distort (subdivide dense meshes for clean results)."));
        k->populateChoices(entries);
        k->setDefaultValue(0); // Perspective
        k->setHintToolTip(tr("How the 3D scene is projected to 2D. "
                              "Perspective: standard pinhole camera. "
                              "Orthographic: parallel rays (technical/elevation views), sized by Ortho Width. "
                              "UV: render each surface into its UV/texture space — bake lit texture, Normal, and object-space position (Pref) into a texture map. "
                              "Spherical: 360 degree lat-long environment render from the camera."));
        outPage->addKnob(k); _imp->projectionMode = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Ortho Width"));
        k->setName("orthoWidth"); k->setDefaultValue(10.0);
        k->setMinimum(0.0001); k->setDisplayMinimum(0.1); k->setDisplayMaximum(100.0);
        k->setHintToolTip(tr("Width (in world units) of the orthographic view frustum. "
                              "Height is derived from the camera's aperture aspect. "
                              "Only used when Projection Mode = Orthographic."));
        outPage->addKnob(k); _imp->orthoWidth = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Antialiasing"));
        k->setName("antialiasing"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("None",   "", "No multisampling (1 sample/pixel) — fastest, hard aliased edges"));
        entries.push_back(ChoiceOption("Low",    "", "2x MSAA"));
        entries.push_back(ChoiceOption("Medium", "", "4x MSAA (default)"));
        entries.push_back(ChoiceOption("High",   "", "8x MSAA — smoothest edges, slowest"));
        k->populateChoices(entries);
        k->setDefaultValue(2); // Medium = 4x, matches the previous hard-coded behaviour
        k->setHintToolTip(tr("Spatial antialiasing quality. Maps to the MSAA sample count "
                              "(None=1, Low=2, Medium=4, High=8), clamped to the GPU's maximum. "
                              "Higher is smoother but slower. This is separate from Motion Blur Samples."));
        outPage->addKnob(k); _imp->antialiasing = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Overscan"));
        k->setName("overscan"); k->setDefaultValue(0);
        k->setMinimum(0); k->setDisplayMinimum(0); k->setDisplayMaximum(512);
        k->setHintToolTip(tr("Render this many extra pixels beyond each edge of the "
                              "Width x Height frame (output bounds grow by this amount on "
                              "all four sides). The frustum widens proportionally so the "
                              "extra pixels reveal more of the scene — useful so downstream "
                              "blurs/transforms/defocus have data past the frame edge. "
                              "0 = off."));
        outPage->addKnob(k); _imp->overscan = k;
    }

    // -------- Motion Blur group (Output tab) --------
    // Scene-wide multi-sample motion blur — samples + shutter + offset modes
    // for camera, geometry, particles, and instances.
    KnobGroupPtr mbGroup = AppManager::createKnob<KnobGroup>(this, tr("Motion Blur"));
    mbGroup->setName("motionBlur");
    mbGroup->setDefaultValue(true);
    outPage->addKnob(mbGroup);

    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Samples"));
        k->setName("motionSamples"); k->setDefaultValue(1);
        k->setMinimum(1); k->setMaximum(32);
        k->setDisplayMinimum(1); k->setDisplayMaximum(16);
        k->setHintToolTip(tr("Physically-accurate motion blur via multi-sample accumulation. "
                              "1 = off. 4-8 = typical quality. 16 = film quality. "
                              "Render cost scales linearly with sample count."));
        mbGroup->addKnob(k); _imp->motionSamples = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Shutter"));
        k->setName("motionShutter"); k->setDefaultValue(0.5);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Shutter open duration as a fraction of frame time. "
                              "0.5 = 180-degree shutter (film standard). "
                              "Only used when Samples > 1."));
        mbGroup->addKnob(k); _imp->motionShutter = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Shutter Offset"));
        k->setName("shutterOffset"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Centered", "", "Shutter open from t - shutter/2 to t + shutter/2 (real-camera behaviour, default)."));
        entries.push_back(ChoiceOption("Start",    "", "Shutter open from t to t + shutter (motion happens after the frame)."));
        entries.push_back(ChoiceOption("End",      "", "Shutter open from t - shutter to t (motion happens before the frame)."));
        entries.push_back(ChoiceOption("Custom",   "", "Use Custom Offset (in frames, added to t) for the shutter start."));
        k->populateChoices(entries);
        k->setDefaultValue(0); // Centered
        k->setHintToolTip(tr("Where the shutter opens relative to the current frame. "
                              "Matches Nuke's ScanlineRender shutter offset semantics."));
        mbGroup->addKnob(k); _imp->shutterOffset = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Custom Offset"));
        k->setName("shutterCustomOffset"); k->setDefaultValue(0.0);
        k->setMinimum(-5.0); k->setMaximum(5.0);
        k->setDisplayMinimum(-1.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("When Shutter Offset = Custom, this is added to the current frame time "
                              "to position the shutter start. Negative values open the shutter before the current frame."));
        mbGroup->addKnob(k); _imp->shutterCustomOffset = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Temporal Jitter"));
        k->setName("temporalJitter"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(1.0);
        k->setHintToolTip(tr("Randomize sample timing within the shutter, breaking the stepped-look "
                              "of low sample counts on slow motion. 0 = uniform spacing (default), "
                              "1 = full random within the shutter window."));
        mbGroup->addKnob(k); _imp->temporalJitter = k;
    }

    // Particle rendering knobs
    KnobPagePtr partPage = AppManager::createKnob<KnobPage>(this, tr("Particles"));
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Particle Mode"));
        k->setName("particleMode"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Point", "", "Single pixel per particle (fast preview)"));
        entries.push_back(ChoiceOption("Disc", "", "Camera-facing filled circle with soft edge"));
        entries.push_back(ChoiceOption("Sphere", "", "Lit sphere with simple N dot L shading"));
        entries.push_back(ChoiceOption("Sprite", "", "Camera-facing quad (current behavior)"));
        entries.push_back(ChoiceOption("Trail", "", "Multi-frame ribbon through each particle's PAST positions — bent trails through bounces and arcs (Houdini particle-trail style). Width/alpha taper head to tail; see the Trail knobs."));
        k->populateChoices(entries);
        k->setDefaultValue(3); // Sprite default (matches current behavior)
        partPage->addKnob(k); _imp->particleMode = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Trail Length"));
        k->setName("trailLength"); k->setDefaultValue(4); k->setAnimationEnabled(true);
        k->setMinimum(1); k->setMaximum(16); k->setDisplayMinimum(1); k->setDisplayMaximum(16);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("Trail mode: how many PAST frames the ribbon spans. Trails bend through bounces (V shapes) because they follow the particle's real path."));
        partPage->addKnob(k); _imp->trailLength = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Head Width"));
        k->setName("trailHeadWidth"); k->setDefaultValue(1.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(4.0);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("Trail width at the particle (head), as a multiple of particle size."));
        partPage->addKnob(k); _imp->trailHeadWidth = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Tail Width"));
        k->setName("trailTailWidth"); k->setDefaultValue(0.25); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setDisplayMinimum(0.0); k->setDisplayMaximum(4.0);
        k->setHintToolTip(tr("Trail width at the oldest point (tail), as a multiple of particle size. Smaller than Head Width = teardrop."));
        partPage->addKnob(k); _imp->trailTailWidth = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Tail Fade"));
        k->setName("trailTailFade"); k->setDefaultValue(0.0); k->setAnimationEnabled(true);
        k->setMinimum(0.0); k->setMaximum(1.0);
        k->setAddNewLine(false);
        k->setHintToolTip(tr("Alpha at the tail relative to the head. 0 = trail fades out completely toward the tail."));
        partPage->addKnob(k); _imp->trailTailFade = k;
    }
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Tail Tint"), 3);
        k->setName("trailTailTint");
        k->setDefaultValue(1.0, 0); k->setDefaultValue(1.0, 1); k->setDefaultValue(1.0, 2);
        k->setAnimationEnabled(true);
        k->setHintToolTip(tr("Color multiplier at the tail, lerped along the trail. White = off. For sparks try a deep red so the head burns white-hot and the tail cools."));
        partPage->addKnob(k); _imp->trailTailTint = k;
    }
    refreshTrailKnobsVisibility(); // hidden unless Particle Mode = Trail
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Blend Mode"));
        k->setName("particleBlend"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Additive", "", "Bright, glowy — fire, sparks, energy"));
        entries.push_back(ChoiceOption("Over", "", "Alpha composite — solid particles, smoke, debris"));
        k->populateChoices(entries);
        k->setDefaultValue(0); // Additive default
        partPage->addKnob(k); _imp->particleBlend = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Solid"));
        k->setName("particleSolid"); k->setAnimationEnabled(false);
        k->setDefaultValue(false);
        k->setHintToolTip(tr("Render Disc / Sphere / Sprite with full alpha at the edges "
                             "instead of fading to transparent. Off (default): soft "
                             "anti-aliased falloff. On: hard edge, solid look."));
        partPage->addKnob(k); _imp->particleSolid = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Particle Scale"));
        k->setName("particleScale"); k->setDefaultValue(1.0);
        k->setMinimum(0.01); k->setDisplayMinimum(0.1); k->setDisplayMaximum(10.0);
        k->setHintToolTip(tr("Global multiplier on particle size."));
        partPage->addKnob(k); _imp->particleScale = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, tr("Motion Blur (stretch)"));
        k->setName("particleMotionBlur"); k->setDefaultValue(0.0);
        k->setMinimum(0.0); k->setMaximum(5.0);
        k->setDisplayMinimum(0.0); k->setDisplayMaximum(2.0);
        k->setHintToolTip(tr("Fast velocity-stretch motion blur (cheat). Stretches particles/instances along velocity. "
                              "This is a beauty-only fake — only Velocity and Pref AOVs are meaningful when stretch is on; "
                              "Normal/UV are flat (no real surface), and Depth/World Position skip particles entirely "
                              "because the stretched fans are translucent (no depth write). "
                              "For motion-blurred AOVs use Motion Samples > 1, which renders at sub-frame times and "
                              "averages every AOV through the integration."));
        partPage->addKnob(k); _imp->particleMotionBlur = k;
    }
    // Motion blur knobs (Samples / Shutter / Shutter Offset / Custom Offset /
    // Temporal Jitter) moved to Output tab — see Motion Blur group above.
    // They're scene-wide settings, not particle-specific.

    // Phase 3D — AOV outputs page. Each toggle adds a per-pixel arbitrary
    // output variable on top of beauty. They only fire when the GLSL pipeline
    // (Output page) is also on; otherwise the legacy fixed-function path
    // doesn't have a way to emit them.
    KnobPagePtr aovPage = AppManager::createKnob<KnobPage>(this, tr("AOVs"));
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Depth"));
        k->setName("outputDepth"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel linear camera-space distance (depth.Z). "
                              "Background pixels emit the camera Far value."));
        aovPage->addKnob(k); _imp->outputDepth = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("World Position"));
        k->setName("outputPosition"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel world-space surface position (world_position.x/y/z), "
                              "reconstructed from depth via inverse(proj * view). "
                              "Background pixels emit (0,0,0)."));
        aovPage->addKnob(k); _imp->outputPosition = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Normal"));
        k->setName("outputNormal"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel surface normal in world space (Normal.x/y/z)."));
        aovPage->addKnob(k); _imp->outputNormal = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("UV"));
        k->setName("outputUV"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel texture coordinate (uv.u/v/w)."));
        aovPage->addKnob(k); _imp->outputUV = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Pref"));
        k->setName("outputPref"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel reference position in object space (Pref.x/y/z). "
                              "Useful as a texture-projection key."));
        aovPage->addKnob(k); _imp->outputPref = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Velocity"));
        k->setName("outputVelocity"); k->setDefaultValue(false);
        k->setHintToolTip(tr("Per-pixel screen-space motion vector in pixels per frame "
                              "(Velocity.x/y/z). Z is reserved (0). Re-extracts geometry "
                              "and camera at time-1."));
        aovPage->addKnob(k); _imp->outputVelocity = k;
    }
}

void
ScanlineRender::refreshTrailKnobsVisibility()
{
    const bool isTrail = _imp->particleMode.lock()
                       && _imp->particleMode.lock()->getValue() == 4;
    if (KnobIntPtr k = _imp->trailLength.lock())      k->setSecret(!isTrail);
    if (KnobDoublePtr k = _imp->trailHeadWidth.lock()) k->setSecret(!isTrail);
    if (KnobDoublePtr k = _imp->trailTailWidth.lock()) k->setSecret(!isTrail);
    if (KnobDoublePtr k = _imp->trailTailFade.lock())  k->setSecret(!isTrail);
    if (KnobColorPtr k = _imp->trailTailTint.lock())   k->setSecret(!isTrail);
}

bool
ScanlineRender::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/, double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    if (!k) return false;

    // Trail knobs only matter in Trail mode.
    if (_imp->particleMode.lock() && k == _imp->particleMode.lock().get()) {
        refreshTrailKnobsVisibility();
        return false; // mode change still evaluates normally
    }

    // Sync to Project — copy project default format → width/height.
    KnobButtonPtr syncBtn = _imp->syncToProject.lock();
    if (syncBtn && k == syncBtn.get()) {
        AppInstancePtr app = getApp();
        if (app && app->getProject()) {
            Format fmt;
            app->getProject()->getProjectDefaultFormat(&fmt);
            const int pw = fmt.width();
            const int ph = fmt.height();
            KnobIntPtr wk = _imp->outputWidth.lock();
            KnobIntPtr hk = _imp->outputHeight.lock();
            if (pw > 0 && ph > 0 && wk && hk) {
                wk->setValue(pw);
                hk->setValue(ph);
            }
        }
        return true;
    }

    return false;
}

bool
ScanlineRender::getBgConformRect(double time, ViewIdx view, RectI* outRect)
{
    // Nuke-style conform: a connected bg (input 0) defines the output rectangle so the render
    // matches the bg / Reformat. Important for UV-bake projection, where the render res should
    // match the UV space (e.g. a square plate).
    //
    // Prefer the bg's output FORMAT over its region of definition. A Reformat sets a new format
    // (e.g. a square) but, with "black outside" off, keeps the original (e.g. 1920x1080) data
    // window as its RoD — so the RoD would not be square even though the format is. The format
    // is exactly the conform target the user means by "set the Reformat to square". Fall back to
    // the RoD only when the bg reports no usable format (e.g. some input-less generators).
    EffectInstancePtr bg = getInput(0);
    if (!bg) {
        return false;
    }
    const RectI fmt = bg->getOutputFormat();
    if (fmt.x2 > fmt.x1 && fmt.y2 > fmt.y1) {
        *outRect = fmt;
        return true;
    }
    RectD bgRod;
    if (bg->getRegionOfDefinition_public(bg->getRenderHash(), time, RenderScale(), view, &bgRod, NULL) == eStatusOK
        && bgRod.x2 > bgRod.x1 && bgRod.y2 > bgRod.y1) {
        outRect->x1 = (int)std::floor(bgRod.x1);
        outRect->y1 = (int)std::floor(bgRod.y1);
        outRect->x2 = (int)std::ceil(bgRod.x2);
        outRect->y2 = (int)std::ceil(bgRod.y2);
        return true;
    }
    return false;
}

StatusEnum
ScanlineRender::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& /*scale*/,
                                      ViewIdx view, RectD* rod)
{
    RectI bgRect;
    if (getBgConformRect(time, view, &bgRect)) {
        rod->x1 = bgRect.x1;
        rod->y1 = bgRect.y1;
        rod->x2 = bgRect.x2;
        rod->y2 = bgRect.y2;
        return eStatusOK;
    }
    const int ov = _imp->overscan.lock() ? _imp->overscan.lock()->getValue() : 0;
    rod->x1 = -ov;
    rod->y1 = -ov;
    rod->x2 = _imp->outputWidth.lock()->getValue()  + ov;
    rod->y2 = _imp->outputHeight.lock()->getValue() + ov;
    return eStatusOK;
}

StatusEnum
ScanlineRender::getPreferredMetadata(NodeMetadata& metadata)
{
    // The render depends on the current frame (animated geo / lights / camera).
    metadata.setIsFrameVarying(true);

    // Declare the output FORMAT (the displayed canvas, distinct from the RoD). Without this the
    // node would inherit the project format (e.g. HD) and the viewer/downstream would show HD
    // even though the RoD conformed to the bg. Conform the format to the bg the same way the RoD
    // does, so format + RoD + framebuffer all agree.
    RectI bgRect;
    if ( getBgConformRect(0., ViewIdx(0), &bgRect) ) {
        RectI fmt;
        fmt.x1 = 0; fmt.y1 = 0;
        fmt.x2 = std::max(1, bgRect.width());
        fmt.y2 = std::max(1, bgRect.height());
        metadata.setOutputFormat(fmt);
    } else {
        RectI fmt;
        fmt.x1 = 0; fmt.y1 = 0;
        fmt.x2 = std::max(1, _imp->outputWidth.lock()->getValue());
        fmt.y2 = std::max(1, _imp->outputHeight.lock()->getValue());
        metadata.setOutputFormat(fmt);
    }
    return eStatusOK;
}

namespace {
// Phase 3D — forward declarations so the member function below can call
// these factories. Their definitions live further down in the same anonymous
// namespace (alongside the GLSL helpers).
static ImagePlaneDesc makeDepthPlane();
static ImagePlaneDesc makeWorldPositionPlane();
static ImagePlaneDesc makeNormalPlane();
static ImagePlaneDesc makeUvPlane();
static ImagePlaneDesc makePrefPlane();
static ImagePlaneDesc makeVelocityPlane();
} // namespace

void
ScanlineRender::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                               EffectInstance::ComponentsNeededMap* comps,
                                               double* passThroughTime,
                                               int* passThroughView,
                                               int* passThroughInput)
{
    // Beauty on the default color plane is always produced.
    (*comps)[-1].push_back(ImagePlaneDesc::getRGBAComponents());

    // Phase 3D — declare each AOV plane only when its knob is on. The render
    // path checks the same knobs to decide whether to allocate MRT attachments
    // and run the readback/blit; keeping them gated here ensures we don't
    // promise planes we won't fill.
    KnobBoolPtr depthK    = _imp->outputDepth.lock();
    KnobBoolPtr posK      = _imp->outputPosition.lock();
    KnobBoolPtr normalK   = _imp->outputNormal.lock();
    KnobBoolPtr uvK       = _imp->outputUV.lock();
    KnobBoolPtr prefK     = _imp->outputPref.lock();
    KnobBoolPtr velocityK = _imp->outputVelocity.lock();
    if (depthK    && depthK->getValue())    (*comps)[-1].push_back(makeDepthPlane());
    if (posK      && posK->getValue())      (*comps)[-1].push_back(makeWorldPositionPlane());
    if (normalK   && normalK->getValue())   (*comps)[-1].push_back(makeNormalPlane());
    if (uvK       && uvK->getValue())       (*comps)[-1].push_back(makeUvPlane());
    if (prefK     && prefK->getValue())     (*comps)[-1].push_back(makePrefPlane());
    if (velocityK && velocityK->getValue()) (*comps)[-1].push_back(makeVelocityPlane());

    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0; // bg input passes through
}

// ==================== Helpers ====================

static void
buildViewMatrix(double tx, double ty, double tz,
                double rx, double ry, double rz,
                float out[16])
{
    // View matrix = inverse of camera-to-world transform.
    // Camera-to-world uses Natron's standard extrinsic XYZ convention
    // (M = Rz*Ry*Rx column-vector, Maya/Blender/Houdini default, same as
    // SceneGraph::buildTRS and ImGuizmo). The inverse is M^T.
    double mInv[3][3];
    RotationConventions::composeInverse(rx, ry, rz, mInv);

    const float ntx = -(float)tx, nty = -(float)ty, ntz = -(float)tz;

    // Pack into column-major float[16]: out[col*4 + row] = mInv[row][col].
    out[0]  = (float)mInv[0][0]; out[1]  = (float)mInv[1][0]; out[2]  = (float)mInv[2][0]; out[3]  = 0.f;
    out[4]  = (float)mInv[0][1]; out[5]  = (float)mInv[1][1]; out[6]  = (float)mInv[2][1]; out[7]  = 0.f;
    out[8]  = (float)mInv[0][2]; out[9]  = (float)mInv[1][2]; out[10] = (float)mInv[2][2]; out[11] = 0.f;
    out[12] = (float)(mInv[0][0]*ntx + mInv[0][1]*nty + mInv[0][2]*ntz);
    out[13] = (float)(mInv[1][0]*ntx + mInv[1][1]*nty + mInv[1][2]*ntz);
    out[14] = (float)(mInv[2][0]*ntx + mInv[2][1]*nty + mInv[2][2]*ntz);
    out[15] = 1.f;
}

// Projection matrix is now built via CameraMath::composeProjectionMatrix
// (independent fov_h / fov_v from both apertures). Image aspect is no longer
// used to derive the Y FOV — that was a long-standing bug producing CG drift
// proportional to camera motion when sensor aspect != image aspect.

// Build the projection matrix for the selected projection mode, applying the
// overscan extent scaling. projMode: 0 = Perspective, 1 = Orthographic.
// Centralized so every projection site (main, motion-blur sub-sample, and the
// previous-frame velocity reference) stays consistent.
static void
buildProjectionForMode(int projMode,
                       double focalLength, double hAperture, double vAperture,
                       double orthoWidth, double ovScaleX, double ovScaleY,
                       float nearZ, float farZ, float out[16])
{
    if (projMode == 1) {
        // Orthographic: symmetric parallel frustum. Width is user-set (world
        // units); height preserves the aperture aspect (vA/hA). Overscan widens
        // the extents proportionally, mirroring the perspective aperture scale.
        const double halfW = 0.5 * orthoWidth * ovScaleX;
        const double aspect = (hAperture > 1e-6) ? (vAperture / hAperture) : 1.0;
        const double halfH = 0.5 * orthoWidth * aspect * ovScaleY;
        std::memset(out, 0, 16 * sizeof(float));
        out[0]  = (halfW > 1e-9) ? (float)(1.0 / halfW) : 1.0f;
        out[5]  = (halfH > 1e-9) ? (float)(1.0 / halfH) : 1.0f;
        out[10] = -2.0f / (farZ - nearZ);
        out[14] = -(farZ + nearZ) / (farZ - nearZ);
        out[15] = 1.0f;
    } else {
        // Perspective (default). Overscan scales each aperture so the extra
        // pixels reveal more scene at the same per-pixel angular size.
        CameraMath::composeProjectionMatrix(focalLength, hAperture * ovScaleX,
                                            vAperture * ovScaleY, nearZ, farZ, out);
    }
}

// ==================== 4x4 matrix helpers (column-major, OpenGL convention) ====================

// out = a * b   (column-major). Used by the GLSL render path to compose
// projView * localMatrix into a per-mesh MVP uniform.
static void
mat4Mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int c = 0; c < 4; ++c) {
        for (int rIdx = 0; rIdx < 4; ++rIdx) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) {
                s += a[k * 4 + rIdx] * b[c * 4 + k];
            }
            r[c * 4 + rIdx] = s;
        }
    }
    std::memcpy(out, r, sizeof(r));
}

// Apply a 4x4 column-major matrix to (x,y,z,w). Available for future GLSL
// CPU-side prep (Phase 3D-3 Velocity AOV will use it for the previous-frame
// clip-space derivation when the GPU path needs CPU validation).
static void
mat4Apply(const float m[16], float x, float y, float z, float w,
          float* ox, float* oy, float* oz, float* ow)
{
    *ox = m[0]*x + m[4]*y + m[8]*z  + m[12]*w;
    *oy = m[1]*x + m[5]*y + m[9]*z  + m[13]*w;
    *oz = m[2]*x + m[6]*y + m[10]*z + m[14]*w;
    *ow = m[3]*x + m[7]*y + m[11]*z + m[15]*w;
}

// ==================== GLSL / MRT helpers ====================
//
// Shared utilities for building shader programs and (optionally) MRT FBOs.
// The mesh and particle render paths both use these; Phase 3E retired the
// fixed-function path so they're always invoked.
//
// Once these stabilize and no longer need Scene3D-specific tweaks, they can
// move to a dedicated `GlslRenderHelpers.{h,cpp}` for reuse by ParticleRender,
// Project3D, and UVProject.

namespace {

// Phase 3D — AOV plane factories. Each AOV is its own ImagePlaneDesc with a
// stable plane ID + channel name layout, so downstream nodes (Shuffle, Write)
// can address them by name. Channel names are short to keep EXR multipart
// metadata tidy. The plane ID/label pair follows Natron's convention of
// matching common DCC names (Nuke, Mantra) for round-trip compatibility.
static ImagePlaneDesc makeDepthPlane()
{
    // Channel name "Z" uppercase is the OpenEXR convention for depth (Houdini
    // Mantra, V-Ray, Arnold, Renderman). Lowercase "z" is recognized by Nuke
    // but is filtered out by stricter EXR readers.
    std::vector<std::string> channels;
    channels.push_back("Z");
    return ImagePlaneDesc("depth", "depth", "Z", channels);
}

static ImagePlaneDesc makeWorldPositionPlane()
{
    std::vector<std::string> channels;
    channels.push_back("x"); channels.push_back("y"); channels.push_back("z");
    return ImagePlaneDesc("world_position", "world_position", "xyz", channels);
}

static ImagePlaneDesc makeNormalPlane()
{
    std::vector<std::string> channels;
    channels.push_back("x"); channels.push_back("y"); channels.push_back("z");
    return ImagePlaneDesc("Normal", "Normal", "xyz", channels);
}

static ImagePlaneDesc makeUvPlane()
{
    std::vector<std::string> channels;
    channels.push_back("u"); channels.push_back("v"); channels.push_back("w");
    return ImagePlaneDesc("uv", "uv", "uvw", channels);
}

static ImagePlaneDesc makePrefPlane()
{
    std::vector<std::string> channels;
    channels.push_back("x"); channels.push_back("y"); channels.push_back("z");
    return ImagePlaneDesc("Pref", "Pref", "xyz", channels);
}

static ImagePlaneDesc makeVelocityPlane()
{
    std::vector<std::string> channels;
    channels.push_back("x"); channels.push_back("y"); channels.push_back("z");
    return ImagePlaneDesc("Velocity", "Velocity", "xyz", channels);
}

// Compile a single shader stage. Logs failure to stderr. Returns 0 on failure.
static GLuint
glslCompileShader(GLenum stage, const char* source, const char* stageName)
{
    GLuint sh = glCreateShader(stage);
    glShaderSource(sh, 1, &source, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(logLen + 1, 0);
        glGetShaderInfoLog(sh, logLen, NULL, log.data());
        std::fprintf(stderr, "[GLSL FAIL] %s compile:\n%s\n", stageName, log.data());
        std::fflush(stderr);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// Compile + link a vertex+fragment shader pair. Returns 0 on failure.
// Caller owns the returned program — must glDeleteProgram() when done.
static GLuint
glslBuildProgram(const char* vertSrc, const char* fragSrc)
{
    GLuint vs = glslCompileShader(GL_VERTEX_SHADER, vertSrc, "vertex shader");
    if (!vs) return 0;
    GLuint fs = glslCompileShader(GL_FRAGMENT_SHADER, fragSrc, "fragment shader");
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint logLen = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(logLen + 1, 0);
        glGetProgramInfoLog(prog, logLen, NULL, log.data());
        std::fprintf(stderr, "[GLSL FAIL] program link:\n%s\n", log.data());
        std::fflush(stderr);
        glDeleteProgram(prog);
        prog = 0;
    }
    // Shaders can be deleted now — they're attached to the program.
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// Create a multi-attachment FBO with N RGBA32F color attachments + a depth
// renderbuffer. Out params receive the FBO handle + texture/RB handles.
// `outColorTextures` will have `numColorAttachments` entries on success.
// Returns true on success; on failure, all created resources are deleted
// and out params are zeroed. Phase 3D consumers wire this up to MRT AOV
// blits — Phase 3C only uses the standalone helpers.
[[maybe_unused]] static bool
glslSetupMrtFbo(int width, int height, int numColorAttachments,
                GLuint* outFbo,
                std::vector<GLuint>* outColorTextures,
                GLuint* outDepthRb)
{
    *outFbo = 0;
    *outDepthRb = 0;
    outColorTextures->clear();
    if (numColorAttachments <= 0 || numColorAttachments > 8) {
        std::fprintf(stderr, "[GLSL FAIL] MRT: invalid attachment count %d (max 8)\n",
                     numColorAttachments);
        std::fflush(stderr);
        return false;
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    std::vector<GLuint> colorTex(numColorAttachments, 0);
    std::vector<GLenum> drawBufs(numColorAttachments);
    for (int i = 0; i < numColorAttachments; ++i) {
        glGenTextures(1, &colorTex[i]);
        glBindTexture(GL_TEXTURE_2D, colorTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0 + i,
                               GL_TEXTURE_2D, colorTex[i], 0);
        drawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
    }

    GLuint depthRb = 0;
    glGenRenderbuffers(1, &depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);

    glDrawBuffers(numColorAttachments, drawBufs.data());

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[GLSL FAIL] MRT FBO incomplete (status=0x%x, attachments=%d)\n",
                     status, numColorAttachments);
        std::fflush(stderr);
        for (GLuint t : colorTex) if (t) glDeleteTextures(1, &t);
        glDeleteRenderbuffers(1, &depthRb);
        glDeleteFramebuffers(1, &fbo);
        return false;
    }

    *outFbo = fbo;
    *outDepthRb = depthRb;
    *outColorTextures = colorTex;
    return true;
}

// Tear down an MRT FBO created by glslSetupMrtFbo. Safe to call with zero handles.
[[maybe_unused]] static void
glslTeardownMrtFbo(GLuint fbo, const std::vector<GLuint>& colorTextures, GLuint depthRb)
{
    if (fbo) glDeleteFramebuffers(1, &fbo);
    for (GLuint t : colorTextures) if (t) glDeleteTextures(1, &t);
    if (depthRb) glDeleteRenderbuffers(1, &depthRb);
}

} // anonymous namespace

// Invert a 4x4 column-major matrix. Returns false on singular. Used by
// the GLSL render path to compute the normal matrix from the local-to-world
// transform (inverse-transpose of the upper 3x3).
static bool
mat4Invert(float out[16], const float m[16])
{
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) return false;
    float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * invDet;
    return true;
}

// ==================== Phase 3C: beauty shaders ====================
//
// GLSL passthrough shaders for the beauty plane. Supports both regular UV
// texturing AND STW projective texturing (UVProject Perspective mode) via a
// u_hasTexture uniform: 0 = no texture (white), 1 = regular UV, 2 = STW.
//
// The fragment shader has output locations for 5 attachments (beauty + 4 AOVs)
// so Phase 3D can extend without recompiling. AOV writes are gated by
// u_writeNormal / u_writeUV / u_writePref / u_writeVelocity uniforms — all
// default to 0 in Phase 3C, so only attachment 0 (beauty) gets meaningful
// data. Unused attachments either don't exist in the FBO (driver silently
// drops writes) or write zeros — both fine.

static const char* kBeautyVert =
    "#version 330 core\n"
    "layout(location = 0) in vec3 in_pos;\n"
    "layout(location = 1) in vec2 in_uv;\n"
    "layout(location = 2) in vec4 in_stw;       // s, t, 0, w (for projective texturing)\n"
    "layout(location = 3) in vec3 in_normal;    // object-space normal (Phase 3D)\n"
    "layout(location = 4) in vec3 in_prevPos;   // previous-frame object-space position (Phase 3D-3 — velocity)\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_prevMvp;                   // previous-frame MVP for velocity computation\n"
    "uniform mat4 u_localMatrix;               // object -> world (for v_worldPos in Shaded mode)\n"
    "uniform mat3 u_normalMatrix;              // transpose(inverse(localMatrix3x3)) for world normals\n"
    "uniform vec2 u_viewportSize;              // (width, height) in pixels — scales NDC delta to screen pixels\n"
    "uniform int  u_projMode;                  // 0/1 = matrix (perspective/orthographic), 2 = UV bake, 3 = spherical\n"
    "uniform mat4 u_view;                      // world->view, for spherical lat-long projection\n"
    "uniform float u_near;\n"
    "uniform float u_far;\n"
    "uniform int  u_useProjector;             // 1 = a Project3D material is active on this geo\n"
    "uniform mat4 u_projectorVP;              // projector view*projection (world -> projector clip)\n"
    "out vec2 v_uv;\n"
    "out vec4 v_stw;\n"
    "out vec3 v_worldNormal;\n"
    "out vec3 v_worldPos;                      // world-space position — used by Shaded mode lighting\n"
    "out vec3 v_prefPos;                       // object-space position (= in_pos) — Pref AOV\n"
    "out vec4 v_currClipPos;                   // current-frame clip-space position — for velocity\n"
    "out vec4 v_prevClipPos;                   // previous-frame clip-space position — for velocity\n"
    "out vec4 v_projClip;                      // projector clip-space position — Project3D\n"
    "void main() {\n"
    "    v_uv          = in_uv;\n"
    "    v_stw         = in_stw;\n"
    "    v_worldNormal = u_normalMatrix * in_normal;\n"
    "    v_worldPos    = (u_localMatrix * vec4(in_pos, 1.0)).xyz;\n"
    "    v_projClip    = (u_useProjector == 1) ? (u_projectorVP * vec4(v_worldPos, 1.0)) : vec4(0.0);\n"
    "    v_prefPos     = in_pos;\n"
    "    v_currClipPos = u_mvp * vec4(in_pos, 1.0);\n"
    "    v_prevClipPos = u_prevMvp * vec4(in_prevPos, 1.0);\n"
    "    if (u_projMode == 2) {\n"
    "        // UV bake: place the vertex at its UV coordinate (UV [0,1] -> NDC\n"
    "        // [-1,1]) so the lit/textured surface is rasterized into texture space.\n"
    "        gl_Position = vec4(in_uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "    } else if (u_projMode == 3) {\n"
    "        // Spherical (equirectangular lat-long): map the view-space direction\n"
    "        // to longitude (x) and latitude (y); radius drives depth for occlusion.\n"
    "        vec3 vp = (u_view * vec4(v_worldPos, 1.0)).xyz;\n"
    "        float r = length(vp);\n"
    "        float lon = atan(vp.x, -vp.z);                                  // -PI..PI\n"
    "        float lat = asin(clamp(vp.y / max(r, 1e-6), -1.0, 1.0));        // -PI/2..PI/2\n"
    "        float ndcZ = clamp((r - u_near) / max(u_far - u_near, 1e-6), 0.0, 1.0) * 2.0 - 1.0;\n"
    "        gl_Position = vec4(lon / 3.14159265, lat / 1.5707963, ndcZ, 1.0);\n"
    "    } else {\n"
    "        gl_Position = v_currClipPos;\n"
    "    }\n"
    "}\n";

static const char* kBeautyFrag =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_stw;\n"
    "in vec3 v_worldNormal;\n"
    "in vec3 v_worldPos;\n"
    "in vec3 v_prefPos;\n"
    "in vec4 v_currClipPos;\n"
    "in vec4 v_prevClipPos;\n"
    "in vec4 v_projClip;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_viewportSize;\n"
    "uniform int u_hasTexture;       // 0=no, 1=regular UV, 2=STW projective\n"
    "uniform int u_numProj;          // number of active projection layers (0 = none, MergeMat stacks)\n"
    "uniform sampler2D u_projPlate0; // per-layer plates, texture units 1..4\n"
    "uniform sampler2D u_projPlate1;\n"
    "uniform sampler2D u_projPlate2;\n"
    "uniform sampler2D u_projPlate3;\n"
    "uniform mat4  u_projVP[4];      // per-layer world -> projector clip\n"
    "uniform int   u_projOn[4];      // 0=front, 1=back, 2=both\n"
    "uniform int   u_projCrop[4];    // 1 = transparent outside the plate frame\n"
    "uniform float u_projNear[4];    // projector-space near clip (distance)\n"
    "uniform float u_projFar[4];     // projector-space far clip (distance)\n"
    "uniform vec3  u_projForward[4]; // per-layer projector forward (world)\n"
    "uniform int   u_projOp[4];      // MergeMat op compositing this layer over those below\n"
    "uniform float u_projMix[4];     // per-layer opacity\n"
    "uniform int u_occMode;          // layer-0 occlusion: 0=none, 1/2 = occlude (depth on unit 5)\n"
    "uniform sampler2D u_occDepth;   // projector depth map (nearest-surface distance)\n"
    "uniform int u_shadingMode;      // 0=Shaded, 1=Flat, 2=Wireframe\n"
    "uniform int u_hasLight;         // 1 if Light3D connected, 0 = fallback default\n"
    "uniform vec3 u_lightPos;        // world-space point light position\n"
    "uniform vec3 u_lightColor;\n"
    "uniform float u_lightIntensity;\n"
    "uniform vec3 u_cameraPos;       // world-space camera position (fallback headlight when no Light3D)\n"
    "uniform vec3 u_ambient;         // global ambient fill colour (Shaded mode)\n"
    "uniform int u_transparency;     // 1 = respect surface alpha, 0 = force opaque\n"
    "uniform int u_writeNormal;      // attachment 1 (Phase 3D)\n"
    "uniform int u_writeUV;          // attachment 2 (Phase 3D)\n"
    "uniform int u_writePref;        // attachment 3 (Phase 3D)\n"
    "uniform int u_writeVelocity;    // attachment 4 (Phase 3D)\n"
    "layout(location = 0) out vec4 out_color;\n"
    "layout(location = 1) out vec4 out_normal;\n"
    "layout(location = 2) out vec4 out_uv;\n"
    "layout(location = 3) out vec4 out_pref;\n"
    "layout(location = 4) out vec4 out_velocity;\n"
    "// Sample projection layer i (straight-alpha). Returns (0) where this layer doesn't project\n"
    "// here; grey base where face/depth/occlusion culled with crop off; smeared edge for off-frame\n"
    "// with crop off. `plate` is passed explicitly so we never dynamically index a sampler array.\n"
    "vec4 mm_sampleProj(int i, sampler2D plate) {\n"
    "    vec4 pclip = u_projVP[i] * vec4(v_worldPos, 1.0);\n"
    "    if (pclip.w <= 0.0) return vec4(0.0);\n"
    "    float pd = pclip.w;\n"
    "    vec2 puv = (pclip.xy / pclip.w) * 0.5 + 0.5;\n"
    "    float f = dot(normalize(v_worldNormal), normalize(u_projForward[i]));\n"
    "    bool faceOK  = (u_projOn[i]==2) || (u_projOn[i]==0 && f<0.0) || (u_projOn[i]==1 && f>0.0);\n"
    "    bool depthOK = (pd >= u_projNear[i] && pd <= u_projFar[i]);\n"
    "    bool inFrame = (puv.x>=0.0 && puv.x<=1.0 && puv.y>=0.0 && puv.y<=1.0);\n"
    "    bool occluded = false;\n"
    "    if (i == 0 && u_occMode != 0 && inFrame) {\n"
    "        float fragDepth = (pclip.z / pclip.w) * 0.5 + 0.5;\n"
    "        if (fragDepth > texture(u_occDepth, puv).r + 0.0015) occluded = true;\n"
    "    }\n"
    "    if (!faceOK || !depthOK || occluded) return (u_projCrop[i]==1) ? vec4(0.0) : vec4(0.45,0.45,0.45,1.0);\n"
    "    if (!inFrame) return (u_projCrop[i]==1) ? vec4(0.0) : texture(plate, clamp(puv,0.0,1.0));\n"
    "    return texture(plate, puv);\n"
    "}\n"
    "// Composite straight-alpha foreground `a` over premultiplied accumulation `b` by MergeMat op.\n"
    "vec4 mm_composite(vec4 a, vec4 b, int op, float mix) {\n"
    "    float aa = a.a * mix;\n"
    "    vec3  apm = a.rgb * aa;\n"
    "    if (op == 0) return b;\n"
    "    if (op == 1) return vec4(apm, aa);\n"
    "    if (op == 3) return b * (1.0 - aa);\n"
    "    if (op == 4) return b * aa;\n"
    "    if (op == 5) return vec4(apm + b.rgb, min(1.0, aa + b.a));\n"
    "    if (op == 6) return vec4(max(apm, b.rgb), max(aa, b.a));\n"
    "    if (op == 7) return vec4(min(apm, b.rgb), min(aa, b.a));\n"
    "    return vec4(apm + b.rgb*(1.0-aa), aa + b.a*(1.0-aa));\n"
    "}\n"
    "void main() {\n"
    "    // --- Wireframe: solid white edges, skip texture sampling + lighting ---\n"
    "    if (u_shadingMode == 2) {\n"
    "        out_color = vec4(1.0, 1.0, 1.0, 1.0);\n"
    "    } else {\n"
    "        // --- Base color from texture / STW / vertex (existing logic) ---\n"
    "        vec4 baseColor;\n"
    "        if (u_numProj > 0) {\n"
    "            // Composite the projection layers (MergeMat) bottom-to-top. A lone Project3D is\n"
    "            // one layer (op=over), reproducing the single-projection result. acc is premult.\n"
    "            vec4 acc = vec4(0.0);\n"
    "            if (0 < u_numProj) acc = mm_composite(mm_sampleProj(0, u_projPlate0), acc, u_projOp[0], u_projMix[0]);\n"
    "            if (1 < u_numProj) acc = mm_composite(mm_sampleProj(1, u_projPlate1), acc, u_projOp[1], u_projMix[1]);\n"
    "            if (2 < u_numProj) acc = mm_composite(mm_sampleProj(2, u_projPlate2), acc, u_projOp[2], u_projMix[2]);\n"
    "            if (3 < u_numProj) acc = mm_composite(mm_sampleProj(3, u_projPlate3), acc, u_projOp[3], u_projMix[3]);\n"
    "            // Nothing projected here (all layers cropped away) -> transparent, no depth write,\n"
    "            // so the geo is cropped and whatever's behind shows through.\n"
    "            if (acc.a < 0.001) discard;\n"
    "            baseColor = acc;\n"
    "        } else if (u_hasTexture == 2) {\n"
    "            if (v_stw.w <= 0.0) discard;\n"
    "            vec2 uv = v_stw.xy / v_stw.w;\n"
    "            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n"
    "                baseColor = vec4(0.0);\n"
    "            } else {\n"
    "                baseColor = texture(u_tex, uv);\n"
    "            }\n"
    "        } else if (u_hasTexture == 1) {\n"
    "            baseColor = texture(u_tex, v_uv);\n"
    "        } else {\n"
    "            baseColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
    "        }\n"
    "        if (u_shadingMode == 0) {\n"
    "            // --- Shaded: N.L diffuse + ambient ---\n"
    "            vec3 N = normalize(v_worldNormal);\n"
    "            vec3 L;\n"
    "            vec3 lCol;\n"
    "            float lInt;\n"
    "            if (u_hasLight == 1) {\n"
    "                L    = normalize(u_lightPos - v_worldPos);\n"
    "                lCol = u_lightColor;\n"
    "                lInt = u_lightIntensity;\n"
    "            } else {\n"
    // No Light3D connected — fall back to a camera-relative headlight (light
    // sits at the camera, follows the camera as it moves). Same convention as
    // Maya's default viewport: the side facing the camera is always lit and
    // the shape stays readable from any angle, no fixed world-space direction.
    "                L    = normalize(u_cameraPos - v_worldPos);\n"
    "                lCol = vec3(1.0, 1.0, 1.0);\n"
    "                lInt = 1.0;\n"
    "            }\n"
    "            float NL      = max(0.0, dot(N, L));\n"
    "            vec3 lit      = baseColor.rgb * (u_ambient + NL * lCol * lInt);\n"
    "            out_color     = vec4(lit, baseColor.a);\n"
    "        } else {\n"
    "            // --- Flat: no lighting, just base color ---\n"
    "            out_color = baseColor;\n"
    "        }\n"
    "    }\n"
    "    // Transparency off: force opaque coverage (ignore surface alpha).\n"
    "    if (u_transparency == 0) out_color.a = 1.0;\n"
    "    // --- Normal AOV ---\n"
    "    if (u_writeNormal == 1) {\n"
    "        vec3 n = normalize(v_worldNormal);\n"
    "        out_normal = vec4(n, 1.0);\n"
    "    } else { out_normal = vec4(0.0); }\n"
    "    // --- UV AOV ---\n"
    "    if (u_writeUV == 1) {\n"
    "        out_uv = vec4(v_uv, 0.0, 1.0);\n"
    "    } else { out_uv = vec4(0.0); }\n"
    "    // --- Pref AOV (object-space position, before localMatrix) ---\n"
    "    if (u_writePref == 1) {\n"
    "        out_pref = vec4(v_prefPos, 1.0);\n"
    "    } else { out_pref = vec4(0.0); }\n"
    "    // --- Velocity AOV (screen-pixels delta between previous + current frame) ---\n"
    "    if (u_writeVelocity == 1) {\n"
    "        vec2 currNdc = v_currClipPos.xy / v_currClipPos.w;\n"
    "        vec2 prevNdc = v_prevClipPos.xy / v_prevClipPos.w;\n"
    "        vec2 deltaNdc = currNdc - prevNdc;\n"
    "        vec2 deltaPx = deltaNdc * (u_viewportSize * 0.5);\n"
    "        out_velocity = vec4(deltaPx, 0.0, 1.0);\n"
    "    } else { out_velocity = vec4(0.0); }\n"
    "}\n";

// ==================== Particle shaders (Phase 3E) ====================
//
// One shader pair drives every particle draw call (Point, Disc, Sphere, Sprite,
// and motion-blur streak lines). Particles only ever write the beauty plane —
// AOVs are gated to GL_NONE for the particle pass via glDrawBuffers, so no AOV
// outputs in the fragment shader. Three uniforms:
//   u_projView   — projection * view, same as the mesh shader
//   u_pointMode  — 0 for lines / quads, 1 for round point sprites (discards
//                  fragments outside the unit circle using gl_PointCoord)
//   u_hasTexture — 0 for solid color (Point/Disc/Sphere/streak), 1 for sprites
//                  (Sprite mode samples u_tex modulated by per-vertex color)

static const char* kParticleVert =
    "#version 330 core\n"
    "layout(location = 0) in vec3 in_pos;\n"
    "layout(location = 1) in vec4 in_color;\n"
    "layout(location = 2) in vec3 in_normal;\n"
    "layout(location = 3) in vec2 in_uv;\n"
    "layout(location = 4) in vec3 in_pref;\n"
    "layout(location = 5) in vec3 in_velocity;   // world-space per-frame displacement\n"
    "uniform mat4 u_projView;\n"
    "uniform mat4 u_prevProjView;                // camera at time-1 (for velocity AOV)\n"
    "out vec4 v_color;\n"
    "out vec3 v_normal;\n"
    "out vec2 v_uv;\n"
    "out vec3 v_pref;\n"
    "out vec4 v_currClip;\n"
    "out vec4 v_prevClip;\n"
    "void main() {\n"
    "    gl_Position = u_projView * vec4(in_pos, 1.0);\n"
    "    v_color     = in_color;\n"
    "    v_normal    = in_normal;\n"
    "    v_uv        = in_uv;\n"
    "    v_pref      = in_pref;\n"
    "    v_currClip  = gl_Position;\n"
    // For the velocity AOV: prev position is the particle's current position
    // minus its per-frame displacement vector. Sufficient under the assumption
    // that per-particle motion is linear between frames (no curved paths).
    "    v_prevClip  = u_prevProjView * vec4(in_pos - in_velocity, 1.0);\n"
    "}\n";

// Particle fragment shader — beauty + MRT AOVs. Same pattern as the mesh
// beauty shader (kBeautyFrag): per-AOV uniform gates so unwanted attachments
// still need to be written to satisfy MRT, but the caller controls which ones
// actually carry data via u_writeXxx.
static const char* kParticleFrag =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "in vec3 v_normal;\n"
    "in vec2 v_uv;\n"
    "in vec3 v_pref;\n"
    "in vec4 v_currClip;\n"
    "in vec4 v_prevClip;\n"
    "uniform int  u_writeNormal;\n"
    "uniform int  u_writeUV;\n"
    "uniform int  u_writePref;\n"
    "uniform int  u_writeVelocity;\n"
    "uniform vec2 u_resolution;\n"
    "layout(location = 0) out vec4 FragColor;\n"
    "layout(location = 1) out vec4 out_normal;\n"
    "layout(location = 2) out vec4 out_uv;\n"
    "layout(location = 3) out vec4 out_pref;\n"
    "layout(location = 4) out vec4 out_velocity;\n"
    "void main() {\n"
    "    FragColor = v_color;\n"
    "    if (u_writeNormal == 1) {\n"
    "        vec3 n = v_normal;\n"
    "        float L = length(n);\n"
    "        if (L > 1e-6) n = n / L;\n"
    "        out_normal = vec4(n, 1.0);\n"
    "    } else { out_normal = vec4(0.0); }\n"
    "    if (u_writeUV == 1) {\n"
    "        out_uv = vec4(v_uv, 0.0, 1.0);\n"
    "    } else { out_uv = vec4(0.0); }\n"
    "    if (u_writePref == 1) {\n"
    "        out_pref = vec4(v_pref, 1.0);\n"
    "    } else { out_pref = vec4(0.0); }\n"
    "    if (u_writeVelocity == 1) {\n"
    "        vec2 currNdc = v_currClip.xy / v_currClip.w;\n"
    "        vec2 prevNdc = v_prevClip.xy / v_prevClip.w;\n"
    "        vec2 dPix    = (currNdc - prevNdc) * 0.5 * u_resolution;\n"
    "        out_velocity = vec4(dPix.x, dPix.y, 0.0, 1.0);\n"
    "    } else { out_velocity = vec4(0.0); }\n"
    "}\n";

// Per-particle vertex. Position + color drive beauty; the rest drive the AOV
// MRT outputs (Phase 3E.5). Default-zeroed so existing 7-field brace init still
// compiles — AOV fields are then set explicitly by each per-mode loop.
struct ParticleVertex {
    float x, y, z;
    float r, g, b, a;
    float nx = 0, ny = 0, nz = 0;   // world-space surface normal
    float u = 0, v = 0;             // texture coordinate (natural where defined, else 0)
    float prefX = 0, prefY = 0, prefZ = 0;  // reference position (per-particle world pos)
    float velX = 0, velY = 0, velZ = 0;     // world-space per-frame displacement
};

// Render either GL_POINTS or GL_LINES of particles via the GLSL pipeline.
// `verts` is interleaved {pos.xyz, color.rgba} per vertex. The caller sets
// up program/uniforms/blend/depth/draw-buffers before calling and tears
// them down after.
static void
drawParticlePrimitives(GLenum primitive, const std::vector<ParticleVertex>& verts)
{
    if (verts.empty()) return;
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(verts.size() * sizeof(ParticleVertex)),
                 verts.data(), GL_STREAM_DRAW);
    // location 0..5: pos, color, normal, uv, pref, velocity
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex),
                          (void*)(7 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex),
                          (void*)(10 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex),
                          (void*)(12 * sizeof(float)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex),
                          (void*)(15 * sizeof(float)));

    glDrawArrays(primitive, 0, (GLsizei)verts.size());

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

// ==================== ParticleInstance shaders ====================
//
// Dedicated shader pair for the ParticleInstance draw path. Modeled on
// kBeautyVert/Frag but trimmed to instance concerns:
//   - no texture / no STW projective texturing — instances are solid-colored
//   - no per-vertex UV / Pref AOVs — instances don't expose those
//   - per-instance color comes via u_instanceColor uniform
//   - cheat-mode motion-blur stretch fade computed in the vertex shader from
//     three uniforms (u_fadeEnabled, u_fadeCenterY, u_fadeHalfY) — matches
//     the legacy immediate-mode behaviour exactly. Multi-sample mode passes
//     u_fadeEnabled=0 and gets uniform alpha.
//
// Vertices are kept in object-local space (one VBO per geo type, built once
// per frame). Per-instance state lives in uniforms (u_localMatrix /
// u_normalMatrix / u_instanceColor / fade params), so one VBO is reused
// across all instances of a geo type via N small draw calls.

static const char* kInstanceVert =
    "#version 330 core\n"
    "layout(location = 0) in vec3 in_pos;\n"
    "layout(location = 1) in vec3 in_normal;\n"
    "layout(location = 2) in vec2 in_uv;\n"
    "uniform mat4 u_projView;\n"
    "uniform mat4 u_prevProjView;\n"
    "uniform mat4 u_localMatrix;\n"
    "uniform mat4 u_prevLocalMatrix;\n"
    "uniform mat3 u_normalMatrix;\n"
    "uniform vec4 u_instanceColor;\n"
    "uniform int  u_fadeEnabled;     // 1 = apply stretch-mode alpha fade based on object-Y\n"
    "uniform float u_fadeCenterY;\n"
    "uniform float u_fadeHalfY;\n"
    "uniform int  u_projMode;        // 0/1 = matrix, 2 = UV bake, 3 = spherical\n"
    "uniform mat4 u_view;            // world->view, for spherical projection\n"
    "uniform float u_near;\n"
    "uniform float u_far;\n"
    "out vec3 v_worldNormal;\n"
    "out vec3 v_worldPos;\n"
    "out vec3 v_objPos;              // object-space position — drives Pref AOV\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "out vec4 v_currClip;\n"
    "out vec4 v_prevClip;\n"
    "void main() {\n"
    "    vec4 worldPos4 = u_localMatrix * vec4(in_pos, 1.0);\n"
    "    v_worldPos    = worldPos4.xyz;\n"
    "    v_objPos      = in_pos;\n"
    "    v_uv          = in_uv;\n"
    "    v_worldNormal = u_normalMatrix * in_normal;\n"
    "    v_currClip    = u_projView * worldPos4;\n"
    "    v_prevClip    = u_prevProjView * (u_prevLocalMatrix * vec4(in_pos, 1.0));\n"
    "    if (u_projMode == 2) {\n"
    "        gl_Position = vec4(in_uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "    } else if (u_projMode == 3) {\n"
    "        vec3 vp = (u_view * worldPos4).xyz;\n"
    "        float r = length(vp);\n"
    "        float lon = atan(vp.x, -vp.z);\n"
    "        float lat = asin(clamp(vp.y / max(r, 1e-6), -1.0, 1.0));\n"
    "        float ndcZ = clamp((r - u_near) / max(u_far - u_near, 1e-6), 0.0, 1.0) * 2.0 - 1.0;\n"
    "        gl_Position = vec4(lon / 3.14159265, lat / 1.5707963, ndcZ, 1.0);\n"
    "    } else {\n"
    "        gl_Position = v_currClip;\n"
    "    }\n"
    // Cheat-mode stretch fade: normalize object-Y to [-1, 1] across bbox, fade
    // alpha toward the stretched ends. Matches the legacy immediate-mode code
    // verbatim (1.0 - clamp(|normY|, 0, 1) * 0.7 → 30% min alpha at extremes).
    "    float fade = 1.0;\n"
    "    if (u_fadeEnabled == 1) {\n"
    "        float normY = (in_pos.y - u_fadeCenterY) / u_fadeHalfY;\n"
    "        float t01   = clamp(abs(normY), 0.0, 1.0);\n"
    "        fade        = 1.0 - t01 * 0.7;\n"
    "    }\n"
    "    v_color = vec4(u_instanceColor.rgb, u_instanceColor.a * fade);\n"
    "}\n";

static const char* kInstanceFrag =
    "#version 330 core\n"
    "in vec3 v_worldNormal;\n"
    "in vec3 v_worldPos;\n"
    "in vec3 v_objPos;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "in vec4 v_currClip;\n"
    "in vec4 v_prevClip;\n"
    "uniform int  u_shadingMode;     // 0=Shaded, 1=Flat, 2=Wireframe\n"
    "uniform int  u_hasLight;\n"
    "uniform vec3 u_lightPos;\n"
    "uniform vec3 u_lightColor;\n"
    "uniform float u_lightIntensity;\n"
    "uniform vec3 u_cameraPos;\n"
    "uniform vec3 u_ambient;\n"
    "uniform int  u_transparency;\n"
    "uniform int  u_writeNormal;\n"
    "uniform int  u_writeUV;\n"
    "uniform int  u_writePref;\n"
    "uniform int  u_writeVelocity;\n"
    "uniform vec2 u_viewportSize;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "layout(location = 1) out vec4 out_normal;\n"
    "layout(location = 2) out vec4 out_uv;\n"
    "layout(location = 3) out vec4 out_pref;\n"
    "layout(location = 4) out vec4 out_velocity;\n"
    "void main() {\n"
    "    if (u_shadingMode == 2) {\n"
    "        out_color = vec4(1.0);\n"
    "    } else if (u_shadingMode == 0) {\n"
    "        vec3 N = normalize(v_worldNormal);\n"
    "        vec3 L; vec3 lCol; float lInt;\n"
    "        if (u_hasLight == 1) {\n"
    "            L = normalize(u_lightPos - v_worldPos); lCol = u_lightColor; lInt = u_lightIntensity;\n"
    "        } else {\n"
    "            L = normalize(u_cameraPos - v_worldPos); lCol = vec3(1.0); lInt = 1.0;\n"
    "        }\n"
    "        float NL = max(0.0, dot(N, L));\n"
    "        vec3 lit = v_color.rgb * (u_ambient + NL * lCol * lInt);\n"
    "        out_color = vec4(lit, v_color.a);\n"
    "    } else {\n"
    "        out_color = v_color;\n"
    "    }\n"
    "    if (u_transparency == 0) out_color.a = 1.0;\n"
    "    out_normal = (u_writeNormal == 1)   ? vec4(normalize(v_worldNormal), 1.0) : vec4(0.0);\n"
    "    out_uv     = (u_writeUV == 1)       ? vec4(v_uv, 0.0, 1.0)                 : vec4(0.0);\n"
    "    out_pref   = (u_writePref == 1)     ? vec4(v_objPos, 1.0)                  : vec4(0.0);\n"
    "    if (u_writeVelocity == 1) {\n"
    "        vec2 cNdc = v_currClip.xy / v_currClip.w;\n"
    "        vec2 pNdc = v_prevClip.xy / v_prevClip.w;\n"
    "        vec2 dPix = (cNdc - pNdc) * 0.5 * u_viewportSize;\n"
    "        out_velocity = vec4(dPix, 0.0, 1.0);\n"
    "    } else { out_velocity = vec4(0.0); }\n"
    "}\n";

// ==================== Volume ray marching shaders ====================
// GLSL 3.30 core profile, uniform-driven matrices. The draw site uploads
// u_modelView / u_projView from the current camera state directly; no
// dependency on the fixed-function matrix stack.

static const char* volumeVertexShader =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_position;\n"
    "uniform mat4 u_modelView;\n"
    "uniform mat4 u_projView;\n"
    "out vec3 v_WorldPos;\n"
    "void main() {\n"
    // Same math as the legacy shader — `v_WorldPos` is actually view-space
    // (modelview = camera view matrix since we never set an extra model
    // transform), but the fragment shader's ray-march logic was written
    // assuming this convention. Preserved verbatim to keep behaviour
    // identical to the legacy path.
    "    v_WorldPos  = vec3(u_modelView * vec4(a_position, 1.0));\n"
    "    gl_Position = u_projView * vec4(a_position, 1.0);\n"
    "}\n";

static const char* volumeFragmentShader =
    "#version 330 core\n"
    "in vec3 v_WorldPos;\n"
    "uniform sampler3D u_VolumeData;\n"
    "uniform vec3 u_VolumeMin;\n"
    "uniform vec3 u_VolumeMax;\n"
    "uniform vec3 u_CameraPos;\n"
    "uniform float u_Density;\n"
    "uniform vec3 u_VolumeColor;\n"
    "uniform float u_StepSize;\n"
    "uniform vec3 u_LightPos;\n"
    "uniform vec3 u_LightColor;\n"
    "uniform float u_LightIntensity;\n"
    "uniform float u_ShadowDensity;\n"
    "uniform int u_ShadowSteps;\n"
    "uniform int u_LightEnabled;\n"
    "out vec4 outColor;\n"
    "\n"
    "vec2 intersectBox(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax) {\n"
    "    vec3 invR = vec3(1.0) / rd;\n"
    "    vec3 t0 = (bmin - ro) * invR;\n"
    "    vec3 t1 = (bmax - ro) * invR;\n"
    "    vec3 tmin = min(t0, t1);\n"
    "    vec3 tmax = max(t0, t1);\n"
    "    float tNear = max(max(tmin.x, tmin.y), tmin.z);\n"
    "    float tFar = min(min(tmax.x, tmax.y), tmax.z);\n"
    "    return vec2(tNear, tFar);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec3 rayDir = normalize(v_WorldPos - u_CameraPos);\n"
    "    vec2 tHit = intersectBox(u_CameraPos, rayDir, u_VolumeMin, u_VolumeMax);\n"
    "    float tNear = max(tHit.x, 0.0);\n"
    "    float tFar = tHit.y;\n"
    "    if (tNear >= tFar) discard;\n"
    "\n"
    "    vec4 accum = vec4(0.0);\n"
    "    vec3 boxSize = u_VolumeMax - u_VolumeMin;\n"
    "\n"
    "    for (float t = tNear; t < tFar; t += u_StepSize) {\n"
    "        vec3 pos = u_CameraPos + rayDir * t;\n"
    "        vec3 texCoord = (pos - u_VolumeMin) / boxSize;\n"
    "        float samp = texture(u_VolumeData, texCoord).r;\n"
    "        if (samp < 0.001) continue;\n"
    "\n"
    "        float d = samp * u_Density * u_StepSize;\n"
    "        vec3 col = u_VolumeColor;\n"
    "\n"
    "        // Lighting with shadow ray\n"
    "        if (u_LightEnabled > 0) {\n"
    "            vec3 lightDir = normalize(u_LightPos - pos);\n"
    "            float lightDist = length(u_LightPos - pos);\n"
    "            float shadowStep = lightDist / float(u_ShadowSteps);\n"
    "            float shadowAccum = 0.0;\n"
    "            for (int s = 1; s <= u_ShadowSteps; s++) {\n"
    "                vec3 shadowPos = pos + lightDir * shadowStep * float(s);\n"
    "                vec3 shadowTC = (shadowPos - u_VolumeMin) / boxSize;\n"
    "                if (shadowTC.x >= 0.0 && shadowTC.x <= 1.0 &&\n"
    "                    shadowTC.y >= 0.0 && shadowTC.y <= 1.0 &&\n"
    "                    shadowTC.z >= 0.0 && shadowTC.z <= 1.0) {\n"
    "                    shadowAccum += texture(u_VolumeData, shadowTC).r * shadowStep;\n"
    "                }\n"
    "            }\n"
    "            float lightAmount = exp(-shadowAccum * u_ShadowDensity);\n"
    "            col = u_VolumeColor * u_LightColor * lightAmount * u_LightIntensity;\n"
    "        }\n"
    "\n"
    "        vec4 sampleColor = vec4(col * d, d);\n"
    "        accum.rgb += (1.0 - accum.a) * sampleColor.rgb;\n"
    "        accum.a += (1.0 - accum.a) * sampleColor.a;\n"
    "        if (accum.a > 0.98) break;\n"
    "    }\n"
    "    outColor = accum;\n"
    "}\n";

// ==================== Geometry extraction helper ====================

// One camera projection layer applied to a geo (a single Project3D, or one leaf of a
// MergeMat tree). Multiple layers composite per-fragment in the shader, bottom (index 0)
// to top, each by its `op` (MergeMat::Operation). A lone Project3D = one layer (op=over).
struct ProjLayer {
    float    VP[16];        // world -> projector clip
    float    forward[3];    // projector forward direction (world), for front/back
    int      projOn;        // 0 front, 1 back, 2 both
    int      crop;          // 1 = transparent outside the plate frame
    float    pNear, pFar;   // projector-space depth crop
    int      op;            // MergeMat::Operation compositing this over the accumulation below
    float    mix;           // foreground opacity (0..1)
    ImagePtr plateImg;      // the plate to project for this layer
    ProjLayer() : projOn(2), crop(1), pNear(0.1f), pFar(10000.f), op(2 /*over*/), mix(1.f)
    {
        for (int i = 0; i < 16; ++i) VP[i] = (i % 5 == 0) ? 1.f : 0.f;
        forward[0] = 0.f; forward[1] = 0.f; forward[2] = -1.f;
    }
};

struct GeoData {
    std::vector<float> verts;    // x,y,z interleaved
    std::vector<float> uvs;      // u,v interleaved (used when stw is empty)
    std::vector<float> stw;      // s,t,w interleaved — populated by UVProject (Perspective +
                                 //   Generate Perspective). When non-empty, takes precedence
                                 //   over uvs and the render loop emits glTexCoord4f(s,t,0,w)
                                 //   so OpenGL does perspective-correct fragment-level divide.
    std::vector<int> triIndices;
    float localMatrix[16];
    ImagePtr texImg;

    // Phase 3 GLSL/MRT extensions. Empty / identity for fixed-function path.
    // Phase 3D populates these for the Normal / Pref / Velocity AOVs:
    //   normals          — object-space per-vertex normals (xyz interleaved)
    //   prevVerts        — previous-frame object-space positions (xyz interleaved)
    //   prevLocalMatrix  — previous-frame local→world transform (col-major)
    std::vector<float> normals;
    std::vector<float> prevVerts;
    float prevLocalMatrix[16];

    // Project3D (camera-projected material). Filled by applyProjectorMaterial when this
    // geo's material input is a Project3D with a camera + plate.
    bool     useProjector;
    float    projectorVP[16];  // projector view*projection (world -> projector clip)
    float    projForward[3];   // projector forward direction (world), for front/back
    int      projOn;           // 0 front, 1 back, 2 both
    int      projCrop;         // 1 = transparent outside the plate frame
    float    projNear, projFar;// projector-space depth crop
    int      occMode;          // 0 none, 1 self, 2 world
    GLuint   occDepthTex;      // projector depth map (0 = not built -> occlusion inactive)
    ImagePtr projPlateImg;     // the plate to project (layer 0 — kept for occlusion build)

    // Layered projections (MergeMat). A lone Project3D fills exactly one layer; a MergeMat
    // tree flattens (bottom-to-top) into several. The shader composites them per-fragment.
    std::vector<ProjLayer> projLayers;

    GeoData()
        : useProjector(false), projOn(0), projCrop(1), projNear(0.1f), projFar(10000.f),
          occMode(0), occDepthTex(0)
    {
        for (int i = 0; i < 16; ++i) prevLocalMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        for (int i = 0; i < 16; ++i) projectorVP[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        projForward[0] = 0.f; projForward[1] = 0.f; projForward[2] = -1.f;
    }
};

// Fan-triangulate a polygon-soup mesh (faceIndices + per-face vertex counts)
// into a flat triangle index array. Required: most DCC exports (Maya / Houdini /
// Blender Alembic) store quads or n-gons, NOT triangles. Treating faceIndices
// as raw triangle indices crosses face boundaries and produces garbage.
static void
fanTriangulate(const std::vector<int>& faceIndices,
               const std::vector<int>& faceCounts,
               std::vector<int>& outTris)
{
    outTris.clear();
    if (faceCounts.empty()) {
        // No face-count info — assume input is already triangulated.
        outTris = faceIndices;
        return;
    }
    // Upper bound: every face contributes (count-2) triangles → (count-2)*3 ints.
    // For an all-quad mesh that's (4-2)*3 = 6 per face. Reserve generously.
    outTris.reserve(faceIndices.size() * 2);

    size_t offset = 0;
    for (size_t f = 0; f < faceCounts.size(); ++f) {
        const int c = faceCounts[f];
        if (c < 3 || offset + (size_t)c > faceIndices.size()) {
            offset += (size_t)std::max(0, c);
            continue;
        }
        const int v0 = faceIndices[offset];
        for (int i = 1; i + 1 < c; ++i) {
            outTris.push_back(v0);
            outTris.push_back(faceIndices[offset + i]);
            outTris.push_back(faceIndices[offset + i + 1]);
        }
        offset += (size_t)c;
    }
}

// Render the volume ray-march proxy cube via a transient VBO
// + glDrawArrays. Replaces the legacy glBegin(GL_QUADS) immediate-mode
// path. Caller must have bound the volume shader and set its uniforms
// (u_modelView / u_projView / volume params) before invoking.
//
// 12 triangles (6 faces × 2 tris) = 36 vertices. Cheap enough to upload
// each call; the alternative (static unit-cube VBO with a per-instance
// transform uniform) is more efficient but adds state-management overhead
// for what's typically a once-per-frame, 36-vertex draw call.
static void
drawVolumeProxyCube(float x0, float y0, float z0,
                    float x1, float y1, float z1)
{
    const float v[] = {
        // -Z face (CCW when viewed from -Z)
        x0,y0,z0,  x1,y0,z0,  x1,y1,z0,   x0,y0,z0,  x1,y1,z0,  x0,y1,z0,
        // +Z face
        x0,y0,z1,  x0,y1,z1,  x1,y1,z1,   x0,y0,z1,  x1,y1,z1,  x1,y0,z1,
        // -X face
        x0,y0,z0,  x0,y1,z0,  x0,y1,z1,   x0,y0,z0,  x0,y1,z1,  x0,y0,z1,
        // +X face
        x1,y0,z0,  x1,y0,z1,  x1,y1,z1,   x1,y0,z0,  x1,y1,z1,  x1,y1,z0,
        // -Y face
        x0,y0,z0,  x0,y0,z1,  x1,y0,z1,   x0,y0,z0,  x1,y0,z1,  x1,y0,z0,
        // +Y face
        x0,y1,z0,  x1,y1,z0,  x1,y1,z1,   x0,y1,z0,  x1,y1,z1,  x0,y1,z1,
    };

    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); // a_position (layout(location = 0))
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    glDrawArrays(GL_TRIANGLES, 0, 36);

    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

// Compute per-vertex normals from triangle indices via face-normal averaging.
// Fallback for mesh sources (ReadGeo, ReadAlembicArchive) that don't carry an
// explicit per-vertex normal channel. Without this the GLSL Normal AOV reads
// (0,0,0) from the vertex attribute, normalize() returns NaN, and the AOV
// renders as solid white (NaN in a float framebuffer).
//
// Algorithm: for each triangle (v0,v1,v2), compute face normal via cross
// product, accumulate to each contributing vertex, then normalize per-vertex.
// Winding is whatever the upstream mesh exported; renderers that care
// (ScanlineRender Shaded mode) use `abs(N.L)` to be winding-agnostic.
static void
computeVertexNormalsFromTris(const std::vector<float>& verts,
                              const std::vector<int>& triIndices,
                              std::vector<float>& outNormals)
{
    const int nv = (int)(verts.size() / 3);
    if (nv <= 0 || triIndices.empty()) {
        outNormals.clear();
        return;
    }
    outNormals.assign((size_t)nv * 3, 0.0f);

    const size_t nTris = triIndices.size() / 3;
    for (size_t t = 0; t < nTris; ++t) {
        const int i0 = triIndices[t * 3 + 0];
        const int i1 = triIndices[t * 3 + 1];
        const int i2 = triIndices[t * 3 + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0 ||
            i0 >= nv || i1 >= nv || i2 >= nv) continue;

        const float* p0 = &verts[i0 * 3];
        const float* p1 = &verts[i1 * 3];
        const float* p2 = &verts[i2 * 3];
        const float ex = p1[0] - p0[0], ey = p1[1] - p0[1], ez = p1[2] - p0[2];
        const float fx = p2[0] - p0[0], fy = p2[1] - p0[1], fz = p2[2] - p0[2];
        const float nx = ey * fz - ez * fy;
        const float ny = ez * fx - ex * fz;
        const float nz = ex * fy - ey * fx;
        // Unnormalized cross product — accumulating raw values is equivalent
        // to area-weighted averaging, which is what we want for smooth shading.
        outNormals[i0 * 3 + 0] += nx; outNormals[i0 * 3 + 1] += ny; outNormals[i0 * 3 + 2] += nz;
        outNormals[i1 * 3 + 0] += nx; outNormals[i1 * 3 + 1] += ny; outNormals[i1 * 3 + 2] += nz;
        outNormals[i2 * 3 + 0] += nx; outNormals[i2 * 3 + 1] += ny; outNormals[i2 * 3 + 2] += nz;
    }

    for (int v = 0; v < nv; ++v) {
        float* n = &outNormals[v * 3];
        const float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (len2 > 1e-20f) {
            const float invLen = 1.0f / std::sqrt(len2);
            n[0] *= invLen; n[1] *= invLen; n[2] *= invLen;
        } else {
            // Degenerate (isolated vertex or zero-area faces) — pick a stable
            // fallback so the Normal AOV doesn't NaN out.
            n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f;
        }
    }
}

static bool
extractGeometry(EffectInstancePtr effect, double time, ViewIdx view, GeoData& out)
{
    out.verts.clear();
    out.uvs.clear();
    out.triIndices.clear();
    SceneGraph::buildTRS(0,0,0, 0,0,0, 1,1,1, out.localMatrix);
    out.texImg.reset();

    if (!effect) return false;
    // Honor "D" disable: a disabled node contributes nothing.
    {
        NodePtr n = effect->getNode();
        if (n && n->isNodeDisabled()) return false;
    }

    Sphere3D* sphere = dynamic_cast<Sphere3D*>(effect.get());
    if (sphere) {
        std::vector<Sphere3D::SphereVertex> sv;
        sphere->generateSphereMesh(time, sv, out.triIndices);
        out.verts.resize(sv.size() * 3);
        out.uvs.resize(sv.size() * 2);
        out.normals.resize(sv.size() * 3);
        for (size_t i = 0; i < sv.size(); ++i) {
            out.verts[i*3+0] = sv[i].x; out.verts[i*3+1] = sv[i].y; out.verts[i*3+2] = sv[i].z;
            out.uvs[i*2+0] = sv[i].u; out.uvs[i*2+1] = sv[i].v;
            out.normals[i*3+0] = sv[i].nx; out.normals[i*3+1] = sv[i].ny; out.normals[i*3+2] = sv[i].nz;
        }
        double tx,ty,tz,rx,ry,rz,sx,sy,sz;
        sphere->getSphereTransform(time, tx,ty,tz, rx,ry,rz, sx,sy,sz);
        SceneGraph::buildTRS((float)tx,(float)ty,(float)tz, (float)rx,(float)ry,(float)rz, (float)sx,(float)sy,(float)sz, out.localMatrix);
        if (sphere->getInput(0)) {
            RectI roi;
            out.texImg = sphere->getImage(0, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    Card3D* card = dynamic_cast<Card3D*>(effect.get());
    if (card) {
        std::vector<Card3D::CardVertex> cv;
        card->generateCardMesh(time, cv, out.triIndices);
        out.verts.resize(cv.size() * 3);
        out.uvs.resize(cv.size() * 2);
        out.normals.resize(cv.size() * 3);
        for (size_t i = 0; i < cv.size(); ++i) {
            out.verts[i*3+0] = cv[i].x; out.verts[i*3+1] = cv[i].y; out.verts[i*3+2] = cv[i].z;
            out.uvs[i*2+0] = cv[i].u; out.uvs[i*2+1] = cv[i].v;
            out.normals[i*3+0] = cv[i].nx; out.normals[i*3+1] = cv[i].ny; out.normals[i*3+2] = cv[i].nz;
        }
        double tx,ty,tz,rx,ry,rz,sx,sy;
        card->getCardTransform(time, tx,ty,tz, rx,ry,rz, sx,sy);
        SceneGraph::buildTRS((float)tx,(float)ty,(float)tz, (float)rx,(float)ry,(float)rz, (float)sx,(float)sy,1.0f, out.localMatrix);
        if (card->getInput(0)) {
            RectI roi;
            out.texImg = card->getImage(0, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    Cube3D* cube = dynamic_cast<Cube3D*>(effect.get());
    if (cube) {
        std::vector<Cube3D::CubeVertex> cv;
        cube->generateCubeMesh(time, cv, out.triIndices);
        out.verts.resize(cv.size() * 3);
        out.uvs.resize(cv.size() * 2);
        out.normals.resize(cv.size() * 3);
        for (size_t i = 0; i < cv.size(); ++i) {
            out.verts[i*3+0] = cv[i].x; out.verts[i*3+1] = cv[i].y; out.verts[i*3+2] = cv[i].z;
            out.uvs[i*2+0] = cv[i].u; out.uvs[i*2+1] = cv[i].v;
            out.normals[i*3+0] = cv[i].nx; out.normals[i*3+1] = cv[i].ny; out.normals[i*3+2] = cv[i].nz;
        }
        double tx,ty,tz,rx,ry,rz,sx,sy,sz;
        cube->getCubeTransform(time, tx,ty,tz, rx,ry,rz, sx,sy,sz);
        SceneGraph::buildTRS((float)tx,(float)ty,(float)tz, (float)rx,(float)ry,(float)rz, (float)sx,(float)sy,(float)sz, out.localMatrix);
        if (cube->getInput(0)) {
            RectI roi;
            out.texImg = cube->getImage(0, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    Cylinder3D* cyl = dynamic_cast<Cylinder3D*>(effect.get());
    if (cyl) {
        std::vector<Cylinder3D::CylinderVertex> cv;
        cyl->generateCylinderMesh(time, cv, out.triIndices);
        out.verts.resize(cv.size() * 3);
        out.uvs.resize(cv.size() * 2);
        out.normals.resize(cv.size() * 3);
        for (size_t i = 0; i < cv.size(); ++i) {
            out.verts[i*3+0] = cv[i].x; out.verts[i*3+1] = cv[i].y; out.verts[i*3+2] = cv[i].z;
            out.uvs[i*2+0] = cv[i].u; out.uvs[i*2+1] = cv[i].v;
            out.normals[i*3+0] = cv[i].nx; out.normals[i*3+1] = cv[i].ny; out.normals[i*3+2] = cv[i].nz;
        }
        double tx,ty,tz,rx,ry,rz,sx,sy,sz;
        cyl->getCylinderTransform(time, tx,ty,tz, rx,ry,rz, sx,sy,sz);
        SceneGraph::buildTRS((float)tx,(float)ty,(float)tz, (float)rx,(float)ry,(float)rz, (float)sx,(float)sy,(float)sz, out.localMatrix);
        if (cyl->getInput(0)) {
            RectI roi;
            out.texImg = cyl->getImage(0, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    ReadGeo* readGeo = dynamic_cast<ReadGeo*>(effect.get());
    if (readGeo) {
        MeshDataPtr mesh = readGeo->getMeshData(time);
        if (!mesh || mesh->numVertices == 0) return false;
        out.verts = mesh->vertices;
        fanTriangulate(mesh->faceIndices, mesh->faceCounts, out.triIndices);
        const int nv = (int)(out.verts.size() / 3);

        // Per-vertex normals via face-cross-product averaging — needed by the
        // Normal AOV (without this, the AOV NaNs out → solid white). MeshData
        // currently doesn't carry an explicit normals channel, so we compute
        // them here from the triangulated topology.
        computeVertexNormalsFromTris(out.verts, out.triIndices, out.normals);

        // Per-vertex UVs from the per-face-vertex array. First occurrence of each
        // vertex wins (lossy for UV seams; correct for typical clean DMP meshes).
        out.uvs.assign(nv * 2, 0.5f);
        if (mesh->hasUVs && mesh->uvs.size() == mesh->faceIndices.size() * 2) {
            std::vector<char> set((size_t)nv, 0);
            for (size_t i = 0; i < mesh->faceIndices.size(); ++i) {
                const int v = mesh->faceIndices[i];
                if (v >= 0 && v < nv && !set[v]) {
                    out.uvs[v * 2 + 0] = mesh->uvs[i * 2 + 0];
                    out.uvs[v * 2 + 1] = mesh->uvs[i * 2 + 1];
                    set[v] = 1;
                }
            }
        }

        // Embedded transform from the source file (Alembic baked xform, or
        // identity for OBJ / static .abc with no xform). Row-major in
        // mesh->transform → column-major in localMatrix.
        float embedded[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                embedded[c * 4 + r] = mesh->transform[r * 4 + c];

        // Compose user TRS knobs (translateX/Y/Z, rotateX/Y/Z, scaleX/Y/Z)
        // on top of the embedded transform. Without this the ScanlineRender
        // output ignored the node's transform knobs — Cycles + SceneGraph
        // pulled them via getKnobByName, ScanlineRender silently used the
        // embedded matrix only. localMatrix = userTRS * embedded so user
        // edits move the whole imported geo regardless of its baked xform.
        auto readDouble = [&](const char* name, float fallback) -> float {
            KnobIPtr k = readGeo->getKnobByName(name);
            if (!k) return fallback;
            KnobDouble* kd = dynamic_cast<KnobDouble*>(k.get());
            return kd ? (float)kd->getValueAtTime(time) : fallback;
        };
        const float tx = readDouble("translateX", 0.0f);
        const float ty = readDouble("translateY", 0.0f);
        const float tz = readDouble("translateZ", 0.0f);
        const float rx = readDouble("rotateX", 0.0f);
        const float ry = readDouble("rotateY", 0.0f);
        const float rz = readDouble("rotateZ", 0.0f);
        const float sx = readDouble("scaleX", 1.0f);
        const float sy = readDouble("scaleY", 1.0f);
        const float sz = readDouble("scaleZ", 1.0f);
        float userTRS[16];
        SceneGraph::buildTRS(tx, ty, tz, rx, ry, rz, sx, sy, sz, userTRS);
        mat4Mul(out.localMatrix, userTRS, embedded);

        // Optional Image input (input 1) — per-mesh texture for the scanline.
        if (readGeo->getInput(1)) {
            RectI roi;
            out.texImg = readGeo->getImage(1, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }
        return true;
    }

    // CameraTracker: render the solved 3D points as locator octahedra so track
    // stick can be verified directly over the plate. Connect the CameraTracker
    // node into a Scene3D input (or straight into the obj input).
    CameraTrackerNode* camTracker = dynamic_cast<CameraTrackerNode*>(effect.get());
    if (camTracker) {
        MeshDataPtr mesh = camTracker->getLocatorMesh();
        if (!mesh || mesh->numVertices == 0) return false;
        out.verts = mesh->vertices;
        fanTriangulate(mesh->faceIndices, mesh->faceCounts, out.triIndices);
        computeVertexNormalsFromTris(out.verts, out.triIndices, out.normals);
        out.uvs.assign((out.verts.size() / 3) * 2, 0.5f);
        SceneGraph::buildTRS(0, 0, 0, 0, 0, 0, 1, 1, 1, out.localMatrix); // identity
        return true;
    }

    return false;
}

// Max number of projection layers a geo can carry (MergeMat depth). One texture unit per
// plate (units 1..N), so this stays well within the GL texture-unit budget.
static const int kMaxProjLayers = 4;

// Fill one ProjLayer from a Project3D (camera + plate). Returns false if the projection is
// inactive (no camera or no plate), in which case the layer should be skipped.
static bool
buildProjLayerFromProject3D(Project3D* proj, double time, ViewIdx view, int op, float mix, ProjLayer& out)
{
    double tx, ty, tz, rx, ry, rz, focal, hAp, vAp;
    if (!proj->getProjectorCamera(time, tx, ty, tz, rx, ry, rz, focal, hAp, vAp)) return false;

    const float pNear = (float)proj->getNearClip(time);
    const float pFar  = (float)proj->getFarClip(time);
    float pView[16], pProj[16];
    buildViewMatrix(tx, ty, tz, rx, ry, rz, pView);
    CameraMath::composeProjectionMatrix(focal, hAp, vAp, (pNear > 1e-4f ? pNear : 0.1f), pFar, pProj);
    mat4Mul(out.VP, pProj, pView);  // world -> projector clip
    out.forward[0] = -pView[2];
    out.forward[1] = -pView[6];
    out.forward[2] = -pView[10];
    out.projOn = proj->getProjectOn(time);
    out.crop   = proj->getCropToFrame(time) ? 1 : 0;
    out.pNear  = pNear;
    out.pFar   = pFar;
    out.op     = op;
    out.mix    = mix;

    RectI roi;
    out.plateImg = proj->getImage(0, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
    return (bool)out.plateImg;  // need a plate to project
}

// Append projection layers (bottom-to-top) for `mat` to `out`. `op` composites this
// material's contribution over the accumulation below it; `mix` is its opacity. Recurses
// through MergeMat trees: background (B) first, then foreground (A) over it with the
// MergeMat's operation. Only Project3D leaves project; flat materials add no layer.
static void
flattenMaterialLayers(MaterialProvider* mat, double time, ViewIdx view, int op, float mix,
                      std::vector<ProjLayer>& out)
{
    if (!mat || (int)out.size() >= kMaxProjLayers) return;
    if (Project3D* p = dynamic_cast<Project3D*>(mat)) {
        ProjLayer layer;
        if (buildProjLayerFromProject3D(p, time, view, op, mix, layer))
            out.push_back(layer);
        return;
    }
    if (MergeMat* m = dynamic_cast<MergeMat*>(mat)) {
        MaterialProvider* A = m->getInputMaterial(0);   // foreground
        MaterialProvider* B = m->getInputMaterial(1);   // background
        const int   mop  = m->getOperation(time);
        const float mmix = (float)m->getMix(time);
        flattenMaterialLayers(B, time, view, MergeMat::eMergeOver, 1.f, out);  // base
        flattenMaterialLayers(A, time, view, mop, mmix, out);                  // A over B
        return;
    }
    // Material3D / textures: not projected (no layer). Future enhancement.
}

// Wrapper that handles both single-emit nodes (one GeoData via the existing
// extractGeometry) and multi-emit nodes (ReadAlembicArchive — one GeoData per
// visible mesh entry, world transform composed from the archive's parent chain).
// Appends 0..N entries to `out`.
// If the geo's material input projects (a Project3D, or a MergeMat tree of Project3Ds),
// fill the projection layers of `g` so renderGeoObjectGlsl composites them onto it.
static void
applyProjectorMaterial(const EffectInstancePtr& geoEffect, double time, ViewIdx view, GeoData& g)
{
    if (!geoEffect) return;
    MaterialProvider* geoMat = dynamic_cast<MaterialProvider*>(geoEffect.get());
    if (!geoMat || !geoMat->hasMaterialInput()) return;
    MaterialProvider* mat = geoMat->getConnectedMaterial();
    if (!mat) return;

    // The bottom layer composites 'over' a transparent base, so a lone Project3D reproduces
    // the single-projection behaviour exactly.
    flattenMaterialLayers(mat, time, view, MergeMat::eMergeOver, 1.f, g.projLayers);
    if (g.projLayers.empty()) return;
    g.useProjector = true;

    // Mirror layer 0 into the legacy single-projector fields (used by the occlusion depth
    // build in render()). Occlusion applies to a single Project3D only — a MergeMat defers it.
    const ProjLayer& l0 = g.projLayers.front();
    for (int i = 0; i < 16; ++i) g.projectorVP[i] = l0.VP[i];
    g.projForward[0] = l0.forward[0]; g.projForward[1] = l0.forward[1]; g.projForward[2] = l0.forward[2];
    g.projOn = l0.projOn; g.projCrop = l0.crop; g.projNear = l0.pNear; g.projFar = l0.pFar;
    g.projPlateImg = l0.plateImg;
    if (Project3D* p = dynamic_cast<Project3D*>(mat)) g.occMode = p->getOcclusionMode(time);
    else g.occMode = 0;
}

static void
extractGeometries(EffectInstancePtr effect, double time, ViewIdx view, std::vector<GeoData>& out)
{
    if (!effect) return;
    // Honor "D" disable: skip the whole branch under a disabled node.
    {
        NodePtr n = effect->getNode();
        if (n && n->isNodeDisabled()) return;
    }

    // UVProject: transparent UV-rewriting wrapper. Walk through to the upstream
    // geo, extract it normally, then apply the rewrite to every GeoData produced.
    UVProject* uvProj = dynamic_cast<UVProject*>(effect.get());
    if (uvProj) {
        EffectInstancePtr upstream = uvProj->getGeoInput();
        if (!upstream) return;
        const size_t prevCount = out.size();
        extractGeometries(upstream, time, view, out);

        // Optional projection-image override via UVProject's input 2 (img).
        ImagePtr projImg;
        if (uvProj->getImgInput()) {
            RectI roi;
            projImg = uvProj->getImage(2, time, RenderScale(), view,
                                       NULL, NULL, false, true,
                                       eStorageModeRAM, 0, &roi);
        }

        for (size_t i = prevCount; i < out.size(); ++i) {
            GeoData& g = out[i];
            std::vector<float> newUVs;
            std::vector<float> newSTW;
            int newComp = 0;
            uvProj->rewriteUVs(g.verts, g.localMatrix, time, newUVs, newSTW, newComp);
            if (newComp == 3) {
                g.stw = std::move(newSTW);
                g.uvs.clear(); // stw takes precedence
            } else if (newComp == 2) {
                g.uvs = std::move(newUVs);
                g.stw.clear();
            }
            // newComp == 0: Mode == Off — keep g.uvs as extracted upstream.

            if (projImg) {
                g.texImg = projImg; // override upstream texture
            }
        }
        return;
    }

    ReadAlembicArchive* abcArchive = dynamic_cast<ReadAlembicArchive*>(effect.get());
    if (abcArchive) {
        ImagePtr sharedTex;
        if (abcArchive->getInput(1)) {
            RectI roi;
            sharedTex = abcArchive->getImage(1, time, RenderScale(), view, NULL, NULL, false, true, eStorageModeRAM, 0, &roi);
        }

        // Project3D / MergeMat on the archive's material input: the archive has
        // ONE material input shared by all entries, so build the projection
        // layers once and stamp them onto every entry's GeoData (the viewport
        // projects per-entry the same way; without this the projection showed
        // in the viewport but vanished in the ScanlineRender output).
        GeoData projTpl;
        applyProjectorMaterial(effect, time, view, projTpl);
        auto stampProjector = [&projTpl](GeoData& g) {
            if (!projTpl.useProjector) return;
            g.useProjector = true;
            g.projLayers = projTpl.projLayers;
            for (int m = 0; m < 16; ++m) g.projectorVP[m] = projTpl.projectorVP[m];
            for (int m = 0; m < 3; ++m) g.projForward[m] = projTpl.projForward[m];
            g.projOn = projTpl.projOn;
            g.projCrop = projTpl.projCrop;
            g.projNear = projTpl.projNear;
            g.projFar = projTpl.projFar;
            g.projPlateImg = projTpl.projPlateImg;
            g.occMode = projTpl.occMode;
        };

        const int count = abcArchive->getSceneNodeCount();
        for (int i = 0; i < count; ++i) {
            MeshDataPtr mesh = abcArchive->getMeshDataAt(i, time);
            if (!mesh || mesh->numVertices == 0) continue;

            GeoData g;
            SceneGraph::buildTRS(0,0,0, 0,0,0, 1,1,1, g.localMatrix);
            if (!abcArchive->getEntryWorldMatrix(i, time, g.localMatrix)) continue;

            g.verts = mesh->vertices;
            fanTriangulate(mesh->faceIndices, mesh->faceCounts, g.triIndices);
            const int nv = (int)(g.verts.size() / 3);

            // Per-vertex normals via face-cross-product averaging — needed
            // by the Normal AOV (without this, the AOV NaNs out → white).
            // Alembic's optional N property isn't read here; if the file
            // carries normals we'd want to prefer them in a future revision.
            computeVertexNormalsFromTris(g.verts, g.triIndices, g.normals);

            g.uvs.assign(nv * 2, 0.5f);
            if (mesh->hasUVs && mesh->uvs.size() == mesh->faceIndices.size() * 2) {
                std::vector<char> set((size_t)nv, 0);
                for (size_t k = 0; k < mesh->faceIndices.size(); ++k) {
                    const int v = mesh->faceIndices[k];
                    if (v >= 0 && v < nv && !set[v]) {
                        g.uvs[v * 2 + 0] = mesh->uvs[k * 2 + 0];
                        g.uvs[v * 2 + 1] = mesh->uvs[k * 2 + 1];
                        set[v] = 1;
                    }
                }
            }
            g.texImg = sharedTex;
            stampProjector(g);
            out.push_back(g);
        }
        return;
    }

    // Single-result path: delegate to the existing extractGeometry helper.
    GeoData g;
    if (extractGeometry(effect, time, view, g)) {
        applyProjectorMaterial(effect, time, view, g);
        out.push_back(g);
    }
}

// GLSL beauty render path. Modern GL 3.3 core: VAO + interleaved VBO + IBO +
// uniform-driven MVP, sampled via the kBeautyVert/kBeautyFrag shader pair.
// Caller must have glUseProgram'd `program` before invoking. `prevProjViewMatrix`
// is the camera (proj * view) at the previous frame — used for the Velocity
// AOV; pass current projViewMatrix when velocity isn't needed.
// `writeNormal/UV/Pref/Velocity` request AOV outputs at MRT attachments 1-4
// (Phase 3D only — caller must have bound an MRT FBO with those slots).
static void
renderGeoObjectGlsl(const GeoData& geo, GLuint program,
                    const float projViewMatrix[16],
                    const float prevProjViewMatrix[16],
                    int viewportW, int viewportH,
                    int shadingMode, bool hasLight,
                    const float lightPos[3], const float lightColor[3],
                    float lightIntensity,
                    const float cameraPos[3],
                    bool writeNormal, bool writeUV, bool writePref, bool writeVelocity)
{
    const int numVerts = (int)(geo.verts.size() / 3);
    const int numTris  = (int)(geo.triIndices.size() / 3);
    if (numVerts == 0 || numTris == 0) return;

    // --- Upload texture if available (mirrors fixed-function path) ---
    GLuint srcTex = 0;
    int hasTextureMode = 0;  // 0=no, 1=UV, 2=STW
    if (geo.texImg) {
        RectI texBounds = geo.texImg->getBounds();
        const int texW = texBounds.width();
        const int texH = texBounds.height();
        if (texW > 0 && texH > 0) {
            glGenTextures(1, &srcTex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, srcTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            std::vector<float> texData((size_t)texW * texH * 4, 0.0f);
            {
                Image::ReadAccess ra(geo.texImg.get());
                for (int y = texBounds.y1; y < texBounds.y2; ++y) {
                    for (int x = texBounds.x1; x < texBounds.x2; ++x) {
                        const float* pix = (const float*)ra.pixelAt(x, y);
                        if (!pix) continue;
                        int idx = ((y - texBounds.y1) * texW + (x - texBounds.x1)) * 4;
                        texData[idx + 0] = pix[0];
                        texData[idx + 1] = pix[1];
                        texData[idx + 2] = pix[2];
                        texData[idx + 3] = (geo.texImg->getComponents().getNumComponents() >= 4) ? pix[3] : 1.0f;
                    }
                }
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, texW, texH, 0, GL_RGBA, GL_FLOAT, texData.data());
            const bool useSTW = !geo.stw.empty() && (int)geo.stw.size() >= numVerts * 3;
            hasTextureMode = useSTW ? 2 : 1;
        }
    }

    // Project3D / MergeMat: upload each projection layer's plate to its own texture unit
    // (layer i -> unit 1+i). The fragment shader composites them when u_numProj > 0.
    GLuint projTex[4] = { 0, 0, 0, 0 };
    const int numProjLayers = std::min((int)geo.projLayers.size(), 4);
    for (int li = 0; li < numProjLayers; ++li) {
        const ImagePtr& plate = geo.projLayers[li].plateImg;
        if (!plate) continue;
        RectI pb = plate->getBounds();
        const int pw = pb.width(), ph = pb.height();
        if (pw <= 0 || ph <= 0) continue;
        glGenTextures(1, &projTex[li]);
        glActiveTexture(GL_TEXTURE1 + li);
        glBindTexture(GL_TEXTURE_2D, projTex[li]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        std::vector<float> pData((size_t)pw * ph * 4, 0.0f);
        {
            Image::ReadAccess ra(plate.get());
            const int nc = plate->getComponents().getNumComponents();
            for (int y = pb.y1; y < pb.y2; ++y) {
                for (int x = pb.x1; x < pb.x2; ++x) {
                    const float* pix = (const float*)ra.pixelAt(x, y);
                    if (!pix) continue;
                    int idx = ((y - pb.y1) * pw + (x - pb.x1)) * 4;
                    pData[idx + 0] = pix[0];
                    pData[idx + 1] = pix[1];
                    pData[idx + 2] = pix[2];
                    pData[idx + 3] = (nc >= 4) ? pix[3] : 1.0f;
                }
            }
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, pw, ph, 0, GL_RGBA, GL_FLOAT, pData.data());
    }
    glActiveTexture(GL_TEXTURE0);  // restore the default active unit
    const int useProjector = (geo.useProjector && numProjLayers > 0 && projTex[0]) ? 1 : 0;

    // --- Per-mesh MVP = projView * localMatrix (current + previous) ---
    float mvp[16];
    mat4Mul(mvp, projViewMatrix, geo.localMatrix);
    float prevMvp[16];
    mat4Mul(prevMvp, prevProjViewMatrix, geo.prevLocalMatrix);

    // --- Normal matrix = transpose(inverse(localMatrix3x3)). For non-uniform
    // scale, normals need inverse-transpose to stay perpendicular to the
    // surface in world space. Identity fallback when localMatrix is singular.
    float normalMat[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };
    {
        float m3[16] = {
            geo.localMatrix[0], geo.localMatrix[1], geo.localMatrix[2],  0,
            geo.localMatrix[4], geo.localMatrix[5], geo.localMatrix[6],  0,
            geo.localMatrix[8], geo.localMatrix[9], geo.localMatrix[10], 0,
            0, 0, 0, 1
        };
        float inv[16];
        if (mat4Invert(inv, m3)) {
            normalMat[0] = inv[0];  normalMat[1] = inv[4];  normalMat[2] = inv[8];
            normalMat[3] = inv[1];  normalMat[4] = inv[5];  normalMat[5] = inv[9];
            normalMat[6] = inv[2];  normalMat[7] = inv[6];  normalMat[8] = inv[10];
        }
    }

    // --- Interleaved VBO: pos(3) + uv(2) + stw(4) + normal(3) + prevPos(3) = 15 floats/vertex
    const int stride = 3 + 2 + 4 + 3 + 3;
    std::vector<float> interleaved((size_t)numVerts * stride, 0.0f);
    const bool hasPrevVerts = ((int)geo.prevVerts.size() >= numVerts * 3);
    for (int v = 0; v < numVerts; ++v) {
        float* row = &interleaved[(size_t)v * stride];
        row[0] = geo.verts[v * 3 + 0];
        row[1] = geo.verts[v * 3 + 1];
        row[2] = geo.verts[v * 3 + 2];
        if ((int)geo.uvs.size() > v * 2 + 1) {
            row[3] = geo.uvs[v * 2 + 0];
            row[4] = geo.uvs[v * 2 + 1];
        }
        if ((int)geo.stw.size() > v * 3 + 2) {
            row[5] = geo.stw[v * 3 + 0];
            row[6] = geo.stw[v * 3 + 1];
            row[7] = 0.0f;
            row[8] = geo.stw[v * 3 + 2];
        }
        if ((int)geo.normals.size() > v * 3 + 2) {
            row[9]  = geo.normals[v * 3 + 0];
            row[10] = geo.normals[v * 3 + 1];
            row[11] = geo.normals[v * 3 + 2];
        }
        if (hasPrevVerts) {
            row[12] = geo.prevVerts[v * 3 + 0];
            row[13] = geo.prevVerts[v * 3 + 1];
            row[14] = geo.prevVerts[v * 3 + 2];
        } else {
            row[12] = row[0]; row[13] = row[1]; row[14] = row[2];
        }
    }

    GLuint vao = 0, vbo = 0, ibo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(interleaved.size() * sizeof(float)),
                 interleaved.data(), GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(9 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(12 * sizeof(float)));

    // Wireframe mode draws GL_LINES from edges derived from triangle indices
    // (3 edges per triangle, including duplicates at shared edges — visually
    // identical to a clean edge list and avoids needing edgeIndices on GeoData
    // for the procedural primitives).
    std::vector<int> wireIndices;
    if (shadingMode == 2) {
        wireIndices.reserve(geo.triIndices.size() * 2);
        for (size_t t = 0; t + 2 < geo.triIndices.size(); t += 3) {
            int a = geo.triIndices[t + 0];
            int b = geo.triIndices[t + 1];
            int c = geo.triIndices[t + 2];
            wireIndices.push_back(a); wireIndices.push_back(b);
            wireIndices.push_back(b); wireIndices.push_back(c);
            wireIndices.push_back(c); wireIndices.push_back(a);
        }
    }

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    if (shadingMode == 2) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(wireIndices.size() * sizeof(int)),
                     wireIndices.data(), GL_STREAM_DRAW);
    } else {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(geo.triIndices.size() * sizeof(int)),
                     geo.triIndices.data(), GL_STREAM_DRAW);
    }

    // --- Set uniforms + draw ---
    glUseProgram(program);
    GLint locMvp           = glGetUniformLocation(program, "u_mvp");
    GLint locPrevMvp       = glGetUniformLocation(program, "u_prevMvp");
    GLint locLocalMatrix   = glGetUniformLocation(program, "u_localMatrix");
    GLint locNormalMat     = glGetUniformLocation(program, "u_normalMatrix");
    GLint locViewportSize  = glGetUniformLocation(program, "u_viewportSize");
    GLint locTex           = glGetUniformLocation(program, "u_tex");
    GLint locHasTexture    = glGetUniformLocation(program, "u_hasTexture");
    GLint locShadingMode   = glGetUniformLocation(program, "u_shadingMode");
    GLint locHasLight      = glGetUniformLocation(program, "u_hasLight");
    GLint locLightPos      = glGetUniformLocation(program, "u_lightPos");
    GLint locLightColor    = glGetUniformLocation(program, "u_lightColor");
    GLint locLightIntensity= glGetUniformLocation(program, "u_lightIntensity");
    GLint locCameraPos     = glGetUniformLocation(program, "u_cameraPos");
    GLint locWriteNormal   = glGetUniformLocation(program, "u_writeNormal");
    GLint locWriteUV       = glGetUniformLocation(program, "u_writeUV");
    GLint locWritePref     = glGetUniformLocation(program, "u_writePref");
    GLint locWriteVelocity = glGetUniformLocation(program, "u_writeVelocity");
    GLint locUseProjector  = glGetUniformLocation(program, "u_useProjector"); // vertex (layer 0)
    GLint locProjectorVP   = glGetUniformLocation(program, "u_projectorVP");  // vertex (layer 0)
    GLint locNumProj       = glGetUniformLocation(program, "u_numProj");
    GLint locProjPlate0    = glGetUniformLocation(program, "u_projPlate0");
    GLint locProjPlate1    = glGetUniformLocation(program, "u_projPlate1");
    GLint locProjPlate2    = glGetUniformLocation(program, "u_projPlate2");
    GLint locProjPlate3    = glGetUniformLocation(program, "u_projPlate3");
    GLint locProjVP        = glGetUniformLocation(program, "u_projVP");
    GLint locProjOn        = glGetUniformLocation(program, "u_projOn");
    GLint locProjCrop      = glGetUniformLocation(program, "u_projCrop");
    GLint locProjNear      = glGetUniformLocation(program, "u_projNear");
    GLint locProjFar       = glGetUniformLocation(program, "u_projFar");
    GLint locProjForward   = glGetUniformLocation(program, "u_projForward");
    GLint locProjOp        = glGetUniformLocation(program, "u_projOp");
    GLint locProjMix       = glGetUniformLocation(program, "u_projMix");
    GLint locOccMode       = glGetUniformLocation(program, "u_occMode");
    GLint locOccDepth      = glGetUniformLocation(program, "u_occDepth");
    if (locMvp >= 0)            glUniformMatrix4fv(locMvp,         1, GL_FALSE, mvp);
    if (locPrevMvp >= 0)        glUniformMatrix4fv(locPrevMvp,     1, GL_FALSE, prevMvp);
    if (locLocalMatrix >= 0)    glUniformMatrix4fv(locLocalMatrix, 1, GL_FALSE, geo.localMatrix);
    if (locNormalMat >= 0)      glUniformMatrix3fv(locNormalMat,   1, GL_FALSE, normalMat);
    if (locViewportSize >= 0)   glUniform2f(locViewportSize, (float)viewportW, (float)viewportH);
    if (locTex >= 0)            glUniform1i(locTex, 0);
    if (locHasTexture >= 0)     glUniform1i(locHasTexture, hasTextureMode);
    if (locShadingMode >= 0)    glUniform1i(locShadingMode, shadingMode);
    if (locHasLight >= 0)       glUniform1i(locHasLight, hasLight ? 1 : 0);
    if (locLightPos >= 0)       glUniform3f(locLightPos,
                                            hasLight ? lightPos[0] : 0.0f,
                                            hasLight ? lightPos[1] : 0.0f,
                                            hasLight ? lightPos[2] : 0.0f);
    if (locLightColor >= 0)     glUniform3f(locLightColor,
                                            hasLight ? lightColor[0] : 1.0f,
                                            hasLight ? lightColor[1] : 1.0f,
                                            hasLight ? lightColor[2] : 1.0f);
    if (locLightIntensity >= 0) glUniform1f(locLightIntensity, hasLight ? lightIntensity : 1.0f);
    if (locCameraPos >= 0)      glUniform3f(locCameraPos, cameraPos[0], cameraPos[1], cameraPos[2]);
    if (locWriteNormal >= 0)    glUniform1i(locWriteNormal,   writeNormal   ? 1 : 0);
    if (locWriteUV >= 0)        glUniform1i(locWriteUV,       writeUV       ? 1 : 0);
    if (locWritePref >= 0)      glUniform1i(locWritePref,     writePref     ? 1 : 0);
    if (locWriteVelocity >= 0)  glUniform1i(locWriteVelocity, writeVelocity ? 1 : 0);
    if (locUseProjector >= 0)   glUniform1i(locUseProjector, useProjector);              // vertex (layer 0)
    if (locProjectorVP >= 0)    glUniformMatrix4fv(locProjectorVP, 1, GL_FALSE, geo.projectorVP);
    if (locNumProj >= 0)        glUniform1i(locNumProj, useProjector ? numProjLayers : 0);
    if (locProjPlate0 >= 0)     glUniform1i(locProjPlate0, 1);   // plates on texture units 1..4
    if (locProjPlate1 >= 0)     glUniform1i(locProjPlate1, 2);
    if (locProjPlate2 >= 0)     glUniform1i(locProjPlate2, 3);
    if (locProjPlate3 >= 0)     glUniform1i(locProjPlate3, 4);
    {
        // Pack the per-layer projection params into fixed-size arrays (unused slots padded
        // with a default ProjLayer — harmless since u_numProj gates the loop).
        float vp[16 * 4]; int on[4]; int crop[4]; float pn[4]; float pf[4]; float fwd[3 * 4]; int op[4]; float mix[4];
        for (int li = 0; li < 4; ++li) {
            ProjLayer L;
            if (li < numProjLayers) L = geo.projLayers[li];
            for (int k = 0; k < 16; ++k) vp[li * 16 + k] = L.VP[k];
            on[li] = L.projOn; crop[li] = L.crop; pn[li] = L.pNear; pf[li] = L.pFar;
            fwd[li * 3 + 0] = L.forward[0]; fwd[li * 3 + 1] = L.forward[1]; fwd[li * 3 + 2] = L.forward[2];
            op[li] = L.op; mix[li] = L.mix;
        }
        if (locProjVP >= 0)      glUniformMatrix4fv(locProjVP, 4, GL_FALSE, vp);
        if (locProjOn >= 0)      glUniform1iv(locProjOn, 4, on);
        if (locProjCrop >= 0)    glUniform1iv(locProjCrop, 4, crop);
        if (locProjNear >= 0)    glUniform1fv(locProjNear, 4, pn);
        if (locProjFar >= 0)     glUniform1fv(locProjFar, 4, pf);
        if (locProjForward >= 0) glUniform3fv(locProjForward, 4, fwd);
        if (locProjOp >= 0)      glUniform1iv(locProjOp, 4, op);
        if (locProjMix >= 0)     glUniform1fv(locProjMix, 4, mix);
    }
    // Occlusion (layer 0 only): active once the projector depth map exists (built in render()).
    const int occUniform = (geo.occMode != 0 && geo.occDepthTex != 0) ? geo.occMode : 0;
    if (geo.occDepthTex) {
        glActiveTexture(GL_TEXTURE5);   // units 1..4 are the projection plates now
        glBindTexture(GL_TEXTURE_2D, geo.occDepthTex);
        glActiveTexture(GL_TEXTURE0);
    }
    if (locOccMode >= 0)        glUniform1i(locOccMode, occUniform);
    if (locOccDepth >= 0)       glUniform1i(locOccDepth, 5);

    if (shadingMode == 2) {
        glDrawElements(GL_LINES, (GLsizei)wireIndices.size(), GL_UNSIGNED_INT, 0);
    } else {
        glDrawElements(GL_TRIANGLES, numTris * 3, GL_UNSIGNED_INT, 0);
    }

    // --- Cleanup ---
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ibo);
    glDeleteVertexArrays(1, &vao);
    if (srcTex) glDeleteTextures(1, &srcTex);
    for (int li = 0; li < 4; ++li) if (projTex[li]) glDeleteTextures(1, &projTex[li]);
}

// ==================== Render ====================

StatusEnum
ScanlineRender::render(const RenderActionArgs& args)
{
    assert(!args.outputPlanes.empty());
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) return eStatusFailed;

    int baseW = _imp->outputWidth.lock()->getValue();
    int baseH = _imp->outputHeight.lock()->getValue();
    int overscan = _imp->overscan.lock() ? _imp->overscan.lock()->getValue() : 0;

    // A connected bg (input 0) overrides the resolution (Nuke-style): render at the bg's FORMAT
    // and skip overscan so the composite aligns 1:1. Important for UV-bake projection (match
    // the UV-space, e.g. a square plate). Use the format (matches getRegionOfDefinition) rather
    // than the bg image bounds: a Reformat with "black outside" off keeps the original data
    // window as its bounds even when its format is square. Fetched up-front here + reused below.
    ImagePtr bgImg;
    {
        EffectInstancePtr bgEffect = getInput(0);
        if (bgEffect) {
            // Size the framebuffer from the SAME conform helper as the format/RoD, so all three
            // agree (the bug we chased: RoD conformed to 2K while the framebuffer stayed HD).
            RectI bgRect;
            if (getBgConformRect(args.time, args.view, &bgRect)) {
                baseW = bgRect.width();
                baseH = bgRect.height();
                overscan = 0;
            }
            RectI bgRoi;
            bgImg = getImage(0, args.time, RenderScale(), args.view,
                             NULL, NULL, false, true, eStorageModeRAM, 0, &bgRoi);
        }
    }

    // Padded render dimensions: the frame plus 'overscan' pixels on every side.
    // All framebuffers, the viewport, readback, and the output write loop use
    // outW/outH, and the output RoD (getRegionOfDefinition) starts at (-overscan,
    // -overscan), so the write loop's fbX = x - outBounds.x1 maps 1:1 with no
    // further offsetting needed.
    int outW = baseW + 2 * overscan;
    int outH = baseH + 2 * overscan;
    // Aperture scale to widen the frustum for overscan. Scaling each aperture by
    // paddedPixels/basePixels keeps every original pixel on the exact same world
    // ray and extends the view into the overscan border (symmetric padding keeps
    // the frustum centered). 1.0 when overscan == 0.
    const double ovScaleX = (baseW > 0) ? (double)outW / (double)baseW : 1.0;
    const double ovScaleY = (baseH > 0) ? (double)outH / (double)baseH : 1.0;

    // Projection mode (0 = Perspective, 1 = Orthographic) + orthographic width.
    const int projMode = _imp->projectionMode.lock() ? _imp->projectionMode.lock()->getValue() : 0;
    const double orthoWidth = _imp->orthoWidth.lock() ? _imp->orthoWidth.lock()->getValue() : 10.0;

    // Global ambient fill colour (Shaded mode). Default 0.15 grey matches the
    // previously hard-coded ambient term.
    float ambient[3] = { 0.15f, 0.15f, 0.15f };
    if (KnobColorPtr ac = _imp->ambient.lock()) {
        ambient[0] = (float)ac->getValueAtTime(args.time, 0);
        ambient[1] = (float)ac->getValueAtTime(args.time, 1);
        ambient[2] = (float)ac->getValueAtTime(args.time, 2);
    }

    // Transparency: 1 = respect surface alpha, 0 = force opaque.
    const int transparency = (_imp->transparency.lock() && !_imp->transparency.lock()->getValue()) ? 0 : 1;

    // --- Get camera from input 2 (through any Dots) ---
    EffectInstancePtr camEffect = skipDots(getInput(2));
    CameraProvider* cam = camEffect ? dynamic_cast<CameraProvider*>(camEffect.get()) : NULL;

    double camTX = 0, camTY = 0, camTZ = 5, camRX = 0, camRY = 0, camRZ = 0;
    double camFL = 50.0, camHA = 24.576, camVA = 18.672;
    float camNear = 0.1f, camFar = 10000.0f;

    if (cam) {
        cam->getCameraPosition(args.time, camTX, camTY, camTZ, camRX, camRY, camRZ);
        camFL = cam->getCameraFocalLength(args.time);
        camHA = cam->getCameraHAperture(args.time);
        camVA = cam->getCameraVAperture(args.time);
        camNear = (float)cam->getCameraNear(args.time);
        camFar = (float)cam->getCameraFar(args.time);
    }

    // --- Collect geometry objects and/or particles ---
    std::vector<GeoData> geoObjects;
    ParticleDataPtr particleData;

    EffectInstancePtr geoEffect = getInput(1);
    if (!geoEffect) return eStatusFailed;
    // Honor the "D" disable knob: a disabled scene/geo input renders nothing.
    if (geoEffect->getNode() && geoEffect->getNode()->isNodeDisabled()) {
        return eStatusFailed;
    }

    // Check for volume nodes
    Volume3D* volume3d = dynamic_cast<Volume3D*>(geoEffect.get());
    ReadVDB* readVdb = dynamic_cast<ReadVDB*>(geoEffect.get());

    // Check for particle instance node (must check BEFORE ParticleProvider
    // since ParticleInstance inherits from it)
    ParticleInstance* particleInstancer = dynamic_cast<ParticleInstance*>(geoEffect.get());

    // Underlying particle data for motion blur offsetting (works for both sprites and instances)
    ParticleDataPtr motionBlurPData;
    // Provider kept for the Trail particle mode (queries past frames' data).
    ParticleProvider* trailProvider = nullptr;

    // Check for particle nodes (only if NOT an instancer — instancer renders geo, not sprites)
    if (!particleInstancer) {
        ParticleProvider* pProvider = dynamic_cast<ParticleProvider*>(geoEffect.get());
        if (pProvider) {
            particleData = pProvider->getParticleData(args.time);
            motionBlurPData = particleData;
            trailProvider = pProvider;
        }
    } else {
        // Instancer: get the upstream particle data for motion blur offsetting
        motionBlurPData = particleInstancer->getParticleData(args.time);
    }

    // Light detection (declared early so Scene iteration can find lights)
    Light3D* light3d = dynamic_cast<Light3D*>(geoEffect.get());

    // Check if input is a Scene3D (multi-object aggregator)
    if (!particleData) {
        Scene3D* scene = dynamic_cast<Scene3D*>(geoEffect.get());
        if (scene) {
            for (int i = 0; i < SCENE3D_MAX_INPUTS; ++i) {
                EffectInstancePtr sceneInput = scene->getInput(i);
                if (!sceneInput) continue;
                // Honor "D" disable: skip disabled scene inputs entirely.
                if (sceneInput->getNode() && sceneInput->getNode()->isNodeDisabled()) continue;

                // Check for volumes in scene
                Volume3D* sVol = dynamic_cast<Volume3D*>(sceneInput.get());
                if (sVol && !volume3d) { volume3d = sVol; continue; }

                ReadVDB* sVdb = dynamic_cast<ReadVDB*>(sceneInput.get());
                if (sVdb && !readVdb) { readVdb = sVdb; continue; }

                // Check for lights in scene (already handled above, but also here)
                Light3D* sLight = dynamic_cast<Light3D*>(sceneInput.get());
                if (sLight) { if (!light3d) light3d = sLight; continue; }

                // Check for particle instancer in scene
                ParticleInstance* sInstancer = dynamic_cast<ParticleInstance*>(sceneInput.get());
                if (sInstancer) {
                    if (!particleInstancer) particleInstancer = sInstancer;
                    continue;
                }

                // Check for particles in scene
                ParticleProvider* sProvider = dynamic_cast<ParticleProvider*>(sceneInput.get());
                if (sProvider) {
                    particleData = sProvider->getParticleData(args.time);
                    motionBlurPData = particleData;
                    trailProvider = sProvider;
                } else {
                    extractGeometries(sceneInput, args.time, args.view, geoObjects);
                }
            }
        } else {
            extractGeometries(geoEffect, args.time, args.view, geoObjects);
        }
    }

    if (geoObjects.empty() && !particleData && !particleInstancer && !volume3d && !readVdb) return eStatusFailed;

    // --- Acquire GL context ---
    GPUContextPool* pool = appPTR->getGPUContextPool();
    if (!pool) return eStatusFailed;

    OSGLContextPtr glContext;
    try {
        glContext = pool->attachGLContextToRender(true);
    } catch (...) {
        return eStatusFailed;
    }
    if (!glContext) return eStatusFailed;

    glContext->setContextCurrentNoRender();

    // --- Determine MSAA sample count from the Antialiasing knob (clamp to driver max) ---
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    int aaLevel = _imp->antialiasing.lock() ? _imp->antialiasing.lock()->getValue() : 2; // default Medium
    static const int kAaSamples[4] = { 1, 2, 4, 8 }; // None / Low / Medium / High
    if (aaLevel < 0 || aaLevel > 3) aaLevel = 2;
    int samples = std::min(kAaSamples[aaLevel], (int)maxSamples);
    if (samples < 1) samples = 1;

    // --- Create MSAA FBO (multisampled color + depth renderbuffers) ---
    GLuint msFBO = 0, msColorRB = 0, msDepthRB = 0;
    glGenFramebuffers(1, &msFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, msFBO);

    glGenRenderbuffers(1, &msColorRB);
    glBindRenderbuffer(GL_RENDERBUFFER, msColorRB);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA32F, outW, outH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColorRB);

    glGenRenderbuffers(1, &msDepthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, msDepthRB);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, outW, outH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msDepthRB);

    GLenum msStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (msStatus != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &msFBO);
        glDeleteRenderbuffers(1, &msColorRB);
        glDeleteRenderbuffers(1, &msDepthRB);
        OSGLContext::unsetCurrentContextNoRender();
        pool->releaseGLContextFromRender(glContext);
        return eStatusFailed;
    }

    // --- Create resolve FBO (single-sample, used for glReadPixels) ---
    GLuint fbo = 0, colorTex = 0, depthRB = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, outW, outH, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    glGenRenderbuffers(1, &depthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, outW, outH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);

    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &msFBO);
        glDeleteRenderbuffers(1, &msColorRB);
        glDeleteRenderbuffers(1, &msDepthRB);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &colorTex);
        glDeleteRenderbuffers(1, &depthRB);
        OSGLContext::unsetCurrentContextNoRender();
        pool->releaseGLContextFromRender(glContext);
        return eStatusFailed;
    }

    // --- Phase 3D/3E: optional MRT attachments (Normal=1, UV=2, Pref=3, Velocity=4) ---
    // Each AOV's attachment is allocated only when its knob is on. Attachments
    // don't have to be contiguous — glDrawBuffers can carry GL_NONE for
    // skipped slots, e.g. [COLOR0, GL_NONE, COLOR2].
    const bool wantsNormalMrtPre   = _imp->outputNormal.lock()   && _imp->outputNormal.lock()->getValue();
    const bool wantsUvMrtPre       = _imp->outputUV.lock()       && _imp->outputUV.lock()->getValue();
    const bool wantsPrefMrtPre     = _imp->outputPref.lock()     && _imp->outputPref.lock()->getValue();
    const bool wantsVelocityMrtPre = _imp->outputVelocity.lock() && _imp->outputVelocity.lock()->getValue();
    GLuint msNormalRB = 0,   normalResolveTex   = 0;
    GLuint msUvRB = 0,       uvResolveTex       = 0;
    GLuint msPrefRB = 0,     prefResolveTex     = 0;
    GLuint msVelocityRB = 0, velocityResolveTex = 0;

    auto allocMrtAttachment = [&](int attachmentIndex, GLuint& msRB, GLuint& resolveTex,
                                  const char* aovName) {
        glBindFramebuffer(GL_FRAMEBUFFER, msFBO);
        glGenRenderbuffers(1, &msRB);
        glBindRenderbuffer(GL_RENDERBUFFER, msRB);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA32F_ARB, outW, outH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attachmentIndex,
                                  GL_RENDERBUFFER, msRB);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &resolveTex);
        glBindTexture(GL_TEXTURE_2D, resolveTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, outW, outH, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attachmentIndex,
                               GL_TEXTURE_2D, resolveTex, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, msFBO);
        bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (ok) ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        if (!ok) {
            std::fprintf(stderr, "[GLSL FAIL] %s AOV: MRT FBO incomplete after adding attachment %d.\n",
                         aovName, attachmentIndex);
            std::fflush(stderr);
            glDeleteRenderbuffers(1, &msRB);    msRB = 0;
            glDeleteTextures(1, &resolveTex);   resolveTex = 0;
        }
    };

    if (wantsNormalMrtPre)   allocMrtAttachment(1, msNormalRB,   normalResolveTex,   "Normal");
    if (wantsUvMrtPre)       allocMrtAttachment(2, msUvRB,       uvResolveTex,       "UV");
    if (wantsPrefMrtPre)     allocMrtAttachment(3, msPrefRB,     prefResolveTex,     "Pref");
    if (wantsVelocityMrtPre) allocMrtAttachment(4, msVelocityRB, velocityResolveTex, "Velocity");

    // Render into the multisampled FBO
    glBindFramebuffer(GL_FRAMEBUFFER, msFBO);

    glViewport(0, 0, outW, outH);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE); // enable MSAA for multisampled FBO

    // --- Set up camera matrices ---
    // Projection comes from both apertures via CameraMath. Image aspect
    // (outW/outH) intentionally not used here — fixes CG drift under camera
    // motion when sensor aspect != image aspect.
    float viewMatrix[16], projMatrix[16];
    buildViewMatrix(camTX, camTY, camTZ, camRX, camRY, camRZ, viewMatrix);
    buildProjectionForMode(projMode, camFL, camHA, camVA, orthoWidth, ovScaleX, ovScaleY, camNear, camFar, projMatrix);

    // --- Motion blur setup ---
    int motionSamples = _imp->motionSamples.lock() ? _imp->motionSamples.lock()->getValue() : 1;
    float motionShutter = _imp->motionShutter.lock() ? (float)_imp->motionShutter.lock()->getValueAtTime(args.time) : 0.5f;
    int shutterOffsetMode = _imp->shutterOffset.lock() ? _imp->shutterOffset.lock()->getValue() : 0;
    float shutterCustomOffset = _imp->shutterCustomOffset.lock() ? (float)_imp->shutterCustomOffset.lock()->getValueAtTime(args.time) : 0.0f;
    float temporalJitter = _imp->temporalJitter.lock() ? (float)_imp->temporalJitter.lock()->getValueAtTime(args.time) : 0.0f;
    if (motionSamples < 1) motionSamples = 1;

    // Stretch cheat: only active when multi-sample is off. Shutter multiplies the stretch.
    float motionBlurKnob = _imp->particleMotionBlur.lock() ? (float)_imp->particleMotionBlur.lock()->getValueAtTime(args.time) : 0.0f;
    float motionBlur = (motionSamples == 1) ? (motionBlurKnob * motionShutter) : 0.0f;

    // Multi-sample motion blur offsets particle positions per shutter sample.
    // The provider's ParticleData is a SHARED immutable snapshot (also read by
    // the 3D viewport and used as the sim's resume state) — never mutate it.
    // Take a private copy to offset, and keep the sprite path (particleData)
    // pointing at the same object it did before.
    std::vector<std::array<float, 3>> origParticlePos;
    if (motionSamples > 1 && motionBlurPData) {
        const bool spritesUseSameData = (particleData == motionBlurPData);
        motionBlurPData = std::make_shared<ParticleData>(*motionBlurPData);
        if (spritesUseSameData) {
            particleData = motionBlurPData;
        }
        origParticlePos.reserve(motionBlurPData->particles.size());
        for (const Particle& p : motionBlurPData->particles) {
            origParticlePos.push_back({p.px, p.py, p.pz});
        }
    }

    // Accumulator for multi-sample blur
    std::vector<float> accumPixels;
    // Phase 3D — parallel accumulators for the GLSL-MRT AOVs. Averaging across
    // motion-blur samples is meaningful for normals, UVs, Pref, and velocity.
    std::vector<float> normalAccumPixels;
    std::vector<float> uvAccumPixels;
    std::vector<float> prefAccumPixels;
    std::vector<float> velocityAccumPixels;

    // Phase 3E — beauty program is the only mesh draw path. Built once outside
    // the multi-sample loop. On build failure, mesh rendering is skipped this
    // render (program==0 → the per-geo branch below short-circuits).
    GLuint glslBeautyProg = glslBuildProgram(kBeautyVert, kBeautyFrag);
    if (!glslBeautyProg) {
        std::fprintf(stderr, "[GLSL FAIL] beauty program build failed — mesh geometry will not render.\n");
        std::fflush(stderr);
    }

    // Precompute projView = projMatrix * viewMatrix for the GLSL path.
    float projViewMatrix[16];
    mat4Mul(projViewMatrix, projMatrix, viewMatrix);

    // Phase 3E — build the particle GLSL program once. Used for any partMode
    // that's been migrated to GLSL (3E.1 covers Point + streak lines; 3E.2/3
    // will add Disc/Sphere/Sprite). Failure to build silently falls back to
    // the legacy fixed-function path for unmigrated modes.
    GLuint glslParticleProg = glslBuildProgram(kParticleVert, kParticleFrag);
    if (!glslParticleProg) {
        std::fprintf(stderr, "[GLSL FAIL] particle program build failed — particles will be skipped for this render.\n");
        std::fflush(stderr);
    }

    // ParticleInstance program — VBO/VAO + uniform-driven draw. On build
    // failure instance rendering is skipped silently for this render.
    GLuint glslInstanceProg = glslBuildProgram(kInstanceVert, kInstanceFrag);
    if (!glslInstanceProg) {
        std::fprintf(stderr, "[GLSL FAIL] instance program build failed — particle instances will be skipped for this render.\n");
        std::fflush(stderr);
    }

    // Phase 3D/3E — latch the final wants*Mrt flags now that we know whether
    // the beauty program built and the MRT attachment was allocated. Any
    // failure latches `false` and we just skip that AOV silently.
    const bool wantsNormalMrt   = wantsNormalMrtPre   && (glslBeautyProg != 0) && (msNormalRB   != 0);
    const bool wantsUvMrt       = wantsUvMrtPre       && (glslBeautyProg != 0) && (msUvRB       != 0);
    const bool wantsPrefMrt     = wantsPrefMrtPre     && (glslBeautyProg != 0) && (msPrefRB     != 0);
    const bool wantsVelocityMrt = wantsVelocityMrtPre && (glslBeautyProg != 0) && (msVelocityRB != 0);
    const bool wantsAnyMrt      = wantsNormalMrt || wantsUvMrt || wantsPrefMrt || wantsVelocityMrt;

    // Phase 3D — previous-frame camera matrices for the Velocity AOV. Use
    // time - 1.0 as the reference. Output is pixels per frame.
    float prevViewMatrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float prevProjMatrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float prevProjViewMatrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (wantsVelocityMrt) {
        const double prevTime = args.time - 1.0;
        double pTx = (double)camTX, pTy = (double)camTY, pTz = (double)camTZ;
        double pRx = (double)camRX, pRy = (double)camRY, pRz = (double)camRZ;
        double pFL = (double)camFL, pHA = (double)camHA, pVA = (double)camVA;
        double pNear = (double)camNear, pFar = (double)camFar;
        if (cam) {
            cam->getCameraPosition(prevTime, pTx, pTy, pTz, pRx, pRy, pRz);
            pFL   = cam->getCameraFocalLength(prevTime);
            pHA   = cam->getCameraHAperture(prevTime);
            pVA   = cam->getCameraVAperture(prevTime);
            pNear = cam->getCameraNear(prevTime);
            pFar  = cam->getCameraFar(prevTime);
        }
        buildViewMatrix((float)pTx, (float)pTy, (float)pTz,
                        (float)pRx, (float)pRy, (float)pRz, prevViewMatrix);
        buildProjectionForMode(projMode, pFL, pHA, pVA, orthoWidth, ovScaleX, ovScaleY,
                               (float)pNear, (float)pFar, prevProjMatrix);
        mat4Mul(prevProjViewMatrix, prevProjMatrix, prevViewMatrix);
    }

    // Phase 3D — extract previous-frame geometry. For animated meshes the
    // verts differ across frames; for static meshes only the local matrix
    // moves. renderGeoObjectGlsl auto-detects via `hasPrevVerts`.
    // Pull shading mode + light parameters once per render (shared by every
    // motion-blur sample and every geo object inside the loop).
    int shadingMode = _imp->shadingMode.lock() ? _imp->shadingMode.lock()->getValue() : 0;
    bool hasLight = false;
    float lightPos[3] = { 0, 0, 0 };
    float lightColor[3] = { 1, 1, 1 };
    float lightIntensity = 1.0f;
    const float cameraPos[3] = { (float)camTX, (float)camTY, (float)camTZ };
    if (light3d) {
        double ltx, lty, ltz, lr, lg, lb, lint, lexp_unused;
        light3d->getLightParams(args.time, ltx, lty, ltz, lr, lg, lb, lint, lexp_unused);
        lightPos[0] = (float)ltx; lightPos[1] = (float)lty; lightPos[2] = (float)ltz;
        lightColor[0] = (float)lr; lightColor[1] = (float)lg; lightColor[2] = (float)lb;
        lightIntensity = (float)lint;
        hasLight = true;
    }

    if (wantsVelocityMrt && geoEffect) {
        std::vector<GeoData> prevGeoObjects;
        const double prevTime = args.time - 1.0;
        Scene3D* scenePrev = dynamic_cast<Scene3D*>(geoEffect.get());
        if (scenePrev) {
            for (int i = 0; i < SCENE3D_MAX_INPUTS; ++i) {
                EffectInstancePtr sceneInputPrev = scenePrev->getInput(i);
                if (!sceneInputPrev) continue;
                if (dynamic_cast<Volume3D*>(sceneInputPrev.get())) continue;
                if (dynamic_cast<ReadVDB*>(sceneInputPrev.get())) continue;
                if (dynamic_cast<Light3D*>(sceneInputPrev.get())) continue;
                if (dynamic_cast<ParticleInstance*>(sceneInputPrev.get())) continue;
                if (dynamic_cast<ParticleProvider*>(sceneInputPrev.get())) continue;
                extractGeometries(sceneInputPrev, prevTime, args.view, prevGeoObjects);
            }
        } else {
            extractGeometries(geoEffect, prevTime, args.view, prevGeoObjects);
        }
        const size_t n = std::min(geoObjects.size(), prevGeoObjects.size());
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(geoObjects[i].prevLocalMatrix, prevGeoObjects[i].localMatrix, sizeof(float) * 16);
            if (prevGeoObjects[i].verts.size() == geoObjects[i].verts.size() &&
                prevGeoObjects[i].verts != geoObjects[i].verts) {
                geoObjects[i].prevVerts = std::move(prevGeoObjects[i].verts);
            }
        }
    }

    // Per-sample camera evaluation mutates viewMatrix / projMatrix /
    // projViewMatrix inside the multi-sample loop. Post-loop code (the
    // WorldPos AOV reconstruction at the end of render()) expects the
    // frame-time matrices, so save them here and restore after the loop.
    float frameViewMatrix[16], frameProjMatrix[16], frameProjViewMatrix[16];
    std::memcpy(frameViewMatrix,     viewMatrix,     16 * sizeof(float));
    std::memcpy(frameProjMatrix,     projMatrix,     16 * sizeof(float));
    std::memcpy(frameProjViewMatrix, projViewMatrix, 16 * sizeof(float));

    // === MULTI-SAMPLE RENDER LOOP ===
    for (int sample = 0; sample < motionSamples; ++sample) {
        // Compute sub-frame time offset per the chosen shutter offset mode.
        //   Centered: shutter window is [-shutter/2, +shutter/2] (real-camera)
        //   Start:    shutter window is [0,         +shutter]    (motion after frame)
        //   End:      shutter window is [-shutter,  0]           (motion before frame)
        //   Custom:   shutter window is [customOffset, customOffset+shutter]
        float sampleDt = 0.0f;
        if (motionSamples > 1) {
            const float t01 = (float)sample / (float)(motionSamples - 1);  // 0..1 across samples
            switch (shutterOffsetMode) {
            case 0: // Centered
                sampleDt = (t01 - 0.5f) * motionShutter;
                break;
            case 1: // Start — shutter opens at current frame
                sampleDt = t01 * motionShutter;
                break;
            case 2: // End — shutter closes at current frame
                sampleDt = (t01 - 1.0f) * motionShutter;
                break;
            case 3: // Custom
                sampleDt = t01 * motionShutter + shutterCustomOffset;
                break;
            default:
                sampleDt = (t01 - 0.5f) * motionShutter;
                break;
            }

            // Temporal jitter — perturb each sample's time within its slot.
            // Deterministic hash on (sample, integer frame) so the same shot
            // renders the same way across launches; magnitude scaled to one
            // slot width so high jitter values don't collide samples.
            if (temporalJitter > 0.0f) {
                uint32_t h = (uint32_t)sample * 2654435761u
                           ^ (uint32_t)(int)args.time * 1597334677u;
                h ^= h >> 16;
                h *= 0x7feb352du;
                h ^= h >> 15;
                const float r = ((h & 0xFFFFu) / 65535.0f) - 0.5f;        // [-0.5, 0.5]
                const float slot = motionShutter / (float)motionSamples;  // width of one sample's slot
                sampleDt += temporalJitter * r * slot;
            }
        }

        // Apply offset to particle positions (affects both sprites and instances)
        if (motionSamples > 1 && motionBlurPData) {
            for (size_t i = 0; i < motionBlurPData->particles.size(); ++i) {
                Particle& p = motionBlurPData->particles[i];
                p.px = origParticlePos[i][0] + p.vx * sampleDt;
                p.py = origParticlePos[i][1] + p.vy * sampleDt;
                p.pz = origParticlePos[i][2] + p.vz * sampleDt;
            }
        }

        // Re-evaluate the camera at sub-frame time. viewMatrix / projMatrix /
        // projViewMatrix are mutated here; the frame-time copies (saved before
        // the loop) are restored after. Rebuilding projViewMatrix is critical
        // — every GLSL draw path reads it as the `u_projView` uniform, and
        // renderGeoObjectGlsl takes it as an argument.
        const double sampleTime = args.time + (double)sampleDt;
        if (cam && motionSamples > 1) {
            double sTx = camTX, sTy = camTY, sTz = camTZ;
            double sRx = camRX, sRy = camRY, sRz = camRZ;
            cam->getCameraPosition(sampleTime, sTx, sTy, sTz, sRx, sRy, sRz);
            const double sFL   = cam->getCameraFocalLength(sampleTime);
            const double sHA   = cam->getCameraHAperture(sampleTime);
            const double sVA   = cam->getCameraVAperture(sampleTime);
            const double sNear = cam->getCameraNear(sampleTime);
            const double sFar  = cam->getCameraFar(sampleTime);
            buildViewMatrix((float)sTx, (float)sTy, (float)sTz,
                            (float)sRx, (float)sRy, (float)sRz, viewMatrix);
            buildProjectionForMode(projMode, sFL, sHA, sVA, orthoWidth, ovScaleX, ovScaleY,
                                   (float)sNear, (float)sFar, projMatrix);
            mat4Mul(projViewMatrix, projMatrix, viewMatrix);
        }

        // Re-extract geometry at sub-frame time so animated transforms
        // (rotating Sphere3D, translating Card3D, animated Alembic xforms)
        // actually motion-blur. Mirrors the outer extraction logic but at
        // sampleTime and skipping non-geo types (particles handle their own
        // sub-frame extrapolation, lights are static).
        // Performance: this re-extracts every geo every sample. Procedural
        // geo (Sphere3D etc.) is cheap (just transform recomputation);
        // animated Alembic re-reads its pre-loaded sample table (cheap).
        if (motionSamples > 1 && !particleData) {
            geoObjects.clear();
            Scene3D* sceneMb = dynamic_cast<Scene3D*>(geoEffect.get());
            if (sceneMb) {
                for (int i = 0; i < SCENE3D_MAX_INPUTS; ++i) {
                    EffectInstancePtr sceneInput = sceneMb->getInput(i);
                    if (!sceneInput) continue;
                    if (sceneInput->getNode() && sceneInput->getNode()->isNodeDisabled()) continue;
                    if (dynamic_cast<Volume3D*>(sceneInput.get())) continue;
                    if (dynamic_cast<ReadVDB*>(sceneInput.get())) continue;
                    if (dynamic_cast<Light3D*>(sceneInput.get())) continue;
                    if (dynamic_cast<ParticleInstance*>(sceneInput.get())) continue;
                    if (dynamic_cast<ParticleProvider*>(sceneInput.get())) continue;
                    extractGeometries(sceneInput, sampleTime, args.view, geoObjects);
                }
            } else {
                extractGeometries(geoEffect, sampleTime, args.view, geoObjects);
            }
        }

        // Clear the MSAA FBO for this sample
        glBindFramebuffer(GL_FRAMEBUFFER, msFBO);
        // Phase 3D — glDrawBuffers maps fragment-shader layout(location=N) to
        // physical color attachments. GL_NONE lets us skip un-allocated slots
        // so e.g. "UV only, no Normal" still works without re-compiling the
        // shader.
        if (wantsAnyMrt) {
            GLenum drawBufs[5] = {
                (GLenum)GL_COLOR_ATTACHMENT0,
                (GLenum)(wantsNormalMrt   ? GL_COLOR_ATTACHMENT1 : GL_NONE),
                (GLenum)(wantsUvMrt       ? GL_COLOR_ATTACHMENT2 : GL_NONE),
                (GLenum)(wantsPrefMrt     ? GL_COLOR_ATTACHMENT3 : GL_NONE),
                (GLenum)(wantsVelocityMrt ? GL_COLOR_ATTACHMENT4 : GL_NONE),
            };
            glDrawBuffers(5, drawBufs);
        } else {
            GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, drawBufs);
        }
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // --- Render all geometry objects ---
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (glslBeautyProg) {
        // Projection mode (UV bake / spherical) applies to every geo via this
        // program; set it once (uniforms persist across renderGeoObjectGlsl's
        // repeated glUseProgram). u_view/u_near/u_far feed the spherical path.
        glUseProgram(glslBeautyProg);
        const GLint locBeautyMode = glGetUniformLocation(glslBeautyProg, "u_projMode");
        if (locBeautyMode >= 0) glUniform1i(locBeautyMode, projMode);
        const GLint locBeautyView = glGetUniformLocation(glslBeautyProg, "u_view");
        if (locBeautyView >= 0) glUniformMatrix4fv(locBeautyView, 1, GL_FALSE, viewMatrix);
        const GLint locBeautyNear = glGetUniformLocation(glslBeautyProg, "u_near");
        if (locBeautyNear >= 0) glUniform1f(locBeautyNear, camNear);
        const GLint locBeautyFar = glGetUniformLocation(glslBeautyProg, "u_far");
        if (locBeautyFar >= 0) glUniform1f(locBeautyFar, camFar);
        const GLint locBeautyAmbient = glGetUniformLocation(glslBeautyProg, "u_ambient");
        if (locBeautyAmbient >= 0) glUniform3fv(locBeautyAmbient, 1, ambient);
        const GLint locBeautyTransp = glGetUniformLocation(glslBeautyProg, "u_transparency");
        if (locBeautyTransp >= 0) glUniform1i(locBeautyTransp, transparency);
        for (size_t gi = 0; gi < geoObjects.size(); ++gi) {
            renderGeoObjectGlsl(geoObjects[gi], glslBeautyProg,
                                projViewMatrix, prevProjViewMatrix,
                                outW, outH,
                                shadingMode, hasLight, lightPos, lightColor, lightIntensity,
                                cameraPos,
                                wantsNormalMrt, wantsUvMrt,
                                wantsPrefMrt, wantsVelocityMrt);
        }
        glUseProgram(0);
        // Restore single-attachment draw buffer so particles + volumes only
        // paint into beauty (they don't emit AOV outputs).
        if (wantsAnyMrt) {
            GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, drawBufs);
        }
    }

    // --- Render particles ---
    if (particleData && particleData->numParticles() > 0) {
        int partMode = _imp->particleMode.lock() ? _imp->particleMode.lock()->getValue() : 3;
        int blendMode = _imp->particleBlend.lock() ? _imp->particleBlend.lock()->getValue() : 0;
        // Additive particles get per-particle emission boost (ParticleAttribute
        // Emission section). Over-blend ignores it: emission is a glow concept.
        const bool emissiveBoost = (blendMode == 0);
        float globalScale = _imp->particleScale.lock() ? (float)_imp->particleScale.lock()->getValueAtTime(args.time) : 1.0f;
        bool solidParticles = _imp->particleSolid.lock() ? _imp->particleSolid.lock()->getValue() : false;

        // Camera right/up/forward vectors directly from the view matrix
        // (column-major float[16]: out[col*4 + row]). Used for billboarding
        // sprites/discs and projecting velocity onto the camera plane for
        // streak lines.
        float rightX = viewMatrix[0], rightY = viewMatrix[4], rightZ = viewMatrix[8];
        float upX    = viewMatrix[1], upY    = viewMatrix[5], upZ    = viewMatrix[9];
        float fwdX   = viewMatrix[2], fwdY   = viewMatrix[6], fwdZ   = viewMatrix[10];

        glEnable(GL_BLEND);
        if (solidParticles) {
            // Solid mode: foreground particle completely replaces what's
            // behind. Combined with depth write below, this gives the
            // "opaque, no see-through" look the knob promises.
            glBlendFunc(GL_ONE, GL_ZERO);
        } else if (blendMode == 0) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);         // Additive
        } else {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Over
        }

        // Solid: write to depth so particles occlude each other (and the
        // scene). Default: keep depth-write off so particles blend
        // through each other while still respecting scene geometry's
        // depth (depth test stays on).
        glDepthMask(solidParticles ? GL_TRUE : GL_FALSE);

        // Note: GL_LINE_SMOOTH/POLYGON_SMOOTH removed — MSAA handles AA properly.
        // GL_POINT_SMOOTH still useful for round point sprites in Point mode.
        glEnable(GL_POINT_SMOOTH);
        glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);

        // Phase 3E.5 — re-enable multi-attachment draw buffers so particles can
        // emit AOVs. The geo loop's restore-to-1 happens above; we now restore
        // the full attachment set for the particle pass, then drop back to
        // single-attachment after particles for the volume pass.
        if (wantsAnyMrt) {
            GLenum drawBufs[5] = {
                (GLenum)GL_COLOR_ATTACHMENT0,
                (GLenum)(wantsNormalMrt   ? GL_COLOR_ATTACHMENT1 : GL_NONE),
                (GLenum)(wantsUvMrt       ? GL_COLOR_ATTACHMENT2 : GL_NONE),
                (GLenum)(wantsPrefMrt     ? GL_COLOR_ATTACHMENT3 : GL_NONE),
                (GLenum)(wantsVelocityMrt ? GL_COLOR_ATTACHMENT4 : GL_NONE),
            };
            glDrawBuffers(5, drawBufs);

            // Phase 3E.5 — per-attachment blend override. Beauty keeps the
            // additive/over blend set just above; the AOV attachments use
            // GL_ONE / GL_ZERO (replace) so values don't accumulate across
            // overlapping particle fragments (otherwise stacking saturates
            // Normal/Pref/UV to nonsense — the same camera-facing normal
            // summed N times for N overlapping particles, etc.). glBlendFunci
            // is GL 4.0 core; on a 3.3 core context the symbol is still loaded
            // by Natron's GL loader on any driver that supports 4.x underneath
            // (any NVIDIA / AMD / Intel from ~2010 onward).
            glBlendFunci(1, GL_ONE, GL_ZERO);
            glBlendFunci(2, GL_ONE, GL_ZERO);
            glBlendFunci(3, GL_ONE, GL_ZERO);
            glBlendFunci(4, GL_ONE, GL_ZERO);
        }

        // Phase 3E.5 — common AOV defaults applied to every particle vertex
        // before push_back. Per-mode branches override UVs (Sprite static) or
        // normals (Sphere static) on top of these defaults.
        //   - normal:   +fwd — billboard's outward face, pointing toward the
        //               camera. Note that `fwd` in this code is row 2 of the
        //               view matrix, i.e. the world-space direction that maps
        //               to +Z in camera space (= behind the camera = where the
        //               camera sits in world). For a camera-facing billboard
        //               the surface normal points toward the camera, so the
        //               correct sign is +fwd. (Using -fwd produces a normal
        //               pointing away from the camera, which the viewer's
        //               [0,1] clamp displays as black.)
        //   - uv:       (0, 0)   (overridden by Sprite static corners)
        //   - pref:     particle world position (per-particle stable id)
        //   - velocity: particle per-frame displacement (drives Velocity AOV)
        auto fillAovs = [&](ParticleVertex& v, const Particle& p) {
            v.nx = fwdX; v.ny = fwdY; v.nz = fwdZ;
            v.u  = 0.0f; v.v  = 0.0f;
            v.prefX = p.px; v.prefY = p.py; v.prefZ = p.pz;
            v.velX  = p.vx; v.velY  = p.vy; v.velZ  = p.vz;
        };

        // Phase 3E.5 — bind the particle program once for the whole partMode
        // cascade and set all uniforms (matrix + AOV write flags + screen
        // resolution for the Velocity AOV pixel scaling). Per-mode branches
        // below just build their vert buffer and call drawParticlePrimitives.
        if (glslParticleProg) {
            glUseProgram(glslParticleProg);
            GLint lProjView      = glGetUniformLocation(glslParticleProg, "u_projView");
            GLint lPrevProjView  = glGetUniformLocation(glslParticleProg, "u_prevProjView");
            GLint lResolution    = glGetUniformLocation(glslParticleProg, "u_resolution");
            GLint lWriteNormal   = glGetUniformLocation(glslParticleProg, "u_writeNormal");
            GLint lWriteUV       = glGetUniformLocation(glslParticleProg, "u_writeUV");
            GLint lWritePref     = glGetUniformLocation(glslParticleProg, "u_writePref");
            GLint lWriteVelocity = glGetUniformLocation(glslParticleProg, "u_writeVelocity");
            if (lProjView      >= 0) glUniformMatrix4fv(lProjView, 1, GL_FALSE, projViewMatrix);
            if (lPrevProjView  >= 0) glUniformMatrix4fv(lPrevProjView, 1, GL_FALSE, prevProjViewMatrix);
            if (lResolution    >= 0) glUniform2f(lResolution, (float)outW, (float)outH);
            if (lWriteNormal   >= 0) glUniform1i(lWriteNormal,   wantsNormalMrt   ? 1 : 0);
            if (lWriteUV       >= 0) glUniform1i(lWriteUV,       wantsUvMrt       ? 1 : 0);
            if (lWritePref     >= 0) glUniform1i(lWritePref,     wantsPrefMrt     ? 1 : 0);
            if (lWriteVelocity >= 0) glUniform1i(lWriteVelocity, wantsVelocityMrt ? 1 : 0);
        }

        if (partMode == 0 && glslParticleProg) {
            // --- Point mode (Phase 3E.1 GLSL) ---
            // GL_LINES streaks when motion-blur stretch is on, else GL_POINTS.
            // Point/line size from the compat-profile glPointSize/glLineWidth,
            // round-point appearance from GL_POINT_SMOOTH (already enabled
            // above).
            glPointSize(3.0f * globalScale);
            glLineWidth(2.0f * globalScale);

            if (motionBlur > 0.001f) {
                // Streak path: two vertices per particle (tail with alpha=0,
                // head with full alpha). Drawn as GL_LINES with vertex-color
                // interpolation across the segment.
                std::vector<ParticleVertex> verts;
                verts.reserve((size_t)particleData->numParticles() * 2);
                for (int i = 0; i < particleData->numParticles(); ++i) {
                    const Particle& p = particleData->particles[i];
                    const float pe = emissiveBoost ? p.emission : 1.0f;
                    float alpha = p.a;
                    if (alpha < 0.001f) continue;
                    float vx = p.vx, vy = p.vy, vz = p.vz;
                    float vDotF = vx * fwdX + vy * fwdY + vz * fwdZ;
                    float vPX = vx - vDotF * fwdX;
                    float vPY = vy - vDotF * fwdY;
                    float vPZ = vz - vDotF * fwdZ;
                    float vLen = std::sqrt(vPX*vPX + vPY*vPY + vPZ*vPZ);
                    if (vLen < 0.0001f) {
                        ParticleVertex v = { p.px, p.py, p.pz, p.r * pe, p.g * pe, p.b * pe, alpha };
                        fillAovs(v, p);
                        verts.push_back(v); verts.push_back(v);
                        continue;
                    }
                    float stretch = vLen * motionBlur;
                    float invLen = 1.0f / vLen;
                    ParticleVertex tail = {
                        p.px - vPX*invLen*stretch, p.py - vPY*invLen*stretch, p.pz - vPZ*invLen*stretch,
                        p.r, p.g, p.b, 0.0f
                    };
                    ParticleVertex head = { p.px, p.py, p.pz, p.r * pe, p.g * pe, p.b * pe, alpha };
                    fillAovs(tail, p); fillAovs(head, p);
                    verts.push_back(tail);
                    verts.push_back(head);
                }
                drawParticlePrimitives(GL_LINES, verts);
            } else {
                std::vector<ParticleVertex> verts;
                verts.reserve((size_t)particleData->numParticles());
                for (int i = 0; i < particleData->numParticles(); ++i) {
                    const Particle& p = particleData->particles[i];
                    const float pe = emissiveBoost ? p.emission : 1.0f;
                    float alpha = p.a;
                    if (alpha < 0.001f) continue;
                    ParticleVertex v = { p.px, p.py, p.pz, p.r * pe, p.g * pe, p.b * pe, alpha };
                    fillAovs(v, p);
                    verts.push_back(v);
                }
                drawParticlePrimitives(GL_POINTS, verts);
            }

        } else if (partMode == 1 && glslParticleProg) {
            // --- Disc mode (Phase 3E.2 GLSL) ---
            // Camera-facing soft-edge disc per particle. Decomposed to a
            // triangle list (center + edge[s] + edge[s+1] for s=0..segs-1)
            // so the whole set draws in one batched GL_TRIANGLES call.
            const int segments = 32;
            float cosTable[33], sinTable[33];
            for (int s = 0; s <= segments; ++s) {
                float a = 2.0f * (float)M_PI * s / segments;
                cosTable[s] = std::cos(a);
                sinTable[s] = std::sin(a);
            }

            std::vector<ParticleVertex> verts;
            verts.reserve((size_t)particleData->numParticles() * (size_t)segments * 3u);

            for (int i = 0; i < particleData->numParticles(); ++i) {
                const Particle& p = particleData->particles[i];
                const float pe = emissiveBoost ? p.emission : 1.0f;
                float alpha = p.a;
                if (alpha < 0.001f) continue;
                float hs = p.size * 0.5f * globalScale;
                bool didMotionBlur = false;

                if (motionBlur > 0.001f) {
                    float vx = p.vx, vy = p.vy, vz = p.vz;
                    float vDotF = vx * fwdX + vy * fwdY + vz * fwdZ;
                    float vPX = vx - vDotF * fwdX;
                    float vPY = vy - vDotF * fwdY;
                    float vPZ = vz - vDotF * fwdZ;
                    float vLen = std::sqrt(vPX*vPX + vPY*vPY + vPZ*vPZ);

                    if (vLen > 0.0001f) {
                        float dx = vPX / vLen, dy = vPY / vLen, dz = vPZ / vLen;
                        float px = fwdY * dz - fwdZ * dy;
                        float py = fwdZ * dx - fwdX * dz;
                        float pz = fwdX * dy - fwdY * dx;
                        float pLen = std::sqrt(px*px + py*py + pz*pz);
                        if (pLen > 0.0001f) { px /= pLen; py /= pLen; pz /= pLen; }

                        float halfLen = vLen * motionBlur * 0.5f + hs;
                        float halfWid = hs;
                        float cx = p.px - dx * halfLen * 0.5f;
                        float cy = p.py - dy * halfLen * 0.5f;
                        float cz = p.pz - dz * halfLen * 0.5f;

                        const int segs = 16;
                        const float edgeAlpha = solidParticles ? alpha : 0.0f;
                        ParticleVertex center = { p.px, p.py, p.pz, p.r * pe, p.g * pe, p.b * pe, alpha };
                        fillAovs(center, p);
                        // Pre-compute ring of edge verts once
                        std::vector<ParticleVertex> ring((size_t)segs + 1);
                        for (int s = 0; s <= segs; ++s) {
                            float a = 2.0f * (float)M_PI * s / segs;
                            float ca = std::cos(a), sa = std::sin(a);
                            float ex = dx * ca * halfLen + px * sa * halfWid;
                            float ey = dy * ca * halfLen + py * sa * halfWid;
                            float ez = dz * ca * halfLen + pz * sa * halfWid;
                            ring[s] = { cx + ex, cy + ey, cz + ez, p.r, p.g, p.b, edgeAlpha };
                            fillAovs(ring[s], p);
                        }
                        for (int s = 0; s < segs; ++s) {
                            verts.push_back(center);
                            verts.push_back(ring[s]);
                            verts.push_back(ring[s + 1]);
                        }
                        didMotionBlur = true;
                    }
                }

                if (!didMotionBlur) {
                    const float edgeAlpha = solidParticles ? alpha : 0.0f;
                    ParticleVertex center = { p.px, p.py, p.pz, p.r * pe, p.g * pe, p.b * pe, alpha };
                    fillAovs(center, p);
                    // Pre-compute ring of edge verts once
                    std::vector<ParticleVertex> ring((size_t)segments + 1);
                    for (int s = 0; s <= segments; ++s) {
                        float ex = rightX * cosTable[s] + upX * sinTable[s];
                        float ey = rightY * cosTable[s] + upY * sinTable[s];
                        float ez = rightZ * cosTable[s] + upZ * sinTable[s];
                        ring[s] = { p.px + ex * hs, p.py + ey * hs, p.pz + ez * hs,
                                    p.r, p.g, p.b, edgeAlpha };
                        fillAovs(ring[s], p);
                    }
                    for (int s = 0; s < segments; ++s) {
                        verts.push_back(center);
                        verts.push_back(ring[s]);
                        verts.push_back(ring[s + 1]);
                    }
                }
            }

            drawParticlePrimitives(GL_TRIANGLES, verts);

        } else if (partMode == 2 && glslParticleProg) {
            // --- Sphere mode (Phase 3E.2 GLSL) ---
            // Motion blur: elliptical fan per particle (same shape as Disc).
            // No motion blur: full sphere mesh with per-vertex N.L shading
            // pre-baked into the vertex colors. All particles batched into one
            // GL_TRIANGLES draw call.
            if (motionBlur < 0.001f) {
                glDepthMask(GL_TRUE);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
            }
            // Light direction in view space (from top-right)
            float lx = 0.5f, ly = 0.7f, lz = 0.5f;
            float ll = std::sqrt(lx*lx + ly*ly + lz*lz);
            lx /= ll; ly /= ll; lz /= ll;

            const int rings = 10, sectors = 14;
            std::vector<ParticleVertex> verts;
            verts.reserve((size_t)particleData->numParticles()
                          * (size_t)(rings * sectors * 6));

            for (int i = 0; i < particleData->numParticles(); ++i) {
                const Particle& p = particleData->particles[i];
                const float pe = emissiveBoost ? p.emission : 1.0f;
                float alpha = p.a;
                if (alpha < 0.001f) continue;
                float rad = p.size * 0.5f * globalScale;
                bool didMotionBlur = false;

                if (motionBlur > 0.001f) {
                    float vx = p.vx, vy = p.vy, vz = p.vz;
                    float vDotF = vx * fwdX + vy * fwdY + vz * fwdZ;
                    float vPX = vx - vDotF * fwdX;
                    float vPY = vy - vDotF * fwdY;
                    float vPZ = vz - vDotF * fwdZ;
                    float vLen = std::sqrt(vPX*vPX + vPY*vPY + vPZ*vPZ);
                    if (vLen > 0.0001f) {
                        float ddx = vPX / vLen, ddy = vPY / vLen, ddz = vPZ / vLen;
                        float ppx = fwdY * ddz - fwdZ * ddy;
                        float ppy = fwdZ * ddx - fwdX * ddz;
                        float ppz = fwdX * ddy - fwdY * ddx;
                        float pLen = std::sqrt(ppx*ppx + ppy*ppy + ppz*ppz);
                        if (pLen > 0.0001f) { ppx /= pLen; ppy /= pLen; ppz /= pLen; }

                        float halfLen = vLen * motionBlur * 0.5f + rad;
                        float halfWid = rad;
                        float cx = p.px - ddx * halfLen * 0.5f;
                        float cy = p.py - ddy * halfLen * 0.5f;
                        float cz = p.pz - ddz * halfLen * 0.5f;

                        const int segs = 16;
                        const float edgeAlpha = solidParticles ? alpha : 0.0f;
                        ParticleVertex center = { p.px, p.py, p.pz, p.r * pe, p.g * pe, p.b * pe, alpha };
                        fillAovs(center, p);
                        std::vector<ParticleVertex> ring((size_t)segs + 1);
                        for (int s = 0; s <= segs; ++s) {
                            float a = 2.0f * (float)M_PI * s / segs;
                            float ca = std::cos(a), sa = std::sin(a);
                            float ex = ddx * ca * halfLen + ppx * sa * halfWid;
                            float ey = ddy * ca * halfLen + ppy * sa * halfWid;
                            float ez = ddz * ca * halfLen + ppz * sa * halfWid;
                            ring[s] = { cx + ex, cy + ey, cz + ez, p.r, p.g, p.b, edgeAlpha };
                            fillAovs(ring[s], p);
                        }
                        for (int s = 0; s < segs; ++s) {
                            verts.push_back(center);
                            verts.push_back(ring[s]);
                            verts.push_back(ring[s + 1]);
                        }
                        didMotionBlur = true;
                    }
                }

                if (didMotionBlur) continue;

                // Solid shaded sphere mesh, with N.L shading per vertex.
                auto worldPos = [&](float nx, float ny, float nz, float& wx, float& wy, float& wz) {
                    wx = p.px + (rightX * nx + upX * ny + fwdX * nz) * rad;
                    wy = p.py + (rightY * nx + upY * ny + fwdY * nz) * rad;
                    wz = p.pz + (rightZ * nx + upZ * ny + fwdZ * nz) * rad;
                };
                auto shade = [&](float nx, float ny, float nz) -> float {
                    float d = nx * lx + ny * ly + nz * lz;
                    return 0.15f + 0.85f * std::max(0.0f, d);
                };
                auto pushV = [&](float nx, float ny, float nz) {
                    float wx, wy, wz; worldPos(nx, ny, nz, wx, wy, wz);
                    float s = shade(nx, ny, nz);
                    ParticleVertex v = { wx, wy, wz, p.r * s, p.g * s, p.b * s, alpha };
                    fillAovs(v, p);
                    // Sphere static — override the camera-facing default with
                    // the true per-vertex world-space normal, and the per-vertex
                    // unit-sphere offset as Pref (object-space reference position).
                    v.nx = nx; v.ny = ny; v.nz = nz;
                    v.prefX = nx; v.prefY = ny; v.prefZ = nz;
                    verts.push_back(v);
                };
                for (int r = 0; r < rings; ++r) {
                    float phi0 = (float)M_PI * r / rings;
                    float phi1 = (float)M_PI * (r + 1) / rings;
                    float cp0 = std::cos(phi0), sp0 = std::sin(phi0);
                    float cp1 = std::cos(phi1), sp1 = std::sin(phi1);
                    for (int s = 0; s < sectors; ++s) {
                        float th0 = 2.0f * (float)M_PI * s / sectors;
                        float th1 = 2.0f * (float)M_PI * (s + 1) / sectors;
                        float ct0 = std::cos(th0), st0 = std::sin(th0);
                        float ct1 = std::cos(th1), st1 = std::sin(th1);
                        float nx00 = sp0*ct0, ny00 = cp0, nz00 = sp0*st0;
                        float nx10 = sp1*ct0, ny10 = cp1, nz10 = sp1*st0;
                        float nx01 = sp0*ct1, ny01 = cp0, nz01 = sp0*st1;
                        float nx11 = sp1*ct1, ny11 = cp1, nz11 = sp1*st1;
                        // Triangle 1
                        pushV(nx00, ny00, nz00);
                        pushV(nx10, ny10, nz10);
                        pushV(nx11, ny11, nz11);
                        // Triangle 2
                        pushV(nx00, ny00, nz00);
                        pushV(nx11, ny11, nz11);
                        pushV(nx01, ny01, nz01);
                    }
                }
            }

            drawParticlePrimitives(GL_TRIANGLES, verts);

            // Restore for subsequent rendering. Keep depth-write enabled
            // if Solid mode is on (otherwise we'd undo the global state
            // set above and subsequent particle batches would no longer
            // occlude properly).
            if (motionBlur < 0.001f) {
                glDisable(GL_CULL_FACE);
                if (!solidParticles) glDepthMask(GL_FALSE);
            }

        } else if (partMode == 3 && glslParticleProg) {
            // --- Sprite mode (Phase 3E.3 GLSL) ---
            // Camera-facing colored quad per particle; elliptical soft fan when
            // motion blur is on. All particles batched into one GL_TRIANGLES
            // draw call. Note: the legacy "Sprite" was always a flat-colored
            // billboard (no texture sampling) — same here.
            std::vector<ParticleVertex> verts;
            verts.reserve((size_t)particleData->numParticles() * 6u);

            for (int i = 0; i < particleData->numParticles(); ++i) {
                const Particle& p = particleData->particles[i];
                const float pe = emissiveBoost ? p.emission : 1.0f;
                float alpha = p.a;
                if (alpha < 0.001f) continue;

                float hs = p.size * 0.5f * globalScale;
                bool didMotionBlur = false;

                if (motionBlur > 0.001f) {
                    float vx = p.vx, vy = p.vy, vz = p.vz;
                    float vDotF = vx * fwdX + vy * fwdY + vz * fwdZ;
                    float vPX = vx - vDotF * fwdX;
                    float vPY = vy - vDotF * fwdY;
                    float vPZ = vz - vDotF * fwdZ;
                    float vLen = std::sqrt(vPX*vPX + vPY*vPY + vPZ*vPZ);

                    if (vLen > 0.0001f) {
                        float dx = vPX / vLen, dy = vPY / vLen, dz = vPZ / vLen;
                        float px = fwdY * dz - fwdZ * dy;
                        float py = fwdZ * dx - fwdX * dz;
                        float pz = fwdX * dy - fwdY * dx;
                        float pLen = std::sqrt(px*px + py*py + pz*pz);
                        if (pLen > 0.0001f) { px /= pLen; py /= pLen; pz /= pLen; }

                        float halfLen = vLen * motionBlur * 0.5f + hs;
                        float halfWid = hs;
                        // Shift the ellipse center backward so the bright spot
                        // lines up with the particle's current position (head
                        // of the streak); the tail trails behind.
                        float centerOffsetBack = halfLen * 0.5f;
                        float cx = p.px - dx * centerOffsetBack;
                        float cy = p.py - dy * centerOffsetBack;
                        float cz = p.pz - dz * centerOffsetBack;
                        float ellLen = halfLen;

                        const int segs = 16;
                        const float edgeAlpha = solidParticles ? alpha : 0.0f;
                        ParticleVertex center = { p.px, p.py, p.pz, p.r * pe, p.g * pe, p.b * pe, alpha };
                        fillAovs(center, p);
                        std::vector<ParticleVertex> ring((size_t)segs + 1);
                        for (int s = 0; s <= segs; ++s) {
                            float a = 2.0f * (float)M_PI * s / segs;
                            float ca = std::cos(a), sa = std::sin(a);
                            float ex = dx * ca * ellLen + px * sa * halfWid;
                            float ey = dy * ca * ellLen + py * sa * halfWid;
                            float ez = dz * ca * ellLen + pz * sa * halfWid;
                            ring[s] = { cx + ex, cy + ey, cz + ez, p.r, p.g, p.b, edgeAlpha };
                            fillAovs(ring[s], p);
                        }
                        for (int s = 0; s < segs; ++s) {
                            verts.push_back(center);
                            verts.push_back(ring[s]);
                            verts.push_back(ring[s + 1]);
                        }
                        didMotionBlur = true;
                    }
                }

                if (didMotionBlur) continue;

                // Plain camera-facing colored quad → 2 triangles. UVs are the
                // natural quad corner layout BL=(0,0), BR=(1,0), TR=(1,1),
                // TL=(0,1) — these are the only natural per-vertex UVs in any
                // particle mode.
                float rx = rightX * hs, ry = rightY * hs, rz = rightZ * hs;
                float ux = upX * hs,    uy = upY * hs,    uz = upZ * hs;
                ParticleVertex bl = { p.px - rx - ux, p.py - ry - uy, p.pz - rz - uz, p.r * pe, p.g * pe, p.b * pe, alpha };
                ParticleVertex br = { p.px + rx - ux, p.py + ry - uy, p.pz + rz - uz, p.r * pe, p.g * pe, p.b * pe, alpha };
                ParticleVertex tr = { p.px + rx + ux, p.py + ry + uy, p.pz + rz + uz, p.r * pe, p.g * pe, p.b * pe, alpha };
                ParticleVertex tl = { p.px - rx + ux, p.py - ry + uy, p.pz - rz + uz, p.r * pe, p.g * pe, p.b * pe, alpha };
                fillAovs(bl, p); fillAovs(br, p); fillAovs(tr, p); fillAovs(tl, p);
                bl.u = 0.0f; bl.v = 0.0f;
                br.u = 1.0f; br.v = 0.0f;
                tr.u = 1.0f; tr.v = 1.0f;
                tl.u = 0.0f; tl.v = 1.0f;
                verts.push_back(bl); verts.push_back(br); verts.push_back(tr);
                verts.push_back(bl); verts.push_back(tr); verts.push_back(tl);
            }

            drawParticlePrimitives(GL_TRIANGLES, verts);
        } else if (partMode == 4 && glslParticleProg) {
            // --- Trail mode: multi-frame camera-facing ribbon through each
            // particle's PAST positions (Houdini particle-trail style). The
            // ribbon follows the real path, so bounces render as bent Vs and
            // arcs curve — unlike the straight velocity streaks. Width, alpha
            // and color taper head -> tail via the Trail knobs.
            const int trailLen = _imp->trailLength.lock() ? _imp->trailLength.lock()->getValueAtTime(args.time) : 4;
            const float headW = _imp->trailHeadWidth.lock() ? (float)_imp->trailHeadWidth.lock()->getValueAtTime(args.time) : 1.0f;
            const float tailW = _imp->trailTailWidth.lock() ? (float)_imp->trailTailWidth.lock()->getValueAtTime(args.time) : 0.25f;
            const float tailFade = _imp->trailTailFade.lock() ? (float)_imp->trailTailFade.lock()->getValueAtTime(args.time) : 0.0f;
            float tintR = 1.f, tintG = 1.f, tintB = 1.f;
            if (KnobColorPtr tk = _imp->trailTailTint.lock()) {
                tintR = (float)tk->getValueAtTime(args.time, 0);
                tintG = (float)tk->getValueAtTime(args.time, 1);
                tintB = (float)tk->getValueAtTime(args.time, 2);
            }

            // Past-frame snapshots, id -> particle. The solver's frame cache
            // makes these lookups cheap after the first render.
            std::vector<std::unordered_map<uint32_t, const Particle*> > history;
            std::vector<ParticleDataPtr> historyData; // keeps snapshots alive
            if (trailProvider) {
                for (int k = 1; k <= trailLen; ++k) {
                    ParticleDataPtr past = trailProvider->getParticleData(args.time - k);
                    if (!past || past->particles.empty()) break;
                    historyData.push_back(past);
                    history.push_back(std::unordered_map<uint32_t, const Particle*>());
                    std::unordered_map<uint32_t, const Particle*>& m = history.back();
                    m.reserve(past->particles.size());
                    for (size_t q = 0; q < past->particles.size(); ++q) {
                        m[past->particles[q].id] = &past->particles[q];
                    }
                }
            }

            std::vector<ParticleVertex> verts;
            verts.reserve((size_t)particleData->numParticles() * (size_t)(trailLen * 6));
            std::vector<const Particle*> chain;
            chain.reserve((size_t)trailLen + 1);
            Particle synth; // synthesized 1-frame-back tail for newborns

            for (int i = 0; i < particleData->numParticles(); ++i) {
                const Particle& p = particleData->particles[i];
                const float pe = emissiveBoost ? p.emission : 1.0f;
                float alpha = p.a;
                if (alpha < 0.001f) continue;

                // Head (current) then progressively older, matched BY ID —
                // stop where the particle didn't exist yet.
                chain.clear();
                chain.push_back(&p);
                for (size_t k = 0; k < history.size(); ++k) {
                    std::unordered_map<uint32_t, const Particle*>::const_iterator it = history[k].find(p.id);
                    if (it == history[k].end()) break;
                    chain.push_back(it->second);
                }
                if (chain.size() < 2) {
                    // Newborn with no history — synthesize a one-frame-back
                    // tail from velocity so it doesn't pop invisible.
                    synth = p;
                    synth.px = p.px - p.vx;
                    synth.py = p.py - p.vy;
                    synth.pz = p.pz - p.vz;
                    chain.push_back(&synth);
                }
                const int nPts = (int)chain.size();

                // Ribbon: two vertices per chain point, offset along the
                // screen-space right vector (perpendicular to both the local
                // trail direction and the camera forward).
                float prevLx = 0, prevLy = 0, prevLz = 0, prevRx = 0, prevRy = 0, prevRz = 0;
                float prevA = 0, prevCr = 0, prevCg = 0, prevCb = 0;
                for (int j = 0; j < nPts; ++j) {
                    const Particle& cp = *chain[j];
                    // taper parameter over the FULL requested length so short
                    // chains keep their head look
                    const float t = (trailLen > 0) ? (float)j / (float)trailLen : 0.f;
                    const float w = cp.size * 0.5f * globalScale * (headW + (tailW - headW) * t);
                    const float aj = alpha * (1.0f + (tailFade - 1.0f) * t);
                    const float cr = p.r * pe * (1.0f + (tintR - 1.0f) * t);
                    const float cg = p.g * pe * (1.0f + (tintG - 1.0f) * t);
                    const float cb = p.b * pe * (1.0f + (tintB - 1.0f) * t);

                    // Local direction: central difference where possible
                    const Particle& pn = *chain[(j + 1 < nPts) ? j + 1 : j];
                    const Particle& pp2 = *chain[(j > 0) ? j - 1 : j];
                    float dx = pp2.px - pn.px, dy = pp2.py - pn.py, dz = pp2.pz - pn.pz;
                    float dLen = std::sqrt(dx*dx + dy*dy + dz*dz);
                    if (dLen < 1e-6f) { dx = 1; dy = 0; dz = 0; dLen = 1; }
                    dx /= dLen; dy /= dLen; dz /= dLen;
                    // right = dir x fwd (camera-facing ribbon)
                    float rX = dy * fwdZ - dz * fwdY;
                    float rY = dz * fwdX - dx * fwdZ;
                    float rZ = dx * fwdY - dy * fwdX;
                    float rLen = std::sqrt(rX*rX + rY*rY + rZ*rZ);
                    if (rLen < 1e-6f) { rX = rightX; rY = rightY; rZ = rightZ; rLen = 1; }
                    rX /= rLen; rY /= rLen; rZ /= rLen;

                    const float lx = cp.px - rX * w, ly = cp.py - rY * w, lz = cp.pz - rZ * w;
                    const float rx2 = cp.px + rX * w, ry2 = cp.py + rY * w, rz2 = cp.pz + rZ * w;

                    if (j > 0) {
                        ParticleVertex v0 = { prevLx, prevLy, prevLz, prevCr, prevCg, prevCb, prevA };
                        ParticleVertex v1 = { prevRx, prevRy, prevRz, prevCr, prevCg, prevCb, prevA };
                        ParticleVertex v2 = { rx2, ry2, rz2, cr, cg, cb, aj };
                        ParticleVertex v3 = { lx, ly, lz, cr, cg, cb, aj };
                        fillAovs(v0, p); fillAovs(v1, p); fillAovs(v2, p); fillAovs(v3, p);
                        verts.push_back(v0); verts.push_back(v1); verts.push_back(v2);
                        verts.push_back(v0); verts.push_back(v2); verts.push_back(v3);
                    }
                    prevLx = lx; prevLy = ly; prevLz = lz;
                    prevRx = rx2; prevRy = ry2; prevRz = rz2;
                    prevA = aj; prevCr = cr; prevCg = cg; prevCb = cb;
                }
            }

            drawParticlePrimitives(GL_TRIANGLES, verts);
        }

        // Phase 3E.5 — unbind particle program, restore single-attachment
        // draw buffer for the volume pass, and unify blend func across all
        // attachments (the per-attachment overrides above only affect AOV
        // slots, but a unified reset is cheaper than tracking which ones
        // were touched and is robust to downstream code).
        if (glslParticleProg) glUseProgram(0);
        if (wantsAnyMrt) {
            GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, drawBufs);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        // Restore state
        glDepthMask(GL_TRUE);
        glDisable(GL_POINT_SMOOTH);
    }

    // --- Render geo instances at particle positions ---
    // VBO/VAO + uniform-driven kInstanceVert/Frag. One VBO per geo type
    // (built once per render), one small draw call per instance. Instances
    // participate in MRT: Normal / UV / Pref / Velocity AOVs are all written.
    // Use sub-frame time so multi-sample motion blur actually queries the
    // particle/instance state at each shutter slot. Falls back to args.time
    // outside the multi-sample loop (sampleTime == args.time when sample == 0
    // and motionSamples == 1).
    const double instanceTime = (motionSamples > 1) ? sampleTime : args.time;
    if (particleInstancer && glslInstanceProg) {
        std::vector<ParticleInstance::GeoInstance> instances;
        // Build instances from our (privately offset) particle data rather
        // than letting the instancer re-pull the provider's shared snapshot —
        // that snapshot no longer carries the per-sample offsets.
        particleInstancer->getInstancesFromData(motionBlurPData, instanceTime, instances);

        if (!instances.empty()) {
            glEnable(GL_DEPTH_TEST);
            if (motionBlur > 0.001f) {
                glDepthMask(GL_FALSE); // stretch cheat mode — translucent
            } else {
                glDepthMask(GL_TRUE);  // solid opaque geo (multi-sample averages give the blur)
            }
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Re-enable MRT so instances can write Normal/UV/Pref/Velocity AOVs
            // (same pattern as the particle pass — beauty keeps over-blend, AOV
            // attachments use replace so AOV values don't accumulate across
            // overlapping fragments).
            if (wantsAnyMrt) {
                GLenum drawBufs[5] = {
                    (GLenum)GL_COLOR_ATTACHMENT0,
                    (GLenum)(wantsNormalMrt   ? GL_COLOR_ATTACHMENT1 : GL_NONE),
                    (GLenum)(wantsUvMrt       ? GL_COLOR_ATTACHMENT2 : GL_NONE),
                    (GLenum)(wantsPrefMrt     ? GL_COLOR_ATTACHMENT3 : GL_NONE),
                    (GLenum)(wantsVelocityMrt ? GL_COLOR_ATTACHMENT4 : GL_NONE),
                };
                glDrawBuffers(5, drawBufs);
                glBlendFunci(1, GL_ONE, GL_ZERO);
                glBlendFunci(2, GL_ONE, GL_ZERO);
                glBlendFunci(3, GL_ONE, GL_ZERO);
                glBlendFunci(4, GL_ONE, GL_ZERO);
            }

            // Per-geo local-space mesh cache. One VBO per geo type, reused
            // across every instance of that type via uniform-driven transforms.
            // Interleaved stride = 8 floats (pos3 + normal3 + uv2).
            struct InstanceGeo {
                std::vector<float> interleaved; // pos.xyz, normal.xyz, uv.xy per vertex
                int numVerts = 0;
                GLuint vao = 0;
                GLuint vbo = 0;
                bool valid = false;
                float bboxCenterY = 0.0f;
                float bboxHalfY   = 1.0f;
            };
            InstanceGeo geos[4] = {};

            // Builds per-vertex flat normals from triangles. The triangulated
            // mesh is expanded into a flat-shaded one (3 unique vertices per
            // triangle, each with the triangle's face normal + the source
            // vertex's UV). Cube/sphere both look correct under this scheme.
            auto buildFlatGeo = [](InstanceGeo& g,
                                   const std::vector<float>& verts,
                                   const std::vector<float>& uvs,
                                   const std::vector<int>& tris) {
                const int numTris = (int)(tris.size() / 3);
                g.interleaved.assign((size_t)numTris * 3 * 8, 0.0f);
                const bool hasUvs = ((int)uvs.size() >= ((int)verts.size() / 3) * 2);
                for (int t = 0; t < numTris; ++t) {
                    int i0 = tris[t*3 + 0];
                    int i1 = tris[t*3 + 1];
                    int i2 = tris[t*3 + 2];
                    float ax = verts[i0*3+0], ay = verts[i0*3+1], az = verts[i0*3+2];
                    float bx = verts[i1*3+0], by = verts[i1*3+1], bz = verts[i1*3+2];
                    float cx = verts[i2*3+0], cy = verts[i2*3+1], cz = verts[i2*3+2];
                    float ex = bx - ax, ey = by - ay, ez = bz - az;
                    float fx = cx - ax, fy = cy - ay, fz = cz - az;
                    float nx = ey*fz - ez*fy;
                    float ny = ez*fx - ex*fz;
                    float nz = ex*fy - ey*fx;
                    float nl = std::sqrt(nx*nx + ny*ny + nz*nz);
                    if (nl > 1e-8f) { nx /= nl; ny /= nl; nz /= nl; }
                    int idx[3] = { i0, i1, i2 };
                    float pos[3][3] = { {ax,ay,az}, {bx,by,bz}, {cx,cy,cz} };
                    for (int k = 0; k < 3; ++k) {
                        float* row = &g.interleaved[(size_t)(t*3 + k) * 8];
                        row[0] = pos[k][0]; row[1] = pos[k][1]; row[2] = pos[k][2];
                        row[3] = nx;        row[4] = ny;        row[5] = nz;
                        if (hasUvs) {
                            row[6] = uvs[idx[k]*2 + 0];
                            row[7] = uvs[idx[k]*2 + 1];
                        }
                    }
                }
                g.numVerts = numTris * 3;
                g.valid = (g.numVerts > 0);
            };

            // Extract local meshes from each connected geo input.
            for (int g = 0; g < 4; ++g) {
                EffectInstancePtr geoInput = particleInstancer->getInput(g + 1);
                if (!geoInput) continue;

                std::vector<float> verts;
                std::vector<float> uvs;
                std::vector<int> tris;

                // Try Cube3D
                Cube3D* cube = dynamic_cast<Cube3D*>(geoInput.get());
                if (cube) {
                    std::vector<Cube3D::CubeVertex> cv;
                    cube->generateCubeMesh(args.time, cv, tris);
                    verts.resize(cv.size() * 3);
                    uvs.resize(cv.size() * 2);
                    for (size_t v = 0; v < cv.size(); ++v) {
                        verts[v*3]   = cv[v].x;
                        verts[v*3+1] = cv[v].y;
                        verts[v*3+2] = cv[v].z;
                        uvs[v*2]     = cv[v].u;
                        uvs[v*2+1]   = cv[v].v;
                    }
                } else {
                    // Try Sphere3D — generate a simple unit-radius mesh with
                    // spherical UVs (longitude=u, latitude=v).
                    Sphere3D* sphere = dynamic_cast<Sphere3D*>(geoInput.get());
                    if (sphere) {
                        const int rings = 16, sectors = 24;
                        float rad = 0.5f;
                        for (int r = 0; r <= rings; ++r) {
                            float phi = (float)M_PI * r / rings;
                            float vCoord = (float)r / (float)rings;
                            for (int s = 0; s <= sectors; ++s) {
                                float theta = 2.0f * (float)M_PI * s / sectors;
                                float uCoord = (float)s / (float)sectors;
                                verts.push_back(rad * std::sin(phi) * std::cos(theta));
                                verts.push_back(rad * std::cos(phi));
                                verts.push_back(rad * std::sin(phi) * std::sin(theta));
                                uvs.push_back(uCoord);
                                uvs.push_back(vCoord);
                            }
                        }
                        for (int r = 0; r < rings; ++r) {
                            for (int s = 0; s < sectors; ++s) {
                                int i0 = r * (sectors + 1) + s;
                                int i1 = i0 + sectors + 1;
                                tris.push_back(i0);     tris.push_back(i1);     tris.push_back(i0 + 1);
                                tris.push_back(i0 + 1); tris.push_back(i1);     tris.push_back(i1 + 1);
                            }
                        }
                    }
                }

                if (verts.empty() || tris.empty()) continue;

                // Y-axis bbox for cheat-mode stretch-fade uniforms.
                float mnY = verts[1], mxY = verts[1];
                for (size_t v = 0; v < verts.size() / 3; ++v) {
                    float y = verts[v * 3 + 1];
                    if (y < mnY) mnY = y;
                    if (y > mxY) mxY = y;
                }
                geos[g].bboxCenterY = (mnY + mxY) * 0.5f;
                geos[g].bboxHalfY   = std::max(0.0001f, (mxY - mnY) * 0.5f);

                buildFlatGeo(geos[g], verts, uvs, tris);
                if (!geos[g].valid) continue;

                // Upload to a per-geo-type VBO/VAO, reused across all instances.
                glGenVertexArrays(1, &geos[g].vao);
                glBindVertexArray(geos[g].vao);
                glGenBuffers(1, &geos[g].vbo);
                glBindBuffer(GL_ARRAY_BUFFER, geos[g].vbo);
                glBufferData(GL_ARRAY_BUFFER,
                             (GLsizeiptr)(geos[g].interleaved.size() * sizeof(float)),
                             geos[g].interleaved.data(), GL_STREAM_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                                      (void*)(3 * sizeof(float)));
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                                      (void*)(6 * sizeof(float)));
                glBindVertexArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }

            // Bind program + look up uniform locations once (cheaper than
            // glGetUniformLocation per instance).
            glUseProgram(glslInstanceProg);
            const GLint locProjView      = glGetUniformLocation(glslInstanceProg, "u_projView");
            const GLint locPrevProjView  = glGetUniformLocation(glslInstanceProg, "u_prevProjView");
            const GLint locLocal         = glGetUniformLocation(glslInstanceProg, "u_localMatrix");
            const GLint locPrevLocal     = glGetUniformLocation(glslInstanceProg, "u_prevLocalMatrix");
            const GLint locNormal        = glGetUniformLocation(glslInstanceProg, "u_normalMatrix");
            const GLint locInstColor     = glGetUniformLocation(glslInstanceProg, "u_instanceColor");
            const GLint locFadeOn        = glGetUniformLocation(glslInstanceProg, "u_fadeEnabled");
            const GLint locFadeCenterY   = glGetUniformLocation(glslInstanceProg, "u_fadeCenterY");
            const GLint locFadeHalfY     = glGetUniformLocation(glslInstanceProg, "u_fadeHalfY");
            const GLint locShadingMode   = glGetUniformLocation(glslInstanceProg, "u_shadingMode");
            const GLint locHasLight      = glGetUniformLocation(glslInstanceProg, "u_hasLight");
            const GLint locLightPos      = glGetUniformLocation(glslInstanceProg, "u_lightPos");
            const GLint locLightColor    = glGetUniformLocation(glslInstanceProg, "u_lightColor");
            const GLint locLightInt      = glGetUniformLocation(glslInstanceProg, "u_lightIntensity");
            const GLint locCameraPos     = glGetUniformLocation(glslInstanceProg, "u_cameraPos");
            const GLint locWriteNormal   = glGetUniformLocation(glslInstanceProg, "u_writeNormal");
            const GLint locWriteUV       = glGetUniformLocation(glslInstanceProg, "u_writeUV");
            const GLint locWritePref     = glGetUniformLocation(glslInstanceProg, "u_writePref");
            const GLint locWriteVelocity = glGetUniformLocation(glslInstanceProg, "u_writeVelocity");
            const GLint locViewportSize  = glGetUniformLocation(glslInstanceProg, "u_viewportSize");
            const GLint locProjMode      = glGetUniformLocation(glslInstanceProg, "u_projMode");
            const GLint locViewMat       = glGetUniformLocation(glslInstanceProg, "u_view");
            const GLint locNearI         = glGetUniformLocation(glslInstanceProg, "u_near");
            const GLint locFarI          = glGetUniformLocation(glslInstanceProg, "u_far");
            const GLint locAmbientI      = glGetUniformLocation(glslInstanceProg, "u_ambient");
            const GLint locTranspI       = glGetUniformLocation(glslInstanceProg, "u_transparency");

            if (locProjMode     >= 0) glUniform1i(locProjMode, projMode);
            if (locAmbientI     >= 0) glUniform3fv(locAmbientI, 1, ambient);
            if (locTranspI      >= 0) glUniform1i(locTranspI, transparency);
            if (locViewMat      >= 0) glUniformMatrix4fv(locViewMat, 1, GL_FALSE, viewMatrix);
            if (locNearI        >= 0) glUniform1f(locNearI, camNear);
            if (locFarI         >= 0) glUniform1f(locFarI, camFar);
            if (locProjView     >= 0) glUniformMatrix4fv(locProjView,     1, GL_FALSE, projViewMatrix);
            if (locPrevProjView >= 0) glUniformMatrix4fv(locPrevProjView, 1, GL_FALSE, prevProjViewMatrix);
            if (locShadingMode  >= 0) glUniform1i(locShadingMode, shadingMode);
            if (locHasLight     >= 0) glUniform1i(locHasLight, hasLight ? 1 : 0);
            if (locLightPos     >= 0) glUniform3fv(locLightPos,   1, lightPos);
            if (locLightColor   >= 0) glUniform3fv(locLightColor, 1, lightColor);
            if (locLightInt     >= 0) glUniform1f(locLightInt, lightIntensity);
            if (locCameraPos    >= 0) glUniform3fv(locCameraPos, 1, cameraPos);
            if (locWriteNormal  >= 0) glUniform1i(locWriteNormal,   wantsNormalMrt   ? 1 : 0);
            if (locWriteUV      >= 0) glUniform1i(locWriteUV,       wantsUvMrt       ? 1 : 0);
            if (locWritePref    >= 0) glUniform1i(locWritePref,     wantsPrefMrt     ? 1 : 0);
            if (locWriteVelocity>= 0) glUniform1i(locWriteVelocity, wantsVelocityMrt ? 1 : 0);
            if (locViewportSize >= 0) glUniform2f(locViewportSize, (float)outW, (float)outH);

            // Per-instance loop — build localMatrix + normalMatrix, set
            // instance-specific uniforms, draw the per-geo VBO.
            for (size_t i = 0; i < instances.size(); ++i) {
                const ParticleInstance::GeoInstance& inst = instances[i];
                int gi = inst.geoSourceIndex;
                if (gi < 0 || gi >= 4 || !geos[gi].valid) continue;
                const InstanceGeo& geo = geos[gi];

                // Build the 4x4 local matrix (translate * rotate * scale).
                // Column-major layout, matching the rest of the file.
                //
                // Multi-sample MB: ParticleInstance only runs its sim once per
                // frame, so getInstances(sampleTime) returns frame-time data
                // for every sample. Extrapolate sub-frame position using the
                // per-instance velocity vector — same trick the particle MB
                // path uses. When motionSamples == 1 sampleDt is 0 and this
                // collapses to the frame-time position.
                const float instPx = inst.px + inst.vx * sampleDt;
                const float instPy = inst.py + inst.vy * sampleDt;
                const float instPz = inst.pz + inst.vz * sampleDt;
                float T[16] = {
                    1,0,0,0,  0,1,0,0,  0,0,1,0,
                    instPx, instPy, instPz, 1
                };

                // Rotation/scale path depends on stretch-mode.
                float RS[16];
                bool fadeEnabled = false;
                if (motionBlur > 0.001f) {
                    float vLen = std::sqrt(inst.vx*inst.vx + inst.vy*inst.vy + inst.vz*inst.vz);
                    if (vLen > 0.0001f) {
                        // Align local Y to velocity, then stretch Y by speed * motionBlur.
                        float vy_n = inst.vy / vLen;
                        float vx_n = inst.vx / vLen;
                        float vz_n = inst.vz / vLen;
                        float angleRad = std::acos(std::max(-1.0f, std::min(1.0f, vy_n)));
                        // axis = cross(Y, velocity) — only the XZ components survive
                        float axX = vz_n;
                        float axZ = -vx_n;
                        float axLen = std::sqrt(axX*axX + axZ*axZ);
                        float R[16] = {
                            1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1
                        };
                        if (axLen > 0.0001f && std::abs(angleRad) > 0.0001f) {
                            float c = std::cos(angleRad);
                            float s = std::sin(angleRad);
                            float ax = axX / axLen, az = axZ / axLen, ay = 0.0f;
                            // Rodrigues rotation, column-major
                            R[0]  = c + ax*ax*(1-c);  R[1]  = ay*ax*(1-c) + az*s; R[2]  = az*ax*(1-c) - ay*s; R[3]  = 0;
                            R[4]  = ax*ay*(1-c) - az*s; R[5] = c + ay*ay*(1-c);   R[6]  = az*ay*(1-c) + ax*s; R[7]  = 0;
                            R[8]  = ax*az*(1-c) + ay*s; R[9] = ay*az*(1-c) - ax*s; R[10] = c + az*az*(1-c);   R[11] = 0;
                            R[12] = 0; R[13] = 0; R[14] = 0; R[15] = 1;
                        }
                        float stretchAmt = 1.0f + vLen * motionBlur;
                        float S[16] = {
                            inst.sx, 0, 0, 0,
                            0, inst.sy * stretchAmt, 0, 0,
                            0, 0, inst.sz, 0,
                            0, 0, 0, 1
                        };
                        mat4Mul(RS, R, S);
                        fadeEnabled = true;
                    } else {
                        // No velocity → falls back to standard Euler rotations.
                        float Rx[16], Ry[16], Rz[16], Sm[16], tmp[16];
                        const float dx = inst.rx * (float)M_PI / 180.0f;
                        const float dy = inst.ry * (float)M_PI / 180.0f;
                        const float dz = inst.rz * (float)M_PI / 180.0f;
                        float cx = std::cos(dx), sx = std::sin(dx);
                        float cy = std::cos(dy), sy = std::sin(dy);
                        float cz = std::cos(dz), sz = std::sin(dz);
                        // X rotation
                        Rx[0]=1; Rx[1]=0;  Rx[2]=0;   Rx[3]=0;
                        Rx[4]=0; Rx[5]=cx; Rx[6]=sx;  Rx[7]=0;
                        Rx[8]=0; Rx[9]=-sx;Rx[10]=cx; Rx[11]=0;
                        Rx[12]=0;Rx[13]=0; Rx[14]=0;  Rx[15]=1;
                        // Y rotation
                        Ry[0]=cy;Ry[1]=0;  Ry[2]=-sy; Ry[3]=0;
                        Ry[4]=0; Ry[5]=1;  Ry[6]=0;   Ry[7]=0;
                        Ry[8]=sy;Ry[9]=0;  Ry[10]=cy; Ry[11]=0;
                        Ry[12]=0;Ry[13]=0; Ry[14]=0;  Ry[15]=1;
                        // Z rotation
                        Rz[0]=cz; Rz[1]=sz; Rz[2]=0;  Rz[3]=0;
                        Rz[4]=-sz;Rz[5]=cz; Rz[6]=0;  Rz[7]=0;
                        Rz[8]=0;  Rz[9]=0;  Rz[10]=1; Rz[11]=0;
                        Rz[12]=0; Rz[13]=0; Rz[14]=0; Rz[15]=1;
                        Sm[0]=inst.sx; Sm[1]=0;  Sm[2]=0;  Sm[3]=0;
                        Sm[4]=0;  Sm[5]=inst.sy; Sm[6]=0;  Sm[7]=0;
                        Sm[8]=0;  Sm[9]=0;  Sm[10]=inst.sz;Sm[11]=0;
                        Sm[12]=0; Sm[13]=0; Sm[14]=0; Sm[15]=1;
                        // Same composition order as the legacy code:
                        // glRotatef(ry,Y) glRotatef(rx,X) glRotatef(rz,Z) glScalef
                        mat4Mul(tmp, Ry, Rx);
                        mat4Mul(RS, tmp, Rz);
                        mat4Mul(tmp, RS, Sm);
                        std::memcpy(RS, tmp, sizeof(RS));
                    }
                } else {
                    // Standard rotation + scale (no stretch).
                    float Rx[16], Ry[16], Rz[16], Sm[16], tmp[16];
                    const float dx = inst.rx * (float)M_PI / 180.0f;
                    const float dy = inst.ry * (float)M_PI / 180.0f;
                    const float dz = inst.rz * (float)M_PI / 180.0f;
                    float cx = std::cos(dx), sx = std::sin(dx);
                    float cy = std::cos(dy), sy = std::sin(dy);
                    float cz = std::cos(dz), sz = std::sin(dz);
                    Rx[0]=1; Rx[1]=0;  Rx[2]=0;   Rx[3]=0;
                    Rx[4]=0; Rx[5]=cx; Rx[6]=sx;  Rx[7]=0;
                    Rx[8]=0; Rx[9]=-sx;Rx[10]=cx; Rx[11]=0;
                    Rx[12]=0;Rx[13]=0; Rx[14]=0;  Rx[15]=1;
                    Ry[0]=cy;Ry[1]=0;  Ry[2]=-sy; Ry[3]=0;
                    Ry[4]=0; Ry[5]=1;  Ry[6]=0;   Ry[7]=0;
                    Ry[8]=sy;Ry[9]=0;  Ry[10]=cy; Ry[11]=0;
                    Ry[12]=0;Ry[13]=0; Ry[14]=0;  Ry[15]=1;
                    Rz[0]=cz; Rz[1]=sz; Rz[2]=0;  Rz[3]=0;
                    Rz[4]=-sz;Rz[5]=cz; Rz[6]=0;  Rz[7]=0;
                    Rz[8]=0;  Rz[9]=0;  Rz[10]=1; Rz[11]=0;
                    Rz[12]=0; Rz[13]=0; Rz[14]=0; Rz[15]=1;
                    Sm[0]=inst.sx; Sm[1]=0;  Sm[2]=0;  Sm[3]=0;
                    Sm[4]=0;  Sm[5]=inst.sy; Sm[6]=0;  Sm[7]=0;
                    Sm[8]=0;  Sm[9]=0;  Sm[10]=inst.sz;Sm[11]=0;
                    Sm[12]=0; Sm[13]=0; Sm[14]=0; Sm[15]=1;
                    mat4Mul(tmp, Ry, Rx);
                    mat4Mul(RS, tmp, Rz);
                    mat4Mul(tmp, RS, Sm);
                    std::memcpy(RS, tmp, sizeof(RS));
                }

                float localMatrix[16];
                mat4Mul(localMatrix, T, RS);

                // normalMatrix = transpose(inverse(localMatrix3x3)). Uses
                // the same trick as renderGeoObjectGlsl: embed 3x3 in a 4x4,
                // invert, transpose the upper-3x3 back out.
                float normalMat[9] = { 1,0,0, 0,1,0, 0,0,1 };
                {
                    float m3[16] = {
                        localMatrix[0], localMatrix[1], localMatrix[2],  0,
                        localMatrix[4], localMatrix[5], localMatrix[6],  0,
                        localMatrix[8], localMatrix[9], localMatrix[10], 0,
                        0, 0, 0, 1
                    };
                    float inv[16];
                    if (mat4Invert(inv, m3)) {
                        normalMat[0] = inv[0];  normalMat[1] = inv[4];  normalMat[2] = inv[8];
                        normalMat[3] = inv[1];  normalMat[4] = inv[5];  normalMat[5] = inv[9];
                        normalMat[6] = inv[2];  normalMat[7] = inv[6];  normalMat[8] = inv[10];
                    }
                }

                // Previous-frame local matrix: translate the instance back by
                // its per-frame velocity vector. Rotation/scale assumed stable
                // frame-to-frame (the standard particle simplification — same
                // convention as kParticleVert's `in_pos - in_velocity`). This
                // gives the Velocity AOV a per-instance contribution on top of
                // the camera-motion contribution from u_prevProjView. Sub-frame
                // offset preserved so multi-sample renders see a consistent
                // velocity vector across the shutter.
                float prevLocalMatrix[16];
                std::memcpy(prevLocalMatrix, localMatrix, sizeof(prevLocalMatrix));
                prevLocalMatrix[12] = instPx - inst.vx;
                prevLocalMatrix[13] = instPy - inst.vy;
                prevLocalMatrix[14] = instPz - inst.vz;

                if (locLocal      >= 0) glUniformMatrix4fv(locLocal,     1, GL_FALSE, localMatrix);
                if (locPrevLocal  >= 0) glUniformMatrix4fv(locPrevLocal, 1, GL_FALSE, prevLocalMatrix);
                if (locNormal     >= 0) glUniformMatrix3fv(locNormal,    1, GL_FALSE, normalMat);
                if (locInstColor  >= 0) glUniform4f(locInstColor, inst.r, inst.g, inst.b, inst.a);
                if (locFadeOn     >= 0) glUniform1i(locFadeOn, fadeEnabled ? 1 : 0);
                if (locFadeCenterY>= 0) glUniform1f(locFadeCenterY, geo.bboxCenterY);
                if (locFadeHalfY  >= 0) glUniform1f(locFadeHalfY,   geo.bboxHalfY);

                glBindVertexArray(geo.vao);
                glDrawArrays(GL_TRIANGLES, 0, geo.numVerts);
            }

            glBindVertexArray(0);
            glUseProgram(0);

            // Release per-geo GL objects.
            for (int g = 0; g < 4; ++g) {
                if (geos[g].vbo) glDeleteBuffers(1, &geos[g].vbo);
                if (geos[g].vao) glDeleteVertexArrays(1, &geos[g].vao);
            }

            // Restore single-attachment draw buffer + unified blend for the
            // downstream volume pass (mirrors the particle-pass teardown).
            if (wantsAnyMrt) {
                GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
                glDrawBuffers(1, drawBufs);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }

            glDisable(GL_CULL_FACE);
            glDepthMask(GL_TRUE);
        }
    }

    // --- Render ReadVDB volume with ray marching shader ---
    if (readVdb) {
        ReadVDB::VDBVolumeData vdbData;
        if (readVdb->getVolumeData(args.time, vdbData)
            && vdbData.resX > 0 && vdbData.resY > 0 && vdbData.resZ > 0) {
            GLuint volTex = 0;
            glGenTextures(1, &volTex);
            glBindTexture(GL_TEXTURE_3D, volTex);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            // Non-cubic 3D texture matching the VDB's voxel aspect ratio.
            // GL_R32F + GL_RED replaces the deprecated GL_LUMINANCE — the
            // fragment shader still reads .r, so behaviour is identical.
            glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F,
                         vdbData.resX, vdbData.resY, vdbData.resZ, 0,
                         GL_RED, GL_FLOAT, vdbData.densityData.data());

            GLShaderPtr shader = std::make_shared<GLShader>();
            std::string shaderError;
            bool ok = shader->addShader(GLShader::eShaderTypeVertex, volumeVertexShader, &shaderError)
                   && shader->addShader(GLShader::eShaderTypeFragment, volumeFragmentShader, &shaderError)
                   && shader->link(&shaderError);

            if (ok) {
                // Apply ReadVDB transform to the bounding box
                double vtx, vty, vtz, vsx, vsy, vsz;
                readVdb->getTransform(args.time, vtx, vty, vtz, vsx, vsy, vsz);

                float x0 = vdbData.bboxMinX * (float)vsx + (float)vtx;
                float y0 = vdbData.bboxMinY * (float)vsy + (float)vty;
                float z0 = vdbData.bboxMinZ * (float)vsz + (float)vtz;
                float x1 = vdbData.bboxMaxX * (float)vsx + (float)vtx;
                float y1 = vdbData.bboxMaxY * (float)vsy + (float)vty;
                float z1 = vdbData.bboxMaxZ * (float)vsz + (float)vtz;

                // Step size relative to volume size — sized so we take about
                // one sample per voxel along the densest axis. Using the max
                // of (worldSize / resN) ratios keeps the step consistent
                // regardless of which axis dominates.
                float stepX = (x1 - x0) / (float)vdbData.resX;
                float stepY = (y1 - y0) / (float)vdbData.resY;
                float stepZ = (z1 - z0) / (float)vdbData.resZ;
                float stepSize = std::min({stepX, stepY, stepZ});
                if (stepSize <= 0.0f) stepSize = 0.02f;

                shader->bind();
                U32 progId = shader->getShaderID();
                glUniform3f(glGetUniformLocation(progId, "u_VolumeMin"), x0, y0, z0);
                glUniform3f(glGetUniformLocation(progId, "u_VolumeMax"), x1, y1, z1);
                glUniform3f(glGetUniformLocation(progId, "u_CameraPos"), (float)camTX, (float)camTY, (float)camTZ);
                glUniform1f(glGetUniformLocation(progId, "u_Density"), vdbData.density);
                glUniform3f(glGetUniformLocation(progId, "u_VolumeColor"), vdbData.colorR, vdbData.colorG, vdbData.colorB);
                glUniform1f(glGetUniformLocation(progId, "u_StepSize"), stepSize);
                glUniform1i(glGetUniformLocation(progId, "u_VolumeData"), 0);

                // Light uniforms
                if (light3d) {
                    double ltx, lty, ltz, lr, lg, lb, lint;
                    double lexp_unused;
                    light3d->getLightParams(args.time, ltx, lty, ltz, lr, lg, lb, lint, lexp_unused);
                    glUniform1i(glGetUniformLocation(progId, "u_LightEnabled"), 1);
                    glUniform3f(glGetUniformLocation(progId, "u_LightPos"), (float)ltx, (float)lty, (float)ltz);
                    glUniform3f(glGetUniformLocation(progId, "u_LightColor"), (float)lr, (float)lg, (float)lb);
                    glUniform1f(glGetUniformLocation(progId, "u_LightIntensity"), (float)lint);

                    // Get shadow params from light's knobs
                    KnobIPtr sdKnob = light3d->getKnobByName("shadowDensity");
                    KnobIPtr ssKnob = light3d->getKnobByName("shadowSteps");
                    float shadowDens = sdKnob ? (float)dynamic_cast<KnobDouble*>(sdKnob.get())->getValueAtTime(args.time) : 1.0f;
                    int shadowSteps = ssKnob ? dynamic_cast<KnobInt*>(ssKnob.get())->getValueAtTime(args.time) : 16;
                    glUniform1f(glGetUniformLocation(progId, "u_ShadowDensity"), shadowDens);
                    glUniform1i(glGetUniformLocation(progId, "u_ShadowSteps"), shadowSteps);
                } else {
                    glUniform1i(glGetUniformLocation(progId, "u_LightEnabled"), 0);
                }

                // Camera matrices upload — per-sample state, drives the
                // shader's u_modelView / u_projView.
                glUniformMatrix4fv(glGetUniformLocation(progId, "u_modelView"),
                                   1, GL_FALSE, viewMatrix);
                glUniformMatrix4fv(glGetUniformLocation(progId, "u_projView"),
                                   1, GL_FALSE, projViewMatrix);

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_3D, volTex);

                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_FRONT);

                drawVolumeProxyCube(x0, y0, z0, x1, y1, z1);

                glDisable(GL_CULL_FACE);
                glDepthMask(GL_TRUE);
                shader->unbind();
            }
            glDeleteTextures(1, &volTex);
        }
    }

    // --- Render procedural volume with ray marching shader ---
    if (volume3d) {
        Volume3D::VolumeParams vp = volume3d->getVolumeParams(args.time);

        // Generate 3D density data
        std::vector<float> volData;
        int volRes = 0;
        volume3d->generateVolumeData(args.time, volData, volRes);

        if (volRes > 0 && !volData.empty()) {
            // Upload 3D texture
            GLuint volTex = 0;
            glGenTextures(1, &volTex);
            glBindTexture(GL_TEXTURE_3D, volTex);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            // GL_R32F + GL_RED replaces GL_LUMINANCE (removed in core profile).
            glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, volRes, volRes, volRes, 0,
                         GL_RED, GL_FLOAT, volData.data());

            // Compile ray marching shader
            GLShaderPtr shader = std::make_shared<GLShader>();
            std::string shaderError;
            bool vsOk = shader->addShader(GLShader::eShaderTypeVertex, volumeVertexShader, &shaderError);
            bool fsOk = shader->addShader(GLShader::eShaderTypeFragment, volumeFragmentShader, &shaderError);
            bool linkOk = vsOk && fsOk && shader->link(&shaderError);

            if (linkOk) {
                // Volume bounding box in world space
                float halfX = vp.scaleX * 0.5f;
                float halfY = vp.scaleY * 0.5f;
                float halfZ = vp.scaleZ * 0.5f;

                float volMinX = vp.centerX - halfX;
                float volMinY = vp.centerY - halfY;
                float volMinZ = vp.centerZ - halfZ;
                float volMaxX = vp.centerX + halfX;
                float volMaxY = vp.centerY + halfY;
                float volMaxZ = vp.centerZ + halfZ;

                shader->bind();

                // Set uniforms
                U32 progId = shader->getShaderID();
                glUniform3f(glGetUniformLocation(progId, "u_VolumeMin"), volMinX, volMinY, volMinZ);
                glUniform3f(glGetUniformLocation(progId, "u_VolumeMax"), volMaxX, volMaxY, volMaxZ);
                glUniform3f(glGetUniformLocation(progId, "u_CameraPos"), (float)camTX, (float)camTY, (float)camTZ);
                glUniform1f(glGetUniformLocation(progId, "u_Density"), vp.density);
                glUniform3f(glGetUniformLocation(progId, "u_VolumeColor"), vp.colorR, vp.colorG, vp.colorB);
                glUniform1f(glGetUniformLocation(progId, "u_StepSize"), 0.02f);
                glUniform1i(glGetUniformLocation(progId, "u_VolumeData"), 0);

                // Light uniforms for procedural volume
                if (light3d) {
                    double ltx, lty, ltz, lr, lg, lb, lint;
                    double lexp_unused;
                    light3d->getLightParams(args.time, ltx, lty, ltz, lr, lg, lb, lint, lexp_unused);
                    glUniform1i(glGetUniformLocation(progId, "u_LightEnabled"), 1);
                    glUniform3f(glGetUniformLocation(progId, "u_LightPos"), (float)ltx, (float)lty, (float)ltz);
                    glUniform3f(glGetUniformLocation(progId, "u_LightColor"), (float)lr, (float)lg, (float)lb);
                    glUniform1f(glGetUniformLocation(progId, "u_LightIntensity"), (float)lint);
                    KnobIPtr sdKnob = light3d->getKnobByName("shadowDensity");
                    KnobIPtr ssKnob = light3d->getKnobByName("shadowSteps");
                    float shadowDens = sdKnob ? (float)dynamic_cast<KnobDouble*>(sdKnob.get())->getValueAtTime(args.time) : 1.0f;
                    int shadowSteps = ssKnob ? dynamic_cast<KnobInt*>(ssKnob.get())->getValueAtTime(args.time) : 16;
                    glUniform1f(glGetUniformLocation(progId, "u_ShadowDensity"), shadowDens);
                    glUniform1i(glGetUniformLocation(progId, "u_ShadowSteps"), shadowSteps);
                } else {
                    glUniform1i(glGetUniformLocation(progId, "u_LightEnabled"), 0);
                }

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_3D, volTex);

                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);

                // Camera matrices upload — per-sample state, drives the
                // shader's u_modelView / u_projView.
                glUniformMatrix4fv(glGetUniformLocation(progId, "u_modelView"),
                                   1, GL_FALSE, viewMatrix);
                glUniformMatrix4fv(glGetUniformLocation(progId, "u_projView"),
                                   1, GL_FALSE, projViewMatrix);

                // Draw proxy cube (back faces for correct ray entry when camera outside)
                glEnable(GL_CULL_FACE);
                glCullFace(GL_FRONT); // render back faces

                drawVolumeProxyCube(volMinX, volMinY, volMinZ,
                                    volMaxX, volMaxY, volMaxZ);

                glDisable(GL_CULL_FACE);
                glDepthMask(GL_TRUE);

                shader->unbind();
            }

            glDeleteTextures(1, &volTex);
        }
    }

    glDisable(GL_BLEND);

        // --- Resolve MSAA: blit multisampled FBO to single-sample FBO ---
        // Default read/draw buffer is attachment 0 (beauty). Blit it first.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(0, 0, outW, outH,
                          0, 0, outW, outH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Phase 3D — blit MRT attachments as separate passes (glBlitFramebuffer
        // only operates on the currently-bound read/draw buffer pair).
        if (wantsNormalMrt) {
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glDrawBuffer(GL_COLOR_ATTACHMENT1);
            glBlitFramebuffer(0, 0, outW, outH, 0, 0, outW, outH,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        if (wantsUvMrt) {
            glReadBuffer(GL_COLOR_ATTACHMENT2);
            glDrawBuffer(GL_COLOR_ATTACHMENT2);
            glBlitFramebuffer(0, 0, outW, outH, 0, 0, outW, outH,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        if (wantsPrefMrt) {
            glReadBuffer(GL_COLOR_ATTACHMENT3);
            glDrawBuffer(GL_COLOR_ATTACHMENT3);
            glBlitFramebuffer(0, 0, outW, outH, 0, 0, outW, outH,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        if (wantsVelocityMrt) {
            glReadBuffer(GL_COLOR_ATTACHMENT4);
            glDrawBuffer(GL_COLOR_ATTACHMENT4);
            glBlitFramebuffer(0, 0, outW, outH, 0, 0, outW, outH,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        // Bind resolve FBO for readback
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        // --- Read back this sample's pixels ---
        std::vector<float> samplePixels(outW * outH * 4);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, samplePixels.data());

        // Accumulate into the accumulator
        if (accumPixels.empty()) {
            accumPixels = samplePixels;
        } else {
            for (size_t i = 0; i < accumPixels.size(); ++i) {
                accumPixels[i] += samplePixels[i];
            }
        }

        // Phase 3D — read + accumulate per-AOV pixels.
        if (wantsNormalMrt) {
            std::vector<float> normalSamplePixels(outW * outH * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, normalSamplePixels.data());
            if (normalAccumPixels.empty()) normalAccumPixels = normalSamplePixels;
            else for (size_t i = 0; i < normalAccumPixels.size(); ++i)
                normalAccumPixels[i] += normalSamplePixels[i];
        }
        if (wantsUvMrt) {
            std::vector<float> uvSamplePixels(outW * outH * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT2);
            glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, uvSamplePixels.data());
            if (uvAccumPixels.empty()) uvAccumPixels = uvSamplePixels;
            else for (size_t i = 0; i < uvAccumPixels.size(); ++i)
                uvAccumPixels[i] += uvSamplePixels[i];
        }
        if (wantsPrefMrt) {
            std::vector<float> prefSamplePixels(outW * outH * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT3);
            glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, prefSamplePixels.data());
            if (prefAccumPixels.empty()) prefAccumPixels = prefSamplePixels;
            else for (size_t i = 0; i < prefAccumPixels.size(); ++i)
                prefAccumPixels[i] += prefSamplePixels[i];
        }
        if (wantsVelocityMrt) {
            std::vector<float> velocitySamplePixels(outW * outH * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT4);
            glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, velocitySamplePixels.data());
            if (velocityAccumPixels.empty()) velocityAccumPixels = velocitySamplePixels;
            else for (size_t i = 0; i < velocityAccumPixels.size(); ++i)
                velocityAccumPixels[i] += velocitySamplePixels[i];
        }
    } // === END MULTI-SAMPLE RENDER LOOP ===

    // Phase 3C — release the GLSL beauty program.
    if (glslBeautyProg) {
        glDeleteProgram(glslBeautyProg);
        glslBeautyProg = 0;
    }
    // Phase 3E — release the particle GLSL program.
    if (glslParticleProg) {
        glDeleteProgram(glslParticleProg);
        glslParticleProg = 0;
    }

    // Release the instance GLSL program.
    if (glslInstanceProg) {
        glDeleteProgram(glslInstanceProg);
        glslInstanceProg = 0;
    }

    // Restore frame-time view/proj matrices for post-loop work — the
    // WorldPos AOV reconstruction below needs the original matrices, not
    // the last motion-blur sample's.
    std::memcpy(viewMatrix,     frameViewMatrix,     16 * sizeof(float));
    std::memcpy(projMatrix,     frameProjMatrix,     16 * sizeof(float));
    std::memcpy(projViewMatrix, frameProjViewMatrix, 16 * sizeof(float));

    // (No particle-position restore needed: the motion-blur offsets are
    // applied to a private copy, never to the provider's shared snapshot.)

    // Average the accumulator to get final pixels
    std::vector<float> pixels;
    if (motionSamples > 1) {
        pixels = std::move(accumPixels);
        float invN = 1.0f / (float)motionSamples;
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] *= invN;
        }
    } else {
        pixels = std::move(accumPixels);
    }

    // Phase 3D — average each AOV accumulator the same way.
    auto avgAovAccum = [&](std::vector<float>& accum) -> std::vector<float> {
        std::vector<float> out = std::move(accum);
        if (motionSamples > 1 && !out.empty()) {
            float invN = 1.0f / (float)motionSamples;
            for (size_t i = 0; i < out.size(); ++i) out[i] *= invN;
        }
        return out;
    };
    std::vector<float> normalPixels   = wantsNormalMrt   ? avgAovAccum(normalAccumPixels)   : std::vector<float>();
    std::vector<float> uvPixels       = wantsUvMrt       ? avgAovAccum(uvAccumPixels)       : std::vector<float>();
    std::vector<float> prefPixels     = wantsPrefMrt     ? avgAovAccum(prefAccumPixels)     : std::vector<float>();
    std::vector<float> velocityPixels = wantsVelocityMrt ? avgAovAccum(velocityAccumPixels) : std::vector<float>();

    // --- Composite with background if connected ---
    // (bgImg was fetched up-front for the resolution override; reused here.)

    // --- Depth + World Position readback ---
    // Multi-sampled depth doesn't compose meaningfully (motion-blurred depth is
    // unphysical), so we just take depth from the final sample's resolve.
    // Acceptable for DOF / fog / re-projection use cases.
    const bool emitDepth = _imp->outputDepth.lock()    && _imp->outputDepth.lock()->getValue();
    const bool emitPos   = _imp->outputPosition.lock() && _imp->outputPosition.lock()->getValue();
    const bool emitAovs  = emitDepth || emitPos;

    std::vector<float> depthBuf;
    if (emitAovs) {
        // Blit MSAA depth → single-sample resolve FBO so we can glReadPixels it.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, outW, outH,
                          0, 0, outW, outH,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        depthBuf.resize((size_t)outW * (size_t)outH);
        glReadPixels(0, 0, outW, outH, GL_DEPTH_COMPONENT, GL_FLOAT, depthBuf.data());
    }

    // Inverse(projection * view) for world-position reconstruction.
    float invMVP[16] = {0};
    bool haveInvMVP = false;
    if (emitPos) {
        float mvp[16];
        mat4Mul(mvp, projMatrix, viewMatrix);
        haveInvMVP = mat4Invert(invMVP, mvp);
    }

    // --- Per-plane output: iterate args.outputPlanes (Color + optional AOVs) ---
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator pit = args.outputPlanes.begin();
         pit != args.outputPlanes.end(); ++pit) {
        const ImagePlaneDesc& planeDesc = pit->first;
        ImagePtr outPlaneImg = pit->second;
        if (!outPlaneImg) continue;
        const RectI outBounds = outPlaneImg->getBounds();
        const std::string& planeID = planeDesc.getPlaneID();

        if (planeDesc.isColorPlane()) {
            // Beauty plane — composite foreground over background image.
            Image::WriteAccess wa(outPlaneImg.get());
            Image::ReadAccess* bgRa = bgImg ? new Image::ReadAccess(bgImg.get()) : NULL;
            RectI bgBounds;
            if (bgImg) bgBounds = bgImg->getBounds();

            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;

                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;

                    float fgR = 0, fgG = 0, fgB = 0, fgA = 0;
                    if (fbX >= 0 && fbX < outW && fbY >= 0 && fbY < outH) {
                        int idx = (fbY * outW + fbX) * 4;
                        fgR = pixels[idx + 0];
                        fgG = pixels[idx + 1];
                        fgB = pixels[idx + 2];
                        fgA = pixels[idx + 3];
                    }

                    if (bgRa && bgBounds.contains(x, y)) {
                        const float* bgPix = (const float*)bgRa->pixelAt(x, y);
                        if (bgPix) {
                            float bgR = bgPix[0], bgG = bgPix[1], bgB = bgPix[2], bgA = bgPix[3];
                            dst[0] = fgR + bgR * (1.0f - fgA);
                            dst[1] = fgG + bgG * (1.0f - fgA);
                            dst[2] = fgB + bgB * (1.0f - fgA);
                            dst[3] = fgA + bgA * (1.0f - fgA);
                            continue;
                        }
                    }

                    dst[0] = fgR;
                    dst[1] = fgG;
                    dst[2] = fgB;
                    dst[3] = fgA;
                }
            }
            delete bgRa;
        } else if (planeID == "depth" && emitDepth && !depthBuf.empty()) {
            // Linear camera-space distance (world units). Background pixels
            // (depth==1.0) emit camFar so downstream nodes have a clean
            // "infinity" value.
            Image::WriteAccess wa(outPlaneImg.get());
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        dst[0] = camFar;
                        continue;
                    }
                    float d = depthBuf[(size_t)fbY * outW + fbX];
                    if (d >= 1.0f - 1e-6f) {
                        dst[0] = camFar;
                    } else if (projMode == 1) {
                        // Orthographic: depth buffer is linear in [near, far].
                        dst[0] = camNear + d * (camFar - camNear);
                    } else {
                        // Perspective: un-project the non-linear depth.
                        float zNdc = 2.0f * d - 1.0f;
                        dst[0] = (2.0f * camNear * camFar) /
                                 (camFar + camNear - zNdc * (camFar - camNear));
                    }
                }
            }
        } else if (planeID == "world_position" && emitPos && haveInvMVP && !depthBuf.empty()) {
            // Reconstruct world-space surface position via inverse(proj*view)
            // on NDC samples. Background pixels (depth==1.0) emit (0,0,0) since
            // no surface was hit.
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    float d = depthBuf[(size_t)fbY * outW + fbX];
                    if (d >= 1.0f - 1e-6f) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    float ndcX = 2.0f * ((float)fbX + 0.5f) / (float)outW - 1.0f;
                    float ndcY = 2.0f * ((float)fbY + 0.5f) / (float)outH - 1.0f;
                    float ndcZ = 2.0f * d - 1.0f;
                    float wX, wY, wZ, wW;
                    mat4Apply(invMVP, ndcX, ndcY, ndcZ, 1.0f, &wX, &wY, &wZ, &wW);
                    if (wW != 0.0f) {
                        float inv = 1.0f / wW;
                        if (outNumComp > 0) dst[0] = wX * inv;
                        if (outNumComp > 1) dst[1] = wY * inv;
                        if (outNumComp > 2) dst[2] = wZ * inv;
                    } else {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                    }
                }
            }
        } else if (planeID == "Normal" && wantsNormalMrt && !normalPixels.empty()) {
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    int idx = (fbY * outW + fbX) * 4;
                    if (outNumComp > 0) dst[0] = normalPixels[idx + 0];
                    if (outNumComp > 1) dst[1] = normalPixels[idx + 1];
                    if (outNumComp > 2) dst[2] = normalPixels[idx + 2];
                }
            }
        } else if (planeID == "uv" && wantsUvMrt && !uvPixels.empty()) {
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    int idx = (fbY * outW + fbX) * 4;
                    if (outNumComp > 0) dst[0] = uvPixels[idx + 0];
                    if (outNumComp > 1) dst[1] = uvPixels[idx + 1];
                    if (outNumComp > 2) dst[2] = 0.0f;
                }
            }
        } else if (planeID == "Pref" && wantsPrefMrt && !prefPixels.empty()) {
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    int idx = (fbY * outW + fbX) * 4;
                    if (outNumComp > 0) dst[0] = prefPixels[idx + 0];
                    if (outNumComp > 1) dst[1] = prefPixels[idx + 1];
                    if (outNumComp > 2) dst[2] = prefPixels[idx + 2];
                }
            }
        } else if (planeID == "Velocity" && wantsVelocityMrt && !velocityPixels.empty()) {
            Image::WriteAccess wa(outPlaneImg.get());
            const int outNumComp = outPlaneImg->getComponents().getNumComponents();
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;
                    int fbX = x - outBounds.x1;
                    int fbY = y - outBounds.y1;
                    if (fbX < 0 || fbX >= outW || fbY < 0 || fbY >= outH) {
                        if (outNumComp > 0) dst[0] = 0.0f;
                        if (outNumComp > 1) dst[1] = 0.0f;
                        if (outNumComp > 2) dst[2] = 0.0f;
                        continue;
                    }
                    int idx = (fbY * outW + fbX) * 4;
                    if (outNumComp > 0) dst[0] = velocityPixels[idx + 0];
                    if (outNumComp > 1) dst[1] = velocityPixels[idx + 1];
                    if (outNumComp > 2) dst[2] = 0.0f;
                }
            }
        }
    } // for each output plane

    // Cleanup
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &colorTex);
    glDeleteRenderbuffers(1, &depthRB);
    glDeleteFramebuffers(1, &msFBO);
    glDeleteRenderbuffers(1, &msColorRB);
    glDeleteRenderbuffers(1, &msDepthRB);
    // Phase 3D — release optional MRT resources.
    if (msNormalRB)         glDeleteRenderbuffers(1, &msNormalRB);
    if (normalResolveTex)   glDeleteTextures(1, &normalResolveTex);
    if (msUvRB)             glDeleteRenderbuffers(1, &msUvRB);
    if (uvResolveTex)       glDeleteTextures(1, &uvResolveTex);
    if (msPrefRB)           glDeleteRenderbuffers(1, &msPrefRB);
    if (prefResolveTex)     glDeleteTextures(1, &prefResolveTex);
    if (msVelocityRB)       glDeleteRenderbuffers(1, &msVelocityRB);
    if (velocityResolveTex) glDeleteTextures(1, &velocityResolveTex);

    OSGLContext::unsetCurrentContextNoRender();
    pool->releaseGLContextFromRender(glContext);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ScanlineRender.cpp"
