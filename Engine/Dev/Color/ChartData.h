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

#ifndef NATRON_ENGINE_CHARTDATA_H
#define NATRON_ENGINE_CHARTDATA_H

// Reference sRGB linear values for supported color charts.
// Data source: colour-science.org, via mmColorTarget v3.1.
// Layout: 24 patches, 4 rows x 6 columns, reading left-to-right, top-to-bottom.

namespace ChartData {

enum ChartType {
    eChartColorChecker24_Post2014 = 0,
    eChartColorChecker24_Pre2014,
    eChartColorCheckerPassportVideo,
    eChartSpyderCHECKR24,
    eChartCount
};

static const char* kChartLabels[] = {
    "ColorChecker24 (post-2014)",
    "ColorChecker24 (pre-2014)",
    "ColorChecker Passport Video",
    "SpyderCHECKR 24",
};

// Patch names for ColorChecker Classic 24
static const char* kColorChecker24Names[] = {
    "Dark Skin",       "Light Skin",      "Blue Sky",        "Foliage",
    "Blue Flower",     "Bluish Green",    "Orange",          "Purplish Blue",
    "Moderate Red",    "Purple",          "Yellow Green",    "Orange Yellow",
    "Blue",            "Green",           "Red",             "Yellow",
    "Magenta",         "Cyan",            "White",           "Neutral 8",
    "Neutral 6.5",     "Neutral 5",       "Neutral 3.5",    "Black",
};

// ColorChecker 24 — After November 2014 (current formulation)
// sRGB linear values
static const double kColorChecker24_Post2014[24][3] = {
    {0.17355167, 0.07874029, 0.05326058},
    {0.55946176, 0.27734355, 0.21194777},
    {0.10509124, 0.18955202, 0.32693865},
    {0.10506442, 0.15021316, 0.05221047},
    {0.22885963, 0.21350031, 0.42346758},
    {0.11449231, 0.50663347, 0.41229432},
    {0.74499115, 0.20172072, 0.0325174},
    {0.0606182,  0.10259253, 0.38373146},
    {0.56055825, 0.08072134, 0.11432307},
    {0.10983077, 0.04254067, 0.13682661},
    {0.32967574, 0.49495612, 0.04886544},
    {0.7689789,  0.35655545, 0.02534346},
    {0.0225082,  0.04870543, 0.28081679},
    {0.0444356,  0.29068277, 0.06458335},
    {0.44636923, 0.03676343, 0.0406788},
    {0.83803037, 0.57175305, 0.01273052},
    {0.52392518, 0.07924915, 0.28656418},
    {-0.04308491, 0.23415773, 0.37506175},
    {0.87919095, 0.88476747, 0.8349529},
    {0.58443959, 0.59212352, 0.58458201},
    {0.35767777, 0.36706043, 0.36528718},
    {0.19008669, 0.19086038, 0.1898278},
    {0.08593528, 0.08873843, 0.08978779},
    {0.03135966, 0.03149993, 0.03231098},
};

// ColorChecker 24 — Before November 2014 (ColorChecker2005)
static const double kColorChecker24_Pre2014[24][3] = {
    {0.17288193, 0.08207622, 0.0571305},
    {0.56787669, 0.29260883, 0.21954586},
    {0.1045436,  0.19649289, 0.32933007},
    {0.1009111,  0.14840476, 0.05322211},
    {0.22302011, 0.21697072, 0.43158497},
    {0.10716345, 0.51353085, 0.41389959},
    {0.74637688, 0.20029437, 0.03076252},
    {0.05952773, 0.10662997, 0.39879971},
    {0.56715445, 0.08488694, 0.11944154},
    {0.11168216, 0.0428821,  0.14156114},
    {0.34227562, 0.50630978, 0.05579511},
    {0.79245488, 0.35816549, 0.02547614},
    {0.01869478, 0.0514102,  0.28882529},
    {0.05430686, 0.2988491,  0.07183856},
    {0.45606939, 0.03079212, 0.04091886},
    {0.85365811, 0.56508406, 0.01484002},
    {0.53508484, 0.09013721, 0.30467937},
    {-0.03652398, 0.24752363, 0.39815654},
    {0.91234124, 0.91492078, 0.89403359},
    {0.57973065, 0.5920579,  0.59330907},
    {0.35485884, 0.36548524, 0.36754652},
    {0.19005098, 0.19188933, 0.19308043},
    {0.08530128, 0.08890396, 0.09252942},
    {0.03032485, 0.03114859, 0.03272795},
};

// ColorChecker Passport Video
static const double kColorCheckerPassportVideo[24][3] = {
    {0.55067428, 0.54452857, 0.09228552},
    {0.52628172, 0.10366784, 0.10548857},
    {0.48029787, 0.12375899, 0.47579495},
    {0.09265672, 0.10448422, 0.43698847},
    {0.10238247, 0.53279984, 0.54033706},
    {0.11431721, 0.48482393, 0.1109238},
    {0.08506253, 0.0546933,  0.04031908},
    {0.2385275,  0.11874789, 0.06697403},
    {0.44878259, 0.28122786, 0.19186507},
    {0.38105856, 0.21108463, 0.12677438},
    {0.52628844, 0.35366733, 0.26489983},
    {0.57410132, 0.40114107, 0.31328171},
    {0.04489626, 0.0439971,  0.04400712},
    {0.06181497, 0.06317388, 0.06497085},
    {0.14976565, 0.1486591,  0.14951377},
    {0.23358708, 0.23608815, 0.23484571},
    {0.40216211, 0.40468166, 0.41005089},
    {0.63990722, 0.64572307, 0.65176688},
    {0.0361024,  0.03645179, 0.03742664},
    {0.02880733, 0.02984399, 0.03046136},
    {0.00637911, 0.00593361, 0.00631497},
    {0.75149834, 0.7580864,  0.75697806},
    {0.83286324, 0.84068143, 0.83548108},
    {0.90184468, 0.89917418, 0.86433561},
};

// SpyderCHECKR 24
static const double kSpyderCHECKR24[24][3] = {
    {0.12352185, 0.50014024, 0.38620378},
    {0.21498245, 0.21030098, 0.42369405},
    {0.08478575, 0.14376125, 0.04632937},
    {0.09352333, 0.18762931, 0.32487815},
    {0.55887621, 0.28546917, 0.20831772},
    {0.16189762, 0.07239271, 0.04617433},
    {0.73185745, 0.1781343,  0.01647002},
    {0.04443235, 0.09769864, 0.34423744},
    {0.54388909, 0.07704525, 0.11343967},
    {0.08655038, 0.04242055, 0.14386053},
    {0.34668778, 0.50421467, 0.04336367},
    {0.84067668, 0.33835229, 0.01306082},
    {-0.05921422, 0.21435331, 0.34620743},
    {0.54098327, 0.07276986, 0.28129634},
    {0.89995247, 0.60091974, -0.00312428},
    {0.48971647, 0.01182625, 0.03266965},
    {0.04106236, 0.28928752, 0.05531369},
    {0.00868623, 0.03868002, 0.23982372},
    {0.95196627, 0.88859608, 0.86075173},
    {0.59764,    0.56945921, 0.55095416},
    {0.35862878, 0.34474285, 0.33166329},
    {0.18731169, 0.17975505, 0.17265435},
    {0.08058826, 0.07746429, 0.07365641},
    {0.02455127, 0.02221809, 0.02401148},
};

// Get reference data for a chart type
inline const double (*getChartData(ChartType chart))[3]
{
    switch (chart) {
    case eChartColorChecker24_Post2014:    return kColorChecker24_Post2014;
    case eChartColorChecker24_Pre2014:     return kColorChecker24_Pre2014;
    case eChartColorCheckerPassportVideo:   return kColorCheckerPassportVideo;
    case eChartSpyderCHECKR24:             return kSpyderCHECKR24;
    default:                                return kColorChecker24_Post2014;
    }
}

// ---- Colorspace Conversion ----

enum Colorspace {
    eColorspaceSRGB = 0,
    eColorspaceACEScg,
    eColorspaceACES2065_1,
    eColorspaceRec709,
    eColorspaceRec2020,
    eColorspaceP3_D65,
    eColorspaceARRI_WG3,
    eColorspaceREDWideGamut,
    eColorspaceDaVinciWideGamut,
    eColorspaceCount
};

static const char* kColorspaceLabels[] = {
    "sRGB",
    "ACEScg",
    "ACES2065-1",
    "Rec.709",
    "Rec.2020",
    "P3-D65",
    "ARRI Wide Gamut 3",
    "REDWideGamutRGB",
    "DaVinci Wide Gamut",
};

// sRGB linear → XYZ (D65)
static const double kSRGBtoXYZ[3][3] = {
    {0.4124, 0.3576, 0.1805},
    {0.2126, 0.7152, 0.0722},
    {0.0193, 0.1192, 0.9505},
};

// Bradford CAT: D65 → ACES whitepoint (for ACEScg and ACES2065-1)
static const double kCAT_D65_to_ACES[3][3] = {
    {1.0119593492787884, 0.008007966657797579, -0.01577937766233084},
    {0.005771078153540621, 1.0013620155134944, -0.006287243211140922},
    {-0.00033762208926845037, -0.0010466140323350855, 0.9275841364655992},
};

// XYZ → RGB matrices per colorspace (from colour-science.org)
static const double kXYZtoRGB[][3][3] = {
    // sRGB (same as Rec.709)
    {{3.240969941904523, -1.5373831775700941, -0.49861076029300355},
     {-0.9692436362808798, 1.8759675015077206, 0.04155505740717563},
     {0.05563007969699364, -0.20397695888897655, 1.0569715142428786}},
    // ACEScg
    {{1.6410233796943259, -0.32480329418479004, -0.2364246952376122},
     {-0.663662858722983, 1.615331591657338, 0.01675634768553014},
     {0.011721894328375374, -0.008284441996237409, 0.9883948585390215}},
    // ACES2065-1
    {{1.0498110175, 0.0, -9.74845e-05},
     {-0.4959030231, 1.3733130458, 0.0982400361},
     {0.0, 0.0, 0.9912520182}},
    // Rec.709 (same as sRGB)
    {{3.240969941904523, -1.5373831775700941, -0.49861076029300355},
     {-0.9692436362808798, 1.8759675015077206, 0.04155505740717563},
     {0.05563007969699364, -0.20397695888897655, 1.0569715142428786}},
    // Rec.2020
    {{1.716651187971268, -0.3556707837763925, -0.2533662813736599},
     {-0.6666843518324889, 1.6164812366349386, 0.015768545813911142},
     {0.017639857445310794, -0.04277061325780854, 0.9421031212354741}},
    // P3-D65
    {{2.4934969119414263, -0.9313836179191245, -0.4027107844507171},
     {-0.8294889695615748, 1.7626640603183463, 0.023624685841943605},
     {0.035845830243784474, -0.07617238926804183, 0.9568845240076874}},
    // ARRI Wide Gamut (ALEXA Wide Gamut)
    {{1.789066, -0.482534, -0.200076},
     {-0.639849, 1.3964, 0.194432},
     {-0.041532, 0.082335, 0.878868}},
    // REDWideGamutRGB
    {{1.412806612336158, -0.17752236616704917, -0.15177037638116067},
     {-0.48620318583506894, 1.290696210836872, 0.15740028369104153},
     {-0.0371387758049716, 0.2863757595579626, 0.6876796053100493}},
    // DaVinci Wide Gamut
    {{1.516672042024044, -0.2814780478789693, -0.14696363323677802},
     {-0.46491710123327584, 1.2514237756817124, 0.17488460886509075},
     {0.06484904706715999, 0.10913934371056985, 0.7614146215498789}},
};

// Does this colorspace need chromatic adaptation (non-D65 whitepoint)?
inline bool colorspaceNeedsCAT(Colorspace cs)
{
    return cs == eColorspaceACEScg || cs == eColorspaceACES2065_1;
}

// Convert a single sRGB linear RGB value to the target colorspace
inline void convertSRGBtoColorspace(double r, double g, double b,
                                     Colorspace cs,
                                     double& outR, double& outG, double& outB)
{
    if (cs == eColorspaceSRGB) {
        outR = r; outG = g; outB = b;
        return;
    }

    // Step 1: sRGB linear → XYZ (D65)
    double x = kSRGBtoXYZ[0][0]*r + kSRGBtoXYZ[0][1]*g + kSRGBtoXYZ[0][2]*b;
    double y = kSRGBtoXYZ[1][0]*r + kSRGBtoXYZ[1][1]*g + kSRGBtoXYZ[1][2]*b;
    double z = kSRGBtoXYZ[2][0]*r + kSRGBtoXYZ[2][1]*g + kSRGBtoXYZ[2][2]*b;

    // Step 2: Chromatic adaptation if needed (D65 → target whitepoint)
    if (colorspaceNeedsCAT(cs)) {
        double cx = kCAT_D65_to_ACES[0][0]*x + kCAT_D65_to_ACES[0][1]*y + kCAT_D65_to_ACES[0][2]*z;
        double cy = kCAT_D65_to_ACES[1][0]*x + kCAT_D65_to_ACES[1][1]*y + kCAT_D65_to_ACES[1][2]*z;
        double cz = kCAT_D65_to_ACES[2][0]*x + kCAT_D65_to_ACES[2][1]*y + kCAT_D65_to_ACES[2][2]*z;
        x = cx; y = cy; z = cz;
    }

    // Step 3: XYZ → target RGB
    int idx = (int)cs;
    outR = kXYZtoRGB[idx][0][0]*x + kXYZtoRGB[idx][0][1]*y + kXYZtoRGB[idx][0][2]*z;
    outG = kXYZtoRGB[idx][1][0]*x + kXYZtoRGB[idx][1][1]*y + kXYZtoRGB[idx][1][2]*z;
    outB = kXYZtoRGB[idx][2][0]*x + kXYZtoRGB[idx][2][1]*y + kXYZtoRGB[idx][2][2]*z;
}

} // namespace ChartData

#endif // NATRON_ENGINE_CHARTDATA_H
