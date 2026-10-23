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

#ifndef NATRON_ENGINE_DEV_POSTFX_LENSTRACE_H
#define NATRON_ENGINE_DEV_POSTFX_LENSTRACE_H

/**
 * LensTrace: real camera lenses for DefocusCore.
 *
 * A lens prescription (spherical surfaces: radius, thickness, index, Abbe number, clear aperture;
 * one surface is the aperture stop) is ray traced sequentially, and the result is baked into a
 * LensTable: for each signed defocus, field position and colour channel, the bokeh kernel the core
 * draws, as
 *   - a 2x2 map from aperture-stop coordinates to sensor pixels, relative to the thin-lens blur
 *     radius (real blur size, astigmatic stretch, near/far orientation),
 *   - the clip discs that the other elements' rims cut out of the stop (mechanical vignetting:
 *     the "cat's eye"), in stop coordinates,
 *   - a shift of the kernel's centre (coma) and a radial brightness profile (spherical
 *     aberration: bright rim on one side of focus, soft centre on the other),
 * with each colour traced at its own wavelength (axial colour).
 *
 * The convex outline (stop shape intersected with the clip discs) is what the span scatter draws
 * exactly; the profile goes into its nested rings. Pure C++17, no Natron dependencies (the testbed
 * builds it too).
 *
 * Coordinates: mm, optical axis z, light travels +z (object on the left, sensor on the right).
 */

#include <functional>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace defocus {

struct LensSurface
{
    double radius = 0.;         // signed curvature radius (mm); 0 = flat; > 0 = centre to the right
    double thickness = 0.;      // distance to the next surface (mm)
    double ior = 1.;            // index (d line) of the medium after this surface; 1 = air
    double abbe = 0.;           // Abbe number of that medium (0 = estimated from the index)
    double semiAperture = 0.;   // clear radius (mm)
    bool stop = false;          // the aperture stop (a flat iris; radius ignored)
};

struct LensPrescription
{
    std::string id, name, source;   // source: patent / book citation
    std::vector<LensSurface> surfaces;
    bool abbeEstimated = false;
};

// Built-in lenses (from expired patents via W. J. Smith, Modern Lens Design; see LensTrace.cpp).
std::vector<std::string> builtinLensIds();
bool builtinLens(const std::string& id, LensPrescription* out);

// Lens file in the pbrt / Kolb format: one surface per line, "radius thickness index aperture"
// (mm; aperture = clear diameter; radius 0 = the stop; index 0 = air), '#' comments. An optional
// fifth column is the Abbe number. Returns false (and a message) if nothing usable was read.
bool parseLensFile(const std::string& text, LensPrescription* out, std::string* error);

// One bokeh kernel of the table. Stop coordinates: unit = stop radius, x = tangential (towards the
// frame edge), y = sagittal. "Relative" = divided by the thin-lens signed blur radius, so the core
// multiplies by the radius it computed from depth.
struct TracedKernel
{
    double a[4] = {1., 0., 0., 1.};   // relative 2x2 map (x', y') = a (x, y)
    double shift = 0.;                // relative centre offset along the tangential axis
    double centroid = 0.;             // centre of the clipped, profiled outline (stop coordinates, x)
    int nClip = 0;
    double clipX[4] = {0., 0., 0., 0.};   // clip discs in stop coordinates (centre on the x axis)
    double clipR[4] = {1., 1., 1., 1.};
    int clipRim[4] = {-1, -1, -1, -1};    // the surface each disc comes from (lookups blend by rim)
    double energy = 1.;               // light passed, relative to the unvignetted stop
    bool valid = false;               // traced (false: the fit failed, a plain disc)
    // Radial profile: density in kRings equal-width annuli of the outline, mean 1 over the area.
    static const int kRings = 8;
    double density[kRings] = {1., 1., 1., 1., 1., 1., 1., 1.};
    std::vector<double> ringScale, ringWeight;   // the same as nested copies of the outline
};

// A table lookup for the scatter, everything interpolated between the 4 nearest entries (no
// allocation per sample). ringScale / ringWeight point into the lookup itself.
struct KernelLookup
{
    double a[4];
    double shift;
    double centroid;
    double energy;
    int nClip;
    double clipX[4], clipR[4];
    int nRings;
    const double* ringScale;
    const double* ringWeight;
    double ringS[TracedKernel::kRings], ringW[TracedKernel::kRings];
};

struct LensTableParams
{
    double focalLengthMm = 50.;     // the prescription is scaled to this focal length
    double fNumber = 2.8;
    double focusDistanceMm = 5000.; // from the entrance pupil
    double fieldRadiusMm = 21.6;    // optical centre to the frame corner on the sensor
    double wavelengthNm[3] = {610., 550., 465.};
};

class LensTable
{
public:
    // Traces the lens; returns null (and a message) if it can't be focused or traced.
    static std::shared_ptr<LensTable> build(const LensPrescription& lens, const LensTableParams& p,
                                            std::string* error);

    // Kernel for a signed relative defocus u = (thin-lens radius) / (thin-lens radius at infinity),
    // normalised field radius rho (1 = frame corner) and channel (0 R, 1 G, 2 B). Interpolated in
    // u and rho except the profile, which comes from the nearest entry.
    void kernel(double u, double rho, int channel, TracedKernel* out) const;
    void lookup(double u, double rho, int channel, KernelLookup* out) const;

    // Reference PSF: traces a grid x grid bundle from the object point at (u, rho) and returns
    // where the rays that pass every aperture land on the sensor, relative to the chief ray (mm;
    // x tangential). False outside the image circle.
    bool tracePoint(double u, double rho, int channel, int grid, std::vector<double>* hx, std::vector<double>* hy) const;

    // Largest extent of any kernel relative to the thin-lens radius (padding, scatter windows).
    double maxScale() const { return _maxScale; }

    // Field maps at the focus distance, sampled uniformly over the normalised field radius
    // 0..mapRhoMax() (mapSize() samples), for the image stages:
    // - mapScale(c): where a point at field radius rho on the sensor comes from in the pinhole
    //   image (same focal length), as a ratio of radii, per channel. Holds the lens's distortion,
    //   its focus breathing (the ratio at the centre, != 1 when focused closer than infinity) and
    //   lateral colour (red / blue against green).
    // - mapIllum(): relative illumination (natural vignetting: the light that passes every
    //   aperture, over the image area it lands on), 1 at the centre.
    // Empty if the lens could not be traced across the field.
    int mapSize() const { return (int)_mapIllum.size(); }
    double mapRhoMax() const { return _mapRhoMax; }
    const std::vector<double>& mapScale(int channel) const { return _mapScale[std::max(0, std::min(2, channel))]; }
    const std::vector<double>& mapIllum() const { return _mapIllum; }

    // Facts about the traced lens, for the UI.
    double effectiveFNumber() const { return _fNumber; }     // clamped to the lens's widest stop
    double widestFNumber() const { return _widestF; }
    double backFocusMm() const { return _bfd; }
    bool chromatic() const { return _chromatic; }
    const LensTableParams& params() const { return _p; }
    // Table entries that could not be traced (they fall back to a thin-lens disc).
    int failedEntries() const { return _failChief + _failSample + _failFit; }
    int failedChief() const { return _failChief; }
    int failedSample() const { return _failSample; }
    int failedFit() const { return _failFit; }

private:
    LensTableParams _p;
    double _fNumber = 2.8, _widestF = 1., _bfd = 0.;
    bool _chromatic = false;
    int _failChief = 0, _failSample = 0, _failFit = 0;
    double _maxScale = 1.;
    std::function<int(int, double, double, int, void*)> _sampler;   // see LensTable::build   // counted from worker threads: diagnostic only
    std::vector<double> _u, _rho;
    std::vector<TracedKernel> _k;   // [channel][u][rho]
    double _mapRhoMax = 1.;
    std::vector<double> _mapScale[3], _mapIllum;
    const TracedKernel& at(int ch, int iu, int ir) const { return _k[((size_t)ch * _u.size() + iu) * _rho.size() + ir]; }
    friend struct LensTableBuilder;
};

} // namespace defocus

#endif // NATRON_ENGINE_DEV_POSTFX_LENSTRACE_H
