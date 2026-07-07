// Copyright (c) 2011 libmv authors.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include "libmv/simple_pipeline/resect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "libmv/base/vector.h"
#include "libmv/logging/logging.h"
#include "libmv/multiview/euclidean_resection.h"
#include "libmv/multiview/resection.h"
#include "libmv/multiview/projection.h"
#include "libmv/numeric/numeric.h"
#include "libmv/numeric/levenberg_marquardt.h"
#include "libmv/simple_pipeline/reconstruction.h"
#include "libmv/simple_pipeline/tracks.h"

namespace libmv {
namespace {

Mat2X PointMatrixFromMarkers(const vector<Marker> &markers) {
  Mat2X points(2, markers.size());
  for (int i = 0; i < markers.size(); ++i) {
    points(0, i) = markers[i].x;
    points(1, i) = markers[i].y;
  }
  return points;
}

// Uses an incremental rotation:
//
//   x = R' * R * X + t;
//
// to avoid issues with the rotation representation. R' is derived from a
// euler vector encoding the rotation in 3 parameters; the direction is the
// axis to rotate around and the magnitude is the amount of the rotation.
struct EuclideanResectCostFunction {
 public:
  typedef Vec  FMatrixType;
  typedef Vec6 XMatrixType;

  EuclideanResectCostFunction(const vector<Marker> &markers,
                              const EuclideanReconstruction &reconstruction,
                              const Mat3 &initial_R)
    : markers(markers),
      reconstruction(reconstruction),
      initial_R(initial_R) {}

  // dRt has dR (delta R) encoded as a euler vector in the first 3 parameters,
  // followed by t in the next 3 parameters.
  Vec operator()(const Vec6 &dRt) const {
    // Unpack R, t from dRt.
    Mat3 R = RotationFromEulerVector(dRt.head<3>()) * initial_R;
    Vec3 t = dRt.tail<3>();

    // Compute the reprojection error for each coordinate.
    Vec residuals(2 * markers.size());
    residuals.setZero();
    for (int i = 0; i < markers.size(); ++i) {
      const EuclideanPoint &point =
          *reconstruction.PointForTrack(markers[i].track);
      Vec3 projected = R * point.X + t;
      projected /= projected(2);
      residuals[2*i + 0] = projected(0) - markers[i].x;
      residuals[2*i + 1] = projected(1) - markers[i].y;
    }
    return residuals;
  }

  const vector<Marker> &markers;
  const EuclideanReconstruction &reconstruction;
  const Mat3 &initial_R;
};

}  // namespace

namespace {

// Per-point reprojection residuals (normalized image units) for a pose.
// Points behind the camera get a huge residual so they never count as inliers.
void ComputeResectResiduals(const Mat3& R, const Vec3& t,
                            const Mat2X& points_2d, const Mat3X& points_3d,
                            std::vector<double>* res) {
  res->resize(points_2d.cols());
  for (int i = 0; i < points_2d.cols(); ++i) {
    Vec3 p = R * points_3d.col(i) + t;
    if (p(2) <= 1e-12) {
      (*res)[i] = 1e9;
      continue;
    }
    const double dx = p(0) / p(2) - points_2d(0, i);
    const double dy = p(1) / p(2) - points_2d(1, i);
    (*res)[i] = std::sqrt(dx * dx + dy * dy);
  }
}

}  // namespace

// Robust camera resection (2026-07 rework — closes the "RANSAC everywhere"
// gap of the classical pipeline): the stock version ran plain EPnP over ALL
// visible points with an L2 refine, so a handful of badly-triangulated 3D
// points could yank a frame's pose (observed as isolated pose spikes the
// bundle could not pull back out of the flat weak-parallax valley).
// Now: extreme-depth 3D outliers are pre-filtered, a full-set EPnP is accepted
// on a fast path when its residuals are already tight, otherwise RANSAC over
// minimal subsets picks the dominant-consensus pose; the LM polish runs on
// INLIERS only, and a cheirality gate rejects mirror poses.
bool EuclideanResect(const vector<Marker> &markers,
                     EuclideanReconstruction *reconstruction, bool final_pass) {
  (void)final_pass;
  if (markers.size() < 5) {
    return false;
  }

  // --- Pre-filter: drop 3D points at absurd depths (failed triangulations).
  // Markers are in calibrated/normalized units here; the 3D outliers we guard
  // against are the "point at 20000 units" kind that a near-nodal init makes.
  vector<Marker> use_markers;
  {
    Vec3 centroid = Vec3::Zero();
    for (int i = 0; i < markers.size(); ++i) {
      centroid += reconstruction->PointForTrack(markers[i].track)->X;
    }
    centroid /= (double)markers.size();
    std::vector<double> dists(markers.size());
    for (int i = 0; i < markers.size(); ++i) {
      dists[i] = (reconstruction->PointForTrack(markers[i].track)->X - centroid).norm();
    }
    std::vector<double> sorted = dists;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double medianDist = sorted[sorted.size() / 2];
    for (int i = 0; i < markers.size(); ++i) {
      if (medianDist <= 1e-12 || dists[i] <= 20.0 * medianDist) {
        use_markers.push_back(markers[i]);
      }
    }
  }
  if (use_markers.size() < 5) {
    use_markers = markers;   // filter was too aggressive — fall back to all
  }

  Mat2X points_2d = PointMatrixFromMarkers(use_markers);
  Mat3X points_3d(3, use_markers.size());
  for (int i = 0; i < use_markers.size(); i++) {
    points_3d.col(i) = reconstruction->PointForTrack(use_markers[i].track)->X;
  }

  const int n = (int)use_markers.size();
  // Inlier gate in normalized units: ~0.004 is ~8-10px at a typical focal —
  // a generous gate; the LM refine + global bundle polish afterwards.
  const double kInlierThr = 0.004;

  Mat3 R;
  Vec3 t;
  std::vector<char> inlierMask((size_t)n, 1);
  bool haveModel = false;

  // --- Fast path: EPnP on all points; accept if the consensus is near-total.
  if (euclidean_resection::EuclideanResection(points_2d, points_3d, &R, &t,
                                              euclidean_resection::RESECTION_EPNP)) {
    std::vector<double> res;
    ComputeResectResiduals(R, t, points_2d, points_3d, &res);
    int nIn = 0;
    for (int i = 0; i < n; ++i) if (res[i] < kInlierThr) ++nIn;
    if (nIn >= (int)(0.9 * n)) {
      for (int i = 0; i < n; ++i) inlierMask[i] = res[i] < kInlierThr;
      haveModel = true;
      LG << "Resect fast path: " << nIn << "/" << n << " inliers";
    }
  }

  // --- RANSAC over minimal subsets (deterministic LCG so runs reproduce).
  if (!haveModel) {
    unsigned rng = 12345u + (unsigned)n * 7919u + (unsigned)use_markers[0].image * 104729u;
    auto nextRand = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng; };
    const int kIters = 128;
    const int kSample = 5;
    int bestInliers = -1;
    Mat3 bestR;
    Vec3 bestT;
    Mat2X s2d(2, kSample);
    Mat3X s3d(3, kSample);
    for (int it = 0; it < kIters; ++it) {
      // sample without replacement
      int idx[kSample];
      for (int s = 0; s < kSample; ++s) {
        bool dup;
        do {
          idx[s] = (int)(nextRand() % (unsigned)n);
          dup = false;
          for (int q = 0; q < s; ++q) if (idx[q] == idx[s]) { dup = true; break; }
        } while (dup);
        s2d.col(s) = points_2d.col(idx[s]);
        s3d.col(s) = points_3d.col(idx[s]);
      }
      Mat3 Rh; Vec3 th;
      if (!euclidean_resection::EuclideanResection(s2d, s3d, &Rh, &th,
                                                   euclidean_resection::RESECTION_EPNP)) {
        continue;
      }
      std::vector<double> res;
      ComputeResectResiduals(Rh, th, points_2d, points_3d, &res);
      int nIn = 0;
      for (int i = 0; i < n; ++i) if (res[i] < kInlierThr) ++nIn;
      if (nIn > bestInliers) {
        bestInliers = nIn;
        bestR = Rh;
        bestT = th;
      }
    }
    if (bestInliers < std::max(5, (int)(0.4 * n))) {
      LG << "Robust resection failed for image " << use_markers[0].image
         << " (best consensus " << bestInliers << "/" << n << ")";
      return false;
    }
    // Re-estimate from the full consensus set for stability.
    std::vector<double> res;
    ComputeResectResiduals(bestR, bestT, points_2d, points_3d, &res);
    int nIn = 0;
    for (int i = 0; i < n; ++i) { inlierMask[i] = res[i] < kInlierThr; if (inlierMask[i]) ++nIn; }
    Mat2X in2d(2, nIn);
    Mat3X in3d(3, nIn);
    for (int i = 0, j = 0; i < n; ++i) {
      if (inlierMask[i]) { in2d.col(j) = points_2d.col(i); in3d.col(j) = points_3d.col(i); ++j; }
    }
    if (!euclidean_resection::EuclideanResection(in2d, in3d, &R, &t,
                                                 euclidean_resection::RESECTION_EPNP)) {
      R = bestR;
      t = bestT;
    }
    haveModel = true;
    LG << "Resect RANSAC: " << nIn << "/" << n << " inliers for image "
       << use_markers[0].image;
  }

  // --- LM refine on the INLIER markers only.
  vector<Marker> inlierMarkers;
  for (int i = 0; i < n; ++i) {
    if (inlierMask[i]) inlierMarkers.push_back(use_markers[i]);
  }

  typedef LevenbergMarquardt<EuclideanResectCostFunction> Solver;
  EuclideanResectCostFunction resect_cost(inlierMarkers, *reconstruction, R);
  Vec6 dRt = Vec6::Zero();
  dRt.tail<3>() = t;
  Solver solver(resect_cost);
  Solver::SolverParameters params;
  solver.minimize(params, &dRt);
  R = RotationFromEulerVector(dRt.head<3>()) * R;
  t = dRt.tail<3>();

  // --- Cheirality gate: a pose that puts a big chunk of its own inliers
  // behind the camera is a mirror/degenerate solution.
  {
    int behind = 0;
    for (size_t i = 0; i < inlierMarkers.size(); ++i) {
      Vec3 p = R * reconstruction->PointForTrack(inlierMarkers[i].track)->X + t;
      if (p(2) <= 0.0) ++behind;
    }
    if (behind > (int)(0.3 * inlierMarkers.size())) {
      LG << "Resection cheirality check failed for image " << use_markers[0].image
         << " (" << behind << "/" << inlierMarkers.size() << " behind camera)";
      return false;
    }
  }

  LG << "Resection for image " << use_markers[0].image << " got:\n"
     << "R:\n" << R << "\nt:\n" << t;
  reconstruction->InsertCamera(use_markers[0].image, R, t);
  return true;
}

namespace {

// Directly parameterize the projection matrix, P, which is a 12 parameter
// homogeneous entry. In theory P should be parameterized with only 11
// parametetrs, but in practice it works fine to let the extra degree of
// freedom drift.
struct ProjectiveResectCostFunction {
 public:
  typedef Vec  FMatrixType;
  typedef Vec12 XMatrixType;

  ProjectiveResectCostFunction(const vector<Marker> &markers,
                               const ProjectiveReconstruction &reconstruction)
    : markers(markers),
      reconstruction(reconstruction) {}

  Vec operator()(const Vec12 &vector_P) const {
    // Unpack P from vector_P.
    Map<const Mat34> P(vector_P.data(), 3, 4);

    // Compute the reprojection error for each coordinate.
    Vec residuals(2 * markers.size());
    residuals.setZero();
    for (int i = 0; i < markers.size(); ++i) {
      const ProjectivePoint &point =
          *reconstruction.PointForTrack(markers[i].track);
      Vec3 projected = P * point.X;
      projected /= projected(2);
      residuals[2*i + 0] = projected(0) - markers[i].x;
      residuals[2*i + 1] = projected(1) - markers[i].y;
    }
    return residuals;
  }

  const vector<Marker> &markers;
  const ProjectiveReconstruction &reconstruction;
};

}  // namespace

bool ProjectiveResect(const vector<Marker> &markers,
                      ProjectiveReconstruction *reconstruction) {
  if (markers.size() < 5) {
    return false;
  }

  // Stack the homogeneous 3D points as the columns of a matrix.
  Mat2X points_2d = PointMatrixFromMarkers(markers);
  Mat4X points_3d_homogeneous(4, markers.size());
  for (int i = 0; i < markers.size(); i++) {
    points_3d_homogeneous.col(i) =
        reconstruction->PointForTrack(markers[i].track)->X;
  }
  LG << "Points for resect:\n" << points_2d;

  // Resection the point.
  Mat34 P;
  resection::Resection(points_2d, points_3d_homogeneous, &P);

  // Flip the sign of P if necessary to keep the point in front of the camera.
  if ((P * points_3d_homogeneous.col(0))(2) < 0) {
    LG << "Point behind camera; switch sign.";
    P = -P;
  }

  // TODO(keir): Check if error is horrible and fail in that case.

  // Refine the resulting projection matrix using geometric error.
  typedef LevenbergMarquardt<ProjectiveResectCostFunction> Solver;

  ProjectiveResectCostFunction resect_cost(markers, *reconstruction);

  // Pack the initial P matrix into a size-12 vector..
  Vec12 vector_P = Map<Vec12>(P.data());

  Solver solver(resect_cost);

  Solver::SolverParameters params;
  /* Solver::Results results = */ solver.minimize(params, &vector_P);
  // TODO(keir): Check results to ensure clean termination.

  // Unpack the projection matrix.
  P = Map<Mat34>(vector_P.data(), 3, 4);

  LG << "Resection for image " << markers[0].image << " got:\n"
     << "P:\n" << P;
  reconstruction->InsertCamera(markers[0].image, P);
  return true;
}
}  // namespace libmv
