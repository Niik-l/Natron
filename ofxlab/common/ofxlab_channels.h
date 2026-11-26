// STAND-IN written for the Natron fork's build of these plugins. The contributor's own
// common/ofxlab_channels.h was not in the zip; this is a guess at it from how the
// three plugins call it: a "channels" string knob ("rgb", "rgba", "all", "r g",
// "red,alpha", "none") turned into four on/off flags.
#ifndef OFXLAB_CHANNELS_STANDIN_H
#define OFXLAB_CHANNELS_STANDIN_H

#include <string>

namespace OfxLab {

/** Lower-case, with spaces and separators dropped: "Z_depth", "Z Down" -> "zdepth", "zdown". */
inline std::string cleanChannelTokens(const std::string &s)
{
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      out += c;
  }
  return out;
}

namespace detail {
inline bool applyToken(const std::string &t, bool sel[4])
{
  if (t.empty())
    return true;
  if (t == "all" || t == "rgba") {
    sel[0] = sel[1] = sel[2] = sel[3] = true;
    return true;
  }
  if (t == "none")
    return true;
  if (t == "red") { sel[0] = true; return true; }
  if (t == "green") { sel[1] = true; return true; }
  if (t == "blue") { sel[2] = true; return true; }
  if (t == "alpha") { sel[3] = true; return true; }
  // otherwise a run of single letters: "rgb", "ra", "ba"
  bool tmp[4] = {false, false, false, false};
  for (std::size_t i = 0; i < t.size(); ++i) {
    switch (t[i]) {
    case 'r': tmp[0] = true; break;
    case 'g': tmp[1] = true; break;
    case 'b': tmp[2] = true; break;
    case 'a': tmp[3] = true; break;
    default: return false;
    }
  }
  for (int c = 0; c < 4; ++c)
    sel[c] = sel[c] || tmp[c];
  return true;
}
} // namespace detail

/** Parse the knob into @p sel. An empty or unreadable knob takes @p def ("rgb", "all"). */
inline void parseChannels(const std::string &knob, bool sel[4], const char *def)
{
  for (int pass = 0; pass < 2; ++pass) {
    const std::string src = pass == 0 ? knob : std::string(def ? def : "all");
    sel[0] = sel[1] = sel[2] = sel[3] = false;
    bool ok = true, any = false;
    std::string tok;
    for (std::size_t i = 0; i <= src.size(); ++i) {
      char c = i < src.size() ? src[i] : ' ';
      if (c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
      if (c >= 'a' && c <= 'z') {
        tok += c;
      } else {
        if (!tok.empty()) {
          any = true;
          ok = detail::applyToken(tok, sel) && ok;
          tok.clear();
        }
      }
    }
    if (ok && any)
      return;
  }
}

/** Stand-in guess: apply an already cleaned token string ("rg", "rgba") onto @p sel. */
inline void parseChannelTokens(const std::string &tokens, bool sel[4])
{
  sel[0] = sel[1] = sel[2] = sel[3] = false;
  detail::applyToken(tokens, sel);
}

} // namespace OfxLab

#endif
