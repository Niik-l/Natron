// STAND-IN written for the Natron fork's build of these plugins. The contributor's own
// common/ofxlab_maskmix.h was not in the zip; this follows the semantics their
// comments state (D-20): t = mix * (invert ? 1-mask : mask), mask clamped to [0,1],
// out = src + t*(effect - src), t == 1 gives the effect bit-for-bit, pixels outside
// the mask image read 0, an unset reader is "fully on".
#ifndef OFXLAB_MASKMIX_STANDIN_H
#define OFXLAB_MASKMIX_STANDIN_H

#include "ofxsImageEffect.h"

namespace OfxLab {

inline double maskMix(double mix, double mask, bool invert)
{
  if (!(mask > 0.0))
    mask = 0.0; // also catches NaN
  else if (mask > 1.0)
    mask = 1.0;
  if (invert)
    mask = 1.0 - mask;
  double t = mix * mask;
  if (!(t > 0.0))
    return 0.0;
  return t > 1.0 ? 1.0 : t;
}

inline float blendOut(float src, float eff, double t)
{
  if (t >= 1.0)
    return eff;
  if (t <= 0.0)
    return src;
  return (float)((double)src + t * ((double)eff - (double)src));
}

class MaskReader {
public:
  MaskReader() : _img(0), _comp(0), _depth(OFX::eBitDepthFloat) {}

  void set(const OFX::Image *img, int comp)
  {
    _img = img;
    _comp = comp;
    if (_img)
      _depth = _img->getPixelDepth();
  }

  bool active() const { return _img != 0; }

  /** Mask value at an absolute pixel; 0 outside the mask image, 1 when no mask is set. */
  double valueAt(int x, int y) const
  {
    if (!_img)
      return 1.0;
    const void *p = _img->getPixelAddress(x, y);
    if (!p)
      return 0.0;
    switch (_depth) {
    case OFX::eBitDepthFloat:
      return (double)((const float *)p)[_comp];
    case OFX::eBitDepthUShort:
      return (double)((const unsigned short *)p)[_comp] / 65535.0;
    case OFX::eBitDepthUByte:
      return (double)((const unsigned char *)p)[_comp] / 255.0;
    default:
      return 0.0;
    }
  }

private:
  const OFX::Image *_img;
  int _comp;
  OFX::BitDepthEnum _depth;
};

} // namespace OfxLab

#endif
