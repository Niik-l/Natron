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

#include "GeoBuilder.h"
#include "SceneGraph.h"
#include "../DotUtils.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "../../../Global/GLIncludes.h"

#include "../../AppManager.h"
#include "../../ChoiceOption.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../ViewIdx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

NATRON_NAMESPACE_ENTER

// ---------------------------------------------------------------------------
// Private state
// ---------------------------------------------------------------------------

struct GeoBuilderPrivate
{
    // 2D grids (the node's state) + their persistent home
    std::vector<GeoBuilder::Grid> grids;
    KnobStringWPtr gridsData;       // hidden, persistent: serialised grid list
    KnobChoiceWPtr gridChoice;      // which grid the Rows/Columns knobs edit
    KnobButtonWPtr addGrid, deleteGrid;
    KnobIntWPtr gridRows, gridCols;
    KnobStringWPtr gridStatus;      // label: what to do next / what is selected
    int gridCounter = 0;
    // Overlay interaction
    int placingCount = -1;          // -1: not placing; 0..3: corners clicked so far
    double placingX[4] = {0, 0, 0, 0};
    double placingY[4] = {0, 0, 0, 0};
    int dragGrid = -1;
    int dragCorner = -1;

    // Shape list (the node's state) + its persistent home
    std::vector<GeoBuilder::Shape> shapes;
    KnobStringWPtr shapesData;      // hidden, persistent: serialised shape list
    KnobChoiceWPtr shapeChoice;     // which shape the "Shape" knobs edit

    // Creation buttons
    KnobButtonWPtr addCard, addCube, addSphere, addCylinder;
    KnobButtonWPtr duplicateShape, deleteShape;

    // Selected-shape knobs (not persistent: the list is the truth)
    KnobStringWPtr shapeName;
    KnobBoolWPtr shapeVisible;
    KnobIntWPtr shapeRows, shapeCols;
    KnobDoubleWPtr shapeSize, shapeHeight;
    KnobDoubleWPtr shapeTX, shapeTY, shapeTZ;
    KnobDoubleWPtr shapeRX, shapeRY, shapeRZ;
    KnobDoubleWPtr shapeSX, shapeSY, shapeSZ;
    KnobStringWPtr info;

    // Node-level transform (read by name by every consumer)
    KnobDoubleWPtr translateX, translateY, translateZ;
    KnobDoubleWPtr rotateX, rotateY, rotateZ;
    KnobDoubleWPtr scaleX, scaleY, scaleZ;
    KnobDoubleWPtr uniformScale;

    // Material
    KnobColorWPtr baseColor;
    KnobDoubleWPtr roughness, metallic, specular;
    KnobColorWPtr emissionColor;
    KnobDoubleWPtr emissionStrength;
    KnobDoubleWPtr transmission, ior;
    KnobFileWPtr textureFile;
    KnobChoiceWPtr diffuseColorspace;

    // Re-entrancy guard: syncing knobs from the list must not write back
    bool syncing = false;
    int shapeCounter[4] = {0, 0, 0, 0};   // for default names
};

// ---------------------------------------------------------------------------
// Construction / plugin description
// ---------------------------------------------------------------------------

GeoBuilder::GeoBuilder(NodePtr node)
    : EffectInstance(node)
    , _imp(new GeoBuilderPrivate())
{
}

GeoBuilder::~GeoBuilder()
{
}

std::string
GeoBuilder::getPluginDescription() const
{
    return tr(
        "Build simple geometry for a shot from primitive shapes (card, cube, "
        "sphere, cylinder), Natron's take on Nuke's ModelBuilder.\n\n"
        "Add shapes with the buttons, pick one in the Shape dropdown and edit "
        "its parameters and transform; the Transform page moves the whole "
        "model. The result renders like a ReadGeo: connect it to a Scene3D "
        "for the 3D viewport, ScanlineRender and Cycles.\n\n"
        "Inputs: mat (material override), cam / src / geo are reserved for the "
        "upcoming alignment tools and are not used yet."
    ).toStdString();
}

std::string
GeoBuilder::getInputLabel(int inputNb) const
{
    switch (inputNb) {
        case 0: return "mat";
        case 1: return "cam";
        case 2: return "src";
        case 3: return "geo";
    }
    return std::string();
}

void
GeoBuilder::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
}

void
GeoBuilder::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
GeoBuilder::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

static KnobDoublePtr
makeDouble(GeoBuilder* self, KnobPagePtr page, const QString& label, const char* name,
           double def, double dispMin, double dispMax, bool persistent, double hardMin = -1e30)
{
    KnobDoublePtr k = AppManager::createKnob<KnobDouble>(self, label);
    k->setName(name);
    k->setDefaultValue(def);
    k->setAnimationEnabled(persistent);
    if (hardMin > -1e29) k->setMinimum(hardMin);
    k->setDisplayMinimum(dispMin);
    k->setDisplayMaximum(dispMax);
    if (!persistent) k->setIsPersistent(false);
    page->addKnob(k);
    return k;
}

void
GeoBuilder::initializeKnobs()
{
    // ---------------- Grid page (2D, drawn in the viewer) ----------------
    KnobPagePtr gridPage = AppManager::createKnob<KnobPage>(this, tr("Grid"));
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Add Grid"));
        k->setName("addGrid");
        k->setHintToolTip(tr("Then click four corners over the plate in the 2D viewer, going "
                             "around the shape. The grid appears after the fourth click and its "
                             "corners stay draggable."));
        gridPage->addKnob(k); _imp->addGrid = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Status"));
        k->setName("gridStatus"); k->setAsLabel(); k->setIsPersistent(false); k->setEvaluateOnChange(false);
        gridPage->addKnob(k); _imp->gridStatus = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Grid"));
        k->setName("grid");
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("The grid the Rows / Columns knobs edit (also selected by clicking a corner in the viewer)."));
        gridPage->addKnob(k); _imp->gridChoice = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Delete Grid"));
        k->setName("deleteGrid"); k->setAddNewLine(false);
        gridPage->addKnob(k); _imp->deleteGrid = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Rows"));
        k->setName("gridRows"); k->setDefaultValue(4); k->setMinimum(1); k->setMaximum(256);
        k->setDisplayMinimum(1); k->setDisplayMaximum(32); k->setIsPersistent(false); k->setEvaluateOnChange(false);
        k->setAddNewLine(false);
        gridPage->addKnob(k); _imp->gridRows = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Columns"));
        k->setName("gridCols"); k->setDefaultValue(4); k->setMinimum(1); k->setMaximum(256);
        k->setDisplayMinimum(1); k->setDisplayMaximum(32); k->setIsPersistent(false); k->setEvaluateOnChange(false);
        gridPage->addKnob(k); _imp->gridCols = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Grids Data"));
        k->setName("gridsData");
        k->setAsMultiLine();
        k->setSecret(true);
        k->setEvaluateOnChange(false);   // 2D only for now: no render depends on it
        gridPage->addKnob(k); _imp->gridsData = k;
    }

    // ---------------- Shapes page ----------------
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Shapes"));

    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Add Card"));
        k->setName("addCard");
        k->setHintToolTip(tr("Add a flat card (rows x cols quads, side = Size)."));
        page->addKnob(k); _imp->addCard = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Add Cube"));
        k->setName("addCube"); k->setAddNewLine(false);
        k->setHintToolTip(tr("Add a cube (edge = Size)."));
        page->addKnob(k); _imp->addCube = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Add Sphere"));
        k->setName("addSphere"); k->setAddNewLine(false);
        k->setHintToolTip(tr("Add a lat/long sphere (rows x cols, radius = Size)."));
        page->addKnob(k); _imp->addSphere = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Add Cylinder"));
        k->setName("addCylinder"); k->setAddNewLine(false);
        k->setHintToolTip(tr("Add a capped cylinder (rows along the height, cols around, radius = Size)."));
        page->addKnob(k); _imp->addCylinder = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Shape"));
        k->setName("shape");
        k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("The shape the controls below edit."));
        page->addKnob(k); _imp->shapeChoice = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Duplicate"));
        k->setName("duplicateShape"); k->setAddNewLine(false);
        page->addKnob(k); _imp->duplicateShape = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Delete"));
        k->setName("deleteShape"); k->setAddNewLine(false);
        page->addKnob(k); _imp->deleteShape = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Name"));
        k->setName("shapeName"); k->setIsPersistent(false);
        page->addKnob(k); _imp->shapeName = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Visible"));
        k->setName("shapeVisible"); k->setDefaultValue(true); k->setIsPersistent(false);
        page->addKnob(k); _imp->shapeVisible = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Rows"));
        k->setName("shapeRows"); k->setDefaultValue(1); k->setMinimum(1); k->setMaximum(256);
        k->setDisplayMinimum(1); k->setDisplayMaximum(64); k->setIsPersistent(false);
        k->setAddNewLine(false);
        page->addKnob(k); _imp->shapeRows = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Columns"));
        k->setName("shapeCols"); k->setDefaultValue(1); k->setMinimum(1); k->setMaximum(256);
        k->setDisplayMinimum(1); k->setDisplayMaximum(64); k->setIsPersistent(false);
        page->addKnob(k); _imp->shapeCols = k;
    }
    _imp->shapeSize = makeDouble(this, page, tr("Size"), "shapeSize", 1.0, 0.01, 10.0, false, 0.0001);
    _imp->shapeSize.lock()->setHintToolTip(tr("Card side, cube edge, sphere radius, cylinder radius."));
    _imp->shapeHeight = makeDouble(this, page, tr("Height"), "shapeHeight", 2.0, 0.01, 10.0, false, 0.0001);
    _imp->shapeHeight.lock()->setHintToolTip(tr("Cylinder height (ignored by the other shapes)."));

    _imp->shapeTX = makeDouble(this, page, tr("Shape Translate X"), "shapeTranslateX", 0.0, -100.0, 100.0, false);
    _imp->shapeTY = makeDouble(this, page, tr("Shape Translate Y"), "shapeTranslateY", 0.0, -100.0, 100.0, false);
    _imp->shapeTZ = makeDouble(this, page, tr("Shape Translate Z"), "shapeTranslateZ", 0.0, -100.0, 100.0, false);
    _imp->shapeRX = makeDouble(this, page, tr("Shape Rotate X"), "shapeRotateX", 0.0, -180.0, 180.0, false);
    _imp->shapeRY = makeDouble(this, page, tr("Shape Rotate Y"), "shapeRotateY", 0.0, -180.0, 180.0, false);
    _imp->shapeRZ = makeDouble(this, page, tr("Shape Rotate Z"), "shapeRotateZ", 0.0, -180.0, 180.0, false);
    _imp->shapeSX = makeDouble(this, page, tr("Shape Scale X"), "shapeScaleX", 1.0, 0.1, 10.0, false, 0.0001);
    _imp->shapeSY = makeDouble(this, page, tr("Shape Scale Y"), "shapeScaleY", 1.0, 0.1, 10.0, false, 0.0001);
    _imp->shapeSZ = makeDouble(this, page, tr("Shape Scale Z"), "shapeScaleZ", 1.0, 0.1, 10.0, false, 0.0001);

    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info"); k->setAsLabel(); k->setIsPersistent(false); k->setEvaluateOnChange(false);
        page->addKnob(k); _imp->info = k;
    }
    {
        // The state. Hidden; one line per shape (see saveShapesToKnob).
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Shapes Data"));
        k->setName("shapesData");
        k->setAsMultiLine();
        k->setSecret(true);
        page->addKnob(k); _imp->shapesData = k;
    }

    // ---------------- Transform page (whole model) ----------------
    KnobPagePtr xformPage = AppManager::createKnob<KnobPage>(this, tr("Transform"));
    _imp->translateX = makeDouble(this, xformPage, tr("Translate X"), "translateX", 0.0, -100.0, 100.0, true);
    _imp->translateY = makeDouble(this, xformPage, tr("Translate Y"), "translateY", 0.0, -100.0, 100.0, true);
    _imp->translateZ = makeDouble(this, xformPage, tr("Translate Z"), "translateZ", 0.0, -100.0, 100.0, true);
    _imp->rotateX = makeDouble(this, xformPage, tr("Rotate X"), "rotateX", 0.0, -180.0, 180.0, true);
    _imp->rotateY = makeDouble(this, xformPage, tr("Rotate Y"), "rotateY", 0.0, -180.0, 180.0, true);
    _imp->rotateZ = makeDouble(this, xformPage, tr("Rotate Z"), "rotateZ", 0.0, -180.0, 180.0, true);
    _imp->scaleX = makeDouble(this, xformPage, tr("Scale X"), "scaleX", 1.0, 0.1, 10.0, true, 0.01);
    _imp->scaleY = makeDouble(this, xformPage, tr("Scale Y"), "scaleY", 1.0, 0.1, 10.0, true, 0.01);
    _imp->scaleZ = makeDouble(this, xformPage, tr("Scale Z"), "scaleZ", 1.0, 0.1, 10.0, true, 0.01);
    _imp->uniformScale = makeDouble(this, xformPage, tr("Uniform Scale"), "uniformScale", 1.0, 0.001, 10.0, true, 0.0001);
    _imp->uniformScale.lock()->setHintToolTip(tr("Multiplies Scale X/Y/Z, so the whole model resizes from one knob."));

    // ---------------- Material page (same as ReadGeo) ----------------
    KnobPagePtr matPage = AppManager::createKnob<KnobPage>(this, tr("Material"));
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Base Color"), 3);
        k->setName("baseColor");
        k->setDefaultValue(0.8, 0); k->setDefaultValue(0.8, 1); k->setDefaultValue(0.8, 2);
        k->setAnimationEnabled(true);
        matPage->addKnob(k); _imp->baseColor = k;
    }
    _imp->roughness = makeDouble(this, matPage, tr("Roughness"), "roughness", 0.5, 0.0, 1.0, true, 0.0);
    _imp->roughness.lock()->setMaximum(1.0);
    _imp->metallic = makeDouble(this, matPage, tr("Metallic"), "metallic", 0.0, 0.0, 1.0, true, 0.0);
    _imp->metallic.lock()->setMaximum(1.0);
    _imp->specular = makeDouble(this, matPage, tr("Specular"), "specular", 0.5, 0.0, 1.0, true, 0.0);
    _imp->specular.lock()->setMaximum(1.0);
    {
        KnobColorPtr k = AppManager::createKnob<KnobColor>(this, tr("Emission Color"), 3);
        k->setName("emissionColor");
        k->setDefaultValue(1.0, 0); k->setDefaultValue(1.0, 1); k->setDefaultValue(1.0, 2);
        matPage->addKnob(k); _imp->emissionColor = k;
    }
    _imp->emissionStrength = makeDouble(this, matPage, tr("Emission Strength"), "emissionStrength", 0.0, 0.0, 10.0, true, 0.0);
    _imp->transmission = makeDouble(this, matPage, tr("Transmission"), "transmission", 0.0, 0.0, 1.0, true, 0.0);
    _imp->transmission.lock()->setMaximum(1.0);
    _imp->ior = makeDouble(this, matPage, tr("IOR"), "ior", 1.45, 1.0, 2.5, true, 1.0);

    KnobPagePtr texPage = AppManager::createKnob<KnobPage>(this, tr("Texture Maps"));
    {
        KnobFilePtr k = AppManager::createKnob<KnobFile>(this, tr("Diffuse Map"));
        k->setName("textureFile");
        k->setHintToolTip(tr("Base color / albedo texture. Supports .exr, .hdr, .png, .jpg"));
        texPage->addKnob(k); _imp->textureFile = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Diffuse Colorspace"));
        k->setName("diffuseColorspace");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("sRGB", "", "sRGB gamma-encoded (PNG, JPEG)"));
        entries.push_back(ChoiceOption("Linear", "", "Linear / scene-referred (EXR, HDR)"));
        entries.push_back(ChoiceOption("ACEScg", "", "ACEScg (AP1 linear, ACES pipeline)"));
        entries.push_back(ChoiceOption("Raw", "", "Raw data, no conversion"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        texPage->addKnob(k); _imp->diffuseColorspace = k;
    }

    refreshShapeChoice();
    loadSelectedShapeIntoKnobs();
    rebuildMesh();
    refreshGridChoice();
    loadSelectedGridIntoKnobs();
    setGridStatus("Add Grid, then click four corners in the viewer.");
}

// ---------------------------------------------------------------------------
// Grids: list <-> hidden knob, selection <-> knobs
// ---------------------------------------------------------------------------

void
GeoBuilder::saveGridsToKnob()
{
    std::ostringstream ss;
    for (size_t i = 0; i < _imp->grids.size(); ++i) {
        const Grid& g = _imp->grids[i];
        std::string nm = g.name;
        for (size_t c = 0; c < nm.size(); ++c) { if (nm[c] == '|' || nm[c] == '\n' || nm[c] == '\r') nm[c] = ' '; }
        ss << nm << '|' << g.rows << '|' << g.cols;
        for (int c = 0; c < 4; ++c) ss << '|' << g.x[c] << '|' << g.y[c];
        ss << '\n';
    }
    KnobStringPtr k = _imp->gridsData.lock();
    if (!k) return;
    _imp->syncing = true;
    k->setValue(ss.str());
    _imp->syncing = false;
}

void
GeoBuilder::loadGridsFromKnob()
{
    _imp->grids.clear();
    _imp->gridCounter = 0;
    KnobStringPtr k = _imp->gridsData.lock();
    if (!k) return;
    std::istringstream in(k->getValue());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        std::string cur;
        for (size_t c = 0; c < line.size(); ++c) {
            if (line[c] == '|') { f.push_back(cur); cur.clear(); } else cur.push_back(line[c]);
        }
        f.push_back(cur);
        if (f.size() < 11) continue;
        Grid g;
        g.name = f[0];
        g.rows = std::max(1, std::atoi(f[1].c_str()));
        g.cols = std::max(1, std::atoi(f[2].c_str()));
        for (int c = 0; c < 4; ++c) { g.x[c] = std::atof(f[3 + c * 2].c_str()); g.y[c] = std::atof(f[4 + c * 2].c_str()); }
        _imp->grids.push_back(g);
        ++_imp->gridCounter;
    }
}

std::vector<GeoBuilder::Grid>
GeoBuilder::getGrids() const
{
    return _imp->grids;
}

int
GeoBuilder::getSelectedGridIndex() const
{
    KnobChoicePtr c = _imp->gridChoice.lock();
    if (!c || _imp->grids.empty()) return -1;
    const int i = c->getValue();
    return (i >= 0 && i < (int)_imp->grids.size()) ? i : -1;
}

void
GeoBuilder::refreshGridChoice()
{
    KnobChoicePtr c = _imp->gridChoice.lock();
    if (!c) return;
    std::vector<ChoiceOption> entries;
    for (size_t i = 0; i < _imp->grids.size(); ++i) entries.push_back(ChoiceOption(_imp->grids[i].name, "", ""));
    _imp->syncing = true;
    c->populateChoices(entries);
    _imp->syncing = false;
}

void
GeoBuilder::loadSelectedGridIntoKnobs()
{
    const int i = getSelectedGridIndex();
    const bool has = (i >= 0);
    _imp->syncing = true;
    KnobIntPtr rows = _imp->gridRows.lock();
    if (rows) { rows->setValue(has ? _imp->grids[i].rows : 4); rows->setEnabled(0, has); }
    KnobIntPtr cols = _imp->gridCols.lock();
    if (cols) { cols->setValue(has ? _imp->grids[i].cols : 4); cols->setEnabled(0, has); }
    KnobButtonPtr del = _imp->deleteGrid.lock();
    if (del) del->setEnabled(0, has);
    _imp->syncing = false;
}

void
GeoBuilder::storeKnobsIntoSelectedGrid()
{
    const int i = getSelectedGridIndex();
    if (i < 0) return;
    KnobIntPtr rows = _imp->gridRows.lock();
    if (rows) _imp->grids[i].rows = std::max(1, rows->getValue());
    KnobIntPtr cols = _imp->gridCols.lock();
    if (cols) _imp->grids[i].cols = std::max(1, cols->getValue());
}

void
GeoBuilder::setGridStatus(const std::string& text)
{
    KnobStringPtr k = _imp->gridStatus.lock();
    if (!k) return;
    _imp->syncing = true;
    k->setValue(text);
    _imp->syncing = false;
}

// ---------------------------------------------------------------------------
// 2D pass-through of the src input
// ---------------------------------------------------------------------------

bool
GeoBuilder::isIdentity(double time, const RenderScale& /*scale*/, const RectI& /*roi*/, ViewIdx view,
                       double* inputTime, ViewIdx* inputView, int* inputNb)
{
    if (getInput(2)) {
        *inputTime = time;
        *inputView = view;
        *inputNb = 2;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Overlay: draw the grids over the viewer image
// ---------------------------------------------------------------------------

static inline void lerp2(double ax, double ay, double bx, double by, double t, double& ox, double& oy)
{
    ox = ax + (bx - ax) * t;
    oy = ay + (by - ay) * t;
}

void
GeoBuilder::drawOverlay(double /*time*/, const RenderScale& /*renderScale*/, ViewIdx /*view*/)
{
    GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_POINT_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);

    const int sel = getSelectedGridIndex();
    const double handle = 6.0;

    for (size_t gi = 0; gi < _imp->grids.size(); ++gi) {
        const Grid& g = _imp->grids[gi];
        const bool isSel = ((int)gi == sel);
        // Corners in click order: 0 -> 1 -> 2 -> 3 around the shape. Rows run
        // between edge 0-3 and edge 1-2, columns between edge 0-1 and edge 3-2.
        if (isSel) glColor4f(1.0f, 0.85f, 0.2f, 0.9f); else glColor4f(0.3f, 0.9f, 1.0f, 0.7f);
        glLineWidth(isSel ? 1.8f : 1.2f);
        glBegin(GL_LINE_LOOP);
        for (int c = 0; c < 4; ++c) glVertex2d(g.x[c], g.y[c]);
        glEnd();

        glLineWidth(1.0f);
        if (isSel) glColor4f(1.0f, 0.85f, 0.2f, 0.45f); else glColor4f(0.3f, 0.9f, 1.0f, 0.35f);
        glBegin(GL_LINES);
        for (int r = 1; r < g.rows; ++r) {
            const double t = (double)r / g.rows;
            double lx, ly, rx, ry;
            lerp2(g.x[0], g.y[0], g.x[3], g.y[3], t, lx, ly);
            lerp2(g.x[1], g.y[1], g.x[2], g.y[2], t, rx, ry);
            glVertex2d(lx, ly); glVertex2d(rx, ry);
        }
        for (int c = 1; c < g.cols; ++c) {
            const double t = (double)c / g.cols;
            double bx, by, tx, ty;
            lerp2(g.x[0], g.y[0], g.x[1], g.y[1], t, bx, by);
            lerp2(g.x[3], g.y[3], g.x[2], g.y[2], t, tx, ty);
            glVertex2d(bx, by); glVertex2d(tx, ty);
        }
        glEnd();

        // Corner handles
        if (isSel) glColor4f(1.0f, 0.85f, 0.2f, 1.0f); else glColor4f(0.3f, 0.9f, 1.0f, 0.9f);
        glLineWidth(1.5f);
        for (int c = 0; c < 4; ++c) {
            glBegin(GL_LINE_LOOP);
            glVertex2d(g.x[c] - handle, g.y[c] - handle);
            glVertex2d(g.x[c] + handle, g.y[c] - handle);
            glVertex2d(g.x[c] + handle, g.y[c] + handle);
            glVertex2d(g.x[c] - handle, g.y[c] + handle);
            glEnd();
        }
    }

    // Corners placed so far for the grid being created
    if (_imp->placingCount > 0) {
        glColor4f(1.0f, 0.4f, 0.2f, 1.0f);
        glLineWidth(1.5f);
        glBegin(GL_LINE_STRIP);
        for (int c = 0; c < _imp->placingCount; ++c) glVertex2d(_imp->placingX[c], _imp->placingY[c]);
        glEnd();
        for (int c = 0; c < _imp->placingCount; ++c) {
            glBegin(GL_LINE_LOOP);
            glVertex2d(_imp->placingX[c] - handle, _imp->placingY[c] - handle);
            glVertex2d(_imp->placingX[c] + handle, _imp->placingY[c] - handle);
            glVertex2d(_imp->placingX[c] + handle, _imp->placingY[c] + handle);
            glVertex2d(_imp->placingX[c] - handle, _imp->placingY[c] + handle);
            glEnd();
        }
    }
}

bool
GeoBuilder::onOverlayPenDown(double /*time*/, const RenderScale& /*renderScale*/, ViewIdx /*view*/,
                             const QPointF& /*viewportPos*/, const QPointF& pos,
                             double /*pressure*/, double /*timestamp*/, PenType /*pen*/)
{
    // Placing a new grid: each click is a corner.
    if (_imp->placingCount >= 0) {
        const int c = _imp->placingCount;
        _imp->placingX[c] = pos.x();
        _imp->placingY[c] = pos.y();
        ++_imp->placingCount;
        if (_imp->placingCount < 4) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Corner %d of 4 placed - click corner %d.", _imp->placingCount, _imp->placingCount + 1);
            setGridStatus(buf);
            redrawOverlayInteract();
            return true;
        }
        Grid g;
        g.name = "Grid" + std::to_string(++_imp->gridCounter);
        for (int i = 0; i < 4; ++i) { g.x[i] = _imp->placingX[i]; g.y[i] = _imp->placingY[i]; }
        KnobIntPtr rows = _imp->gridRows.lock();
        KnobIntPtr cols = _imp->gridCols.lock();
        // New grids take the subdivision currently shown (defaults 4 x 4).
        if (rows && rows->isEnabled(0)) g.rows = std::max(1, rows->getValue());
        if (cols && cols->isEnabled(0)) g.cols = std::max(1, cols->getValue());
        _imp->placingCount = -1;
        _imp->grids.push_back(g);
        saveGridsToKnob();
        refreshGridChoice();
        KnobChoicePtr choice = _imp->gridChoice.lock();
        if (choice) { _imp->syncing = true; choice->setValue((int)_imp->grids.size() - 1); _imp->syncing = false; }
        loadSelectedGridIntoKnobs();
        setGridStatus(g.name + " created - drag its corners, or Add Grid for another.");
        redrawOverlayInteract();
        return true;
    }

    // Otherwise: grab a corner handle. The selected grid wins ties.
    const double threshold = 12.0;
    auto hit = [&](int gi) -> int {
        const Grid& g = _imp->grids[gi];
        for (int c = 0; c < 4; ++c) {
            const double dx = pos.x() - g.x[c], dy = pos.y() - g.y[c];
            if (dx * dx + dy * dy < threshold * threshold) return c;
        }
        return -1;
    };
    const int sel = getSelectedGridIndex();
    if (sel >= 0) {
        const int c = hit(sel);
        if (c >= 0) { _imp->dragGrid = sel; _imp->dragCorner = c; return true; }
    }
    for (size_t gi = 0; gi < _imp->grids.size(); ++gi) {
        if ((int)gi == sel) continue;
        const int c = hit((int)gi);
        if (c >= 0) {
            _imp->dragGrid = (int)gi; _imp->dragCorner = c;
            KnobChoicePtr choice = _imp->gridChoice.lock();
            if (choice) { _imp->syncing = true; choice->setValue((int)gi); _imp->syncing = false; }
            loadSelectedGridIntoKnobs();
            redrawOverlayInteract();
            return true;
        }
    }
    return false;
}

bool
GeoBuilder::onOverlayPenMotion(double /*time*/, const RenderScale& /*renderScale*/, ViewIdx /*view*/,
                               const QPointF& /*viewportPos*/, const QPointF& pos,
                               double /*pressure*/, double /*timestamp*/)
{
    if (_imp->dragGrid >= 0 && _imp->dragGrid < (int)_imp->grids.size() && _imp->dragCorner >= 0) {
        Grid& g = _imp->grids[_imp->dragGrid];
        g.x[_imp->dragCorner] = pos.x();
        g.y[_imp->dragCorner] = pos.y();
        redrawOverlayInteract();
        return true;
    }
    return false;
}

bool
GeoBuilder::onOverlayPenUp(double /*time*/, const RenderScale& /*renderScale*/, ViewIdx /*view*/,
                           const QPointF& /*viewportPos*/, const QPointF& /*pos*/,
                           double /*pressure*/, double /*timestamp*/)
{
    if (_imp->dragGrid >= 0) {
        _imp->dragGrid = -1;
        _imp->dragCorner = -1;
        saveGridsToKnob();   // one undo step per drag
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Shape list <-> hidden knob
// ---------------------------------------------------------------------------

void
GeoBuilder::saveShapesToKnob()
{
    // One shape per line, '|'-separated; names carry no '|' or newlines.
    std::ostringstream ss;
    for (size_t i = 0; i < _imp->shapes.size(); ++i) {
        const Shape& s = _imp->shapes[i];
        std::string nm = s.name;
        for (size_t c = 0; c < nm.size(); ++c) { if (nm[c] == '|' || nm[c] == '\n' || nm[c] == '\r') nm[c] = ' '; }
        ss << s.type << '|' << nm << '|' << (s.visible ? 1 : 0) << '|' << s.rows << '|' << s.cols << '|'
           << s.size << '|' << s.height << '|'
           << s.tx << '|' << s.ty << '|' << s.tz << '|'
           << s.rx << '|' << s.ry << '|' << s.rz << '|'
           << s.sx << '|' << s.sy << '|' << s.sz << '\n';
    }
    KnobStringPtr k = _imp->shapesData.lock();
    if (!k) return;
    _imp->syncing = true;
    k->setValue(ss.str());
    _imp->syncing = false;
}

void
GeoBuilder::loadShapesFromKnob()
{
    _imp->shapes.clear();
    for (int i = 0; i < 4; ++i) _imp->shapeCounter[i] = 0;
    KnobStringPtr k = _imp->shapesData.lock();
    if (!k) return;
    std::istringstream in(k->getValue());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        std::string cur;
        for (size_t c = 0; c < line.size(); ++c) {
            if (line[c] == '|') { f.push_back(cur); cur.clear(); } else cur.push_back(line[c]);
        }
        f.push_back(cur);
        if (f.size() < 16) continue;
        Shape s;
        s.type = std::atoi(f[0].c_str());
        s.name = f[1];
        s.visible = std::atoi(f[2].c_str()) != 0;
        s.rows = std::max(1, std::atoi(f[3].c_str()));
        s.cols = std::max(1, std::atoi(f[4].c_str()));
        s.size = std::atof(f[5].c_str());
        s.height = std::atof(f[6].c_str());
        s.tx = std::atof(f[7].c_str()); s.ty = std::atof(f[8].c_str()); s.tz = std::atof(f[9].c_str());
        s.rx = std::atof(f[10].c_str()); s.ry = std::atof(f[11].c_str()); s.rz = std::atof(f[12].c_str());
        s.sx = std::atof(f[13].c_str()); s.sy = std::atof(f[14].c_str()); s.sz = std::atof(f[15].c_str());
        if (s.type < 0 || s.type > 3) s.type = 0;
        ++_imp->shapeCounter[s.type];
        _imp->shapes.push_back(s);
    }
}

std::vector<GeoBuilder::Shape>
GeoBuilder::getShapes() const
{
    return _imp->shapes;
}

int
GeoBuilder::getSelectedShapeIndex() const
{
    KnobChoicePtr c = _imp->shapeChoice.lock();
    if (!c || _imp->shapes.empty()) return -1;
    int i = c->getValue();
    if (i < 0 || i >= (int)_imp->shapes.size()) return -1;
    return i;
}

// ---------------------------------------------------------------------------
// Selection <-> per-shape knobs
// ---------------------------------------------------------------------------

void
GeoBuilder::refreshShapeChoice()
{
    KnobChoicePtr c = _imp->shapeChoice.lock();
    if (!c) return;
    std::vector<ChoiceOption> entries;
    for (size_t i = 0; i < _imp->shapes.size(); ++i) {
        entries.push_back(ChoiceOption(_imp->shapes[i].name, "", ""));
    }
    _imp->syncing = true;
    c->populateChoices(entries);
    _imp->syncing = false;

    KnobStringPtr info = _imp->info.lock();
    if (info) {
        std::ostringstream ss;
        size_t nv = 0, nf = 0;
        {
            std::lock_guard<std::mutex> lk(_meshMutex);
            if (_mesh) { nv = _mesh->numVertices; nf = _mesh->numFaces; }
        }
        ss << _imp->shapes.size() << " shape" << (_imp->shapes.size() == 1 ? "" : "s")
           << ", " << nv << " vertices, " << nf << " faces";
        _imp->syncing = true;
        info->setValue(ss.str());
        _imp->syncing = false;
    }
}

void
GeoBuilder::loadSelectedShapeIntoKnobs()
{
    const int i = getSelectedShapeIndex();
    const bool has = (i >= 0);
    Shape s;
    if (has) s = _imp->shapes[i];

    _imp->syncing = true;
    beginChanges();
    auto setD = [&](KnobDoubleWPtr w, double v) { KnobDoublePtr k = w.lock(); if (k) { k->setValue(v); k->setEnabled(0, has); } };
    KnobStringPtr nm = _imp->shapeName.lock();
    if (nm) { nm->setValue(s.name); nm->setEnabled(0, has); }
    KnobBoolPtr vis = _imp->shapeVisible.lock();
    if (vis) { vis->setValue(s.visible); vis->setEnabled(0, has); }
    KnobIntPtr rows = _imp->shapeRows.lock();
    if (rows) { rows->setValue(s.rows); rows->setEnabled(0, has && s.type != 1); }
    KnobIntPtr cols = _imp->shapeCols.lock();
    if (cols) { cols->setValue(s.cols); cols->setEnabled(0, has && s.type != 1); }
    setD(_imp->shapeSize, s.size);
    setD(_imp->shapeHeight, s.height);
    KnobDoublePtr hk = _imp->shapeHeight.lock();
    if (hk) hk->setEnabled(0, has && s.type == 3);
    setD(_imp->shapeTX, s.tx); setD(_imp->shapeTY, s.ty); setD(_imp->shapeTZ, s.tz);
    setD(_imp->shapeRX, s.rx); setD(_imp->shapeRY, s.ry); setD(_imp->shapeRZ, s.rz);
    setD(_imp->shapeSX, s.sx); setD(_imp->shapeSY, s.sy); setD(_imp->shapeSZ, s.sz);
    for (KnobButtonWPtr w : { _imp->duplicateShape, _imp->deleteShape }) {
        KnobButtonPtr b = w.lock(); if (b) b->setEnabled(0, has);
    }
    endChanges();
    _imp->syncing = false;
}

void
GeoBuilder::storeKnobsIntoSelectedShape()
{
    const int i = getSelectedShapeIndex();
    if (i < 0) return;
    Shape& s = _imp->shapes[i];
    auto getD = [&](KnobDoubleWPtr w, double fallback) { KnobDoublePtr k = w.lock(); return k ? k->getValue() : fallback; };
    KnobStringPtr nm = _imp->shapeName.lock();
    if (nm) { std::string n = nm->getValue(); if (!n.empty()) s.name = n; }
    KnobBoolPtr vis = _imp->shapeVisible.lock();
    if (vis) s.visible = vis->getValue();
    KnobIntPtr rows = _imp->shapeRows.lock();
    if (rows) s.rows = std::max(1, rows->getValue());
    KnobIntPtr cols = _imp->shapeCols.lock();
    if (cols) s.cols = std::max(1, cols->getValue());
    s.size = getD(_imp->shapeSize, s.size);
    s.height = getD(_imp->shapeHeight, s.height);
    s.tx = getD(_imp->shapeTX, s.tx); s.ty = getD(_imp->shapeTY, s.ty); s.tz = getD(_imp->shapeTZ, s.tz);
    s.rx = getD(_imp->shapeRX, s.rx); s.ry = getD(_imp->shapeRY, s.ry); s.rz = getD(_imp->shapeRZ, s.rz);
    s.sx = getD(_imp->shapeSX, s.sx); s.sy = getD(_imp->shapeSY, s.sy); s.sz = getD(_imp->shapeSZ, s.sz);
}

void
GeoBuilder::addShape(int type)
{
    static const char* baseNames[4] = {"Card", "Cube", "Sphere", "Cylinder"};
    Shape s;
    s.type = type;
    s.name = std::string(baseNames[type]) + std::to_string(++_imp->shapeCounter[type]);
    switch (type) {
        case 0: s.rows = 4; s.cols = 4; s.size = 1.0; break;      // card: Nuke's 4x4 default
        case 1: s.rows = 1; s.cols = 1; s.size = 1.0; break;
        case 2: s.rows = 20; s.cols = 20; s.size = 1.0; break;    // sphere: 20x20
        case 3: s.rows = 2; s.cols = 20; s.size = 1.0; s.height = 2.0; break;
    }
    _imp->shapes.push_back(s);
    saveShapesToKnob();
    refreshShapeChoice();
    KnobChoicePtr c = _imp->shapeChoice.lock();
    if (c) {
        _imp->syncing = true;
        c->setValue((int)_imp->shapes.size() - 1);
        _imp->syncing = false;
    }
    loadSelectedShapeIntoKnobs();
    rebuildMesh();
    refreshShapeChoice(); // info line with the new counts
}

// ---------------------------------------------------------------------------
// Mesh generation
// ---------------------------------------------------------------------------

namespace {

struct Builder
{
    MeshData& m;
    int base = 0;            // vertex index offset of the shape being emitted
    const float* xf = nullptr; // column-major 4x4 for the shape

    explicit Builder(MeshData& mesh) : m(mesh) {}

    void beginShape(const float* mat) { base = (int)(m.vertices.size() / 3); xf = mat; }

    int vert(float x, float y, float z)
    {
        // Apply the shape's local transform on the way in.
        const float tx = xf[0]*x + xf[4]*y + xf[8]*z + xf[12];
        const float ty = xf[1]*x + xf[5]*y + xf[9]*z + xf[13];
        const float tz = xf[2]*x + xf[6]*y + xf[10]*z + xf[14];
        m.vertices.push_back(tx); m.vertices.push_back(ty); m.vertices.push_back(tz);
        return (int)(m.vertices.size() / 3) - 1;
    }

    void edge(int a, int b) { m.edgeIndices.push_back(a); m.edgeIndices.push_back(b); }

    // Counter-clockwise from outside — winding is the source of truth for
    // normals in the viewport, ScanlineRender and Cycles (mesh path).
    void quad(int a, int b, int c, int d, float ua, float va, float ub, float vb, float uc, float vc, float ud, float vd)
    {
        m.faceIndices.push_back(a); m.faceIndices.push_back(b); m.faceIndices.push_back(c); m.faceIndices.push_back(d);
        m.faceCounts.push_back(4);
        m.uvs.push_back(ua); m.uvs.push_back(va); m.uvs.push_back(ub); m.uvs.push_back(vb);
        m.uvs.push_back(uc); m.uvs.push_back(vc); m.uvs.push_back(ud); m.uvs.push_back(vd);
        edge(a, b); edge(b, c); edge(c, d); edge(d, a);
    }
    void tri(int a, int b, int c, float ua, float va, float ub, float vb, float uc, float vc)
    {
        m.faceIndices.push_back(a); m.faceIndices.push_back(b); m.faceIndices.push_back(c);
        m.faceCounts.push_back(3);
        m.uvs.push_back(ua); m.uvs.push_back(va); m.uvs.push_back(ub); m.uvs.push_back(vb); m.uvs.push_back(uc); m.uvs.push_back(vc);
        edge(a, b); edge(b, c); edge(c, a);
    }
};

// Card: rows x cols grid in the XY plane, side `size`, centred, facing +Z.
void emitCard(Builder& b, const GeoBuilder::Shape& s)
{
    const int R = std::max(1, s.rows), C = std::max(1, s.cols);
    const float h = (float)s.size * 0.5f;
    std::vector<int> idx((R + 1) * (C + 1));
    for (int r = 0; r <= R; ++r) {
        for (int c = 0; c <= C; ++c) {
            const float x = -h + (float)s.size * (float)c / C;
            const float y = -h + (float)s.size * (float)r / R;
            idx[r * (C + 1) + c] = b.vert(x, y, 0.f);
        }
    }
    for (int r = 0; r < R; ++r) {
        for (int c = 0; c < C; ++c) {
            const float u0 = (float)c / C, u1 = (float)(c + 1) / C, v0 = (float)r / R, v1 = (float)(r + 1) / R;
            b.quad(idx[r*(C+1)+c], idx[r*(C+1)+c+1], idx[(r+1)*(C+1)+c+1], idx[(r+1)*(C+1)+c],
                   u0, v0, u1, v0, u1, v1, u0, v1);
        }
    }
}

// Cube: 6 outward-facing quads, edge `size`, 4 verts per face so UVs are per face.
void emitCube(Builder& b, const GeoBuilder::Shape& s)
{
    const float h = (float)s.size * 0.5f;
    // Each face: 4 corners in CCW order seen from outside.
    const float faces[6][4][3] = {
        {{-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h}},   // +Z
        {{ h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h}},   // -Z
        {{ h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h}},   // +X
        {{-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h}},   // -X
        {{-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h}},   // +Y
        {{-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h}},   // -Y
    };
    for (int f = 0; f < 6; ++f) {
        int v[4];
        for (int i = 0; i < 4; ++i) v[i] = b.vert(faces[f][i][0], faces[f][i][1], faces[f][i][2]);
        b.quad(v[0], v[1], v[2], v[3], 0, 0, 1, 0, 1, 1, 0, 1);
    }
}

// Sphere: lat/long, `rows` bands from the south pole up, `cols` around; pole
// bands are triangles so no degenerate quads.
void emitSphere(Builder& b, const GeoBuilder::Shape& s)
{
    const int R = std::max(2, s.rows), C = std::max(3, s.cols);
    const float rad = (float)s.size;
    std::vector<int> idx((R + 1) * (C + 1));
    for (int r = 0; r <= R; ++r) {
        const float phi = (float)M_PI * ((float)r / R - 0.5f);   // -90 .. +90
        for (int c = 0; c <= C; ++c) {
            const float th = 2.f * (float)M_PI * (float)c / C;
            idx[r * (C + 1) + c] = b.vert(rad * cosf(phi) * sinf(th), rad * sinf(phi), rad * cosf(phi) * cosf(th));
        }
    }
    for (int r = 0; r < R; ++r) {
        for (int c = 0; c < C; ++c) {
            const float u0 = (float)c / C, u1 = (float)(c + 1) / C, v0 = (float)r / R, v1 = (float)(r + 1) / R;
            const int a = idx[r*(C+1)+c], bb = idx[r*(C+1)+c+1], cc = idx[(r+1)*(C+1)+c+1], d = idx[(r+1)*(C+1)+c];
            if (r == 0)          b.tri(a, cc, d, (u0 + u1) * 0.5f, v0, u1, v1, u0, v1);       // south pole fan
            else if (r == R - 1) b.tri(a, bb, cc, u0, v0, u1, v0, (u0 + u1) * 0.5f, v1);     // north pole fan
            else                 b.quad(a, bb, cc, d, u0, v0, u1, v0, u1, v1, u0, v1);
        }
    }
}

// Cylinder along Y: radius `size`, height `height`, `rows` bands, `cols`
// around, capped top and bottom with triangle fans.
void emitCylinder(Builder& b, const GeoBuilder::Shape& s)
{
    const int R = std::max(1, s.rows), C = std::max(3, s.cols);
    const float rad = (float)s.size, hh = (float)s.height * 0.5f;
    std::vector<int> idx((R + 1) * (C + 1));
    for (int r = 0; r <= R; ++r) {
        const float y = -hh + (float)s.height * (float)r / R;
        for (int c = 0; c <= C; ++c) {
            const float th = 2.f * (float)M_PI * (float)c / C;
            idx[r * (C + 1) + c] = b.vert(rad * sinf(th), y, rad * cosf(th));
        }
    }
    for (int r = 0; r < R; ++r) {
        for (int c = 0; c < C; ++c) {
            const float u0 = (float)c / C, u1 = (float)(c + 1) / C, v0 = (float)r / R, v1 = (float)(r + 1) / R;
            b.quad(idx[r*(C+1)+c], idx[r*(C+1)+c+1], idx[(r+1)*(C+1)+c+1], idx[(r+1)*(C+1)+c],
                   u0, v0, u1, v0, u1, v1, u0, v1);
        }
    }
    // Caps: (centre, p_i, p_i+1) has a +Y normal for increasing theta.
    const int top = b.vert(0.f, hh, 0.f), bot = b.vert(0.f, -hh, 0.f);
    for (int c = 0; c < C; ++c) {
        const float th0 = 2.f * (float)M_PI * (float)c / C, th1 = 2.f * (float)M_PI * (float)(c + 1) / C;
        const int t0 = idx[R*(C+1)+c], t1 = idx[R*(C+1)+c+1];
        const int b0 = idx[c], b1 = idx[c+1];
        b.tri(top, t0, t1, 0.5f, 0.5f, 0.5f + 0.5f*sinf(th0), 0.5f + 0.5f*cosf(th0), 0.5f + 0.5f*sinf(th1), 0.5f + 0.5f*cosf(th1));
        b.tri(bot, b1, b0, 0.5f, 0.5f, 0.5f + 0.5f*sinf(th1), 0.5f + 0.5f*cosf(th1), 0.5f + 0.5f*sinf(th0), 0.5f + 0.5f*cosf(th0));
    }
}

} // namespace

void
GeoBuilder::rebuildMesh()
{
    MeshDataPtr mesh = std::make_shared<MeshData>();
    Builder b(*mesh);
    for (size_t i = 0; i < _imp->shapes.size(); ++i) {
        const Shape& s = _imp->shapes[i];
        if (!s.visible) continue;
        float xf[16];
        SceneGraph::buildTRS((float)s.tx, (float)s.ty, (float)s.tz,
                             (float)s.rx, (float)s.ry, (float)s.rz,
                             (float)s.sx, (float)s.sy, (float)s.sz, xf);
        b.beginShape(xf);
        switch (s.type) {
            case 0: emitCard(b, s); break;
            case 1: emitCube(b, s); break;
            case 2: emitSphere(b, s); break;
            case 3: emitCylinder(b, s); break;
        }
    }
    mesh->numVertices = mesh->vertices.size() / 3;
    mesh->numFaces = mesh->faceCounts.size();
    mesh->texCoordComponents = 2;
    mesh->hasUVs = !mesh->uvs.empty();
    {
        std::lock_guard<std::mutex> lk(_meshMutex);
        _mesh = mesh;
    }
}

MeshDataPtr
GeoBuilder::getMeshData(double /*time*/) const
{
    std::lock_guard<std::mutex> lk(_meshMutex);
    return _mesh;
}

// ---------------------------------------------------------------------------
// Knob changes
// ---------------------------------------------------------------------------

bool
GeoBuilder::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/,
                        ViewSpec /*view*/, double /*time*/, bool /*originatedFromMainThread*/)
{
    if (_imp->syncing) return false;

    // ---- grids ----
    if (k == _imp->addGrid.lock().get()) {
        _imp->placingCount = 0;
        setGridStatus("Click corner 1 of 4 in the viewer (go around the shape).");
        redrawOverlayInteract();
        return true;
    }
    if (k == _imp->deleteGrid.lock().get()) {
        const int i = getSelectedGridIndex();
        if (i < 0) return true;
        _imp->grids.erase(_imp->grids.begin() + i);
        saveGridsToKnob();
        refreshGridChoice();
        KnobChoicePtr c = _imp->gridChoice.lock();
        if (c && !_imp->grids.empty()) { _imp->syncing = true; c->setValue(std::min(i, (int)_imp->grids.size() - 1)); _imp->syncing = false; }
        loadSelectedGridIntoKnobs();
        setGridStatus(_imp->grids.empty() ? "No grids. Add Grid, then click four corners in the viewer."
                                          : "Grid deleted.");
        redrawOverlayInteract();
        return true;
    }
    if (k == _imp->gridChoice.lock().get()) {
        loadSelectedGridIntoKnobs();
        redrawOverlayInteract();
        return true;
    }
    if (k == _imp->gridRows.lock().get() || k == _imp->gridCols.lock().get()) {
        storeKnobsIntoSelectedGrid();
        saveGridsToKnob();
        redrawOverlayInteract();
        return true;
    }
    if (k == _imp->gridsData.lock().get()) {
        // Project load, undo, or a script writing the list directly.
        const int keep = getSelectedGridIndex();
        loadGridsFromKnob();
        refreshGridChoice();
        KnobChoicePtr c = _imp->gridChoice.lock();
        if (c && !_imp->grids.empty()) { _imp->syncing = true; c->setValue(std::max(0, std::min(keep, (int)_imp->grids.size() - 1))); _imp->syncing = false; }
        loadSelectedGridIntoKnobs();
        redrawOverlayInteract();
        return true;
    }

    // ---- shapes ----
    if (k == _imp->addCard.lock().get())     { addShape(0); return true; }
    if (k == _imp->addCube.lock().get())     { addShape(1); return true; }
    if (k == _imp->addSphere.lock().get())   { addShape(2); return true; }
    if (k == _imp->addCylinder.lock().get()) { addShape(3); return true; }

    if (k == _imp->duplicateShape.lock().get()) {
        const int i = getSelectedShapeIndex();
        if (i < 0) return true;
        Shape s = _imp->shapes[i];
        s.name += "_copy";
        _imp->shapes.insert(_imp->shapes.begin() + i + 1, s);
        saveShapesToKnob();
        refreshShapeChoice();
        KnobChoicePtr c = _imp->shapeChoice.lock();
        if (c) { _imp->syncing = true; c->setValue(i + 1); _imp->syncing = false; }
        loadSelectedShapeIntoKnobs();
        rebuildMesh();
        refreshShapeChoice();
        return true;
    }
    if (k == _imp->deleteShape.lock().get()) {
        const int i = getSelectedShapeIndex();
        if (i < 0) return true;
        _imp->shapes.erase(_imp->shapes.begin() + i);
        saveShapesToKnob();
        refreshShapeChoice();
        KnobChoicePtr c = _imp->shapeChoice.lock();
        if (c && !_imp->shapes.empty()) { _imp->syncing = true; c->setValue(std::min(i, (int)_imp->shapes.size() - 1)); _imp->syncing = false; }
        loadSelectedShapeIntoKnobs();
        rebuildMesh();
        refreshShapeChoice();
        return true;
    }
    if (k == _imp->shapeChoice.lock().get()) {
        loadSelectedShapeIntoKnobs();
        return true;
    }
    if (k == _imp->shapesData.lock().get()) {
        // Project load, undo, or a script writing the list directly.
        loadShapesFromKnob();
        refreshShapeChoice();
        loadSelectedShapeIntoKnobs();
        rebuildMesh();
        refreshShapeChoice();
        return true;
    }

    // Any of the per-shape knobs: write back, re-serialise, rebuild.
    KnobI* shapeKnobs[] = {
        _imp->shapeName.lock().get(), _imp->shapeVisible.lock().get(),
        _imp->shapeRows.lock().get(), _imp->shapeCols.lock().get(),
        _imp->shapeSize.lock().get(), _imp->shapeHeight.lock().get(),
        _imp->shapeTX.lock().get(), _imp->shapeTY.lock().get(), _imp->shapeTZ.lock().get(),
        _imp->shapeRX.lock().get(), _imp->shapeRY.lock().get(), _imp->shapeRZ.lock().get(),
        _imp->shapeSX.lock().get(), _imp->shapeSY.lock().get(), _imp->shapeSZ.lock().get(),
    };
    for (KnobI* sk : shapeKnobs) {
        if (sk && sk == k) {
            storeKnobsIntoSelectedShape();
            saveShapesToKnob();
            if (k == _imp->shapeName.lock().get()) {
                const int i = getSelectedShapeIndex();
                refreshShapeChoice();
                KnobChoicePtr c = _imp->shapeChoice.lock();
                if (c && i >= 0) { _imp->syncing = true; c->setValue(i); _imp->syncing = false; }
            }
            rebuildMesh();
            refreshShapeChoice();
            return true;
        }
    }
    return false;
}

void
GeoBuilder::onKnobsLoaded()
{
    // Project restored: the hidden lists are the truth; rebuild everything from them.
    loadGridsFromKnob();
    refreshGridChoice();
    {
        KnobChoicePtr gc = _imp->gridChoice.lock();
        if (gc && !_imp->grids.empty()) { _imp->syncing = true; gc->setValue(0); _imp->syncing = false; }
    }
    loadSelectedGridIntoKnobs();
    setGridStatus(_imp->grids.empty() ? "Add Grid, then click four corners in the viewer."
                                      : std::to_string(_imp->grids.size()) + " grid(s) loaded.");
    loadShapesFromKnob();
    refreshShapeChoice();
    KnobChoicePtr c = _imp->shapeChoice.lock();
    if (c && !_imp->shapes.empty()) { _imp->syncing = true; c->setValue(0); _imp->syncing = false; }
    loadSelectedShapeIntoKnobs();
    rebuildMesh();
    refreshShapeChoice();
}

// ---------------------------------------------------------------------------
// RoD / render (this node produces no image)
// ---------------------------------------------------------------------------

StatusEnum
GeoBuilder::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                  ViewIdx view, RectD* rod)
{
    // With a plate on src the node is a pass-through (isIdentity), so report
    // the plate's RoD; otherwise a token 1x1 like the other 3D sources.
    EffectInstancePtr src = getInput(2);
    if (src) {
        bool isProject = false;
        return src->getRegionOfDefinition_public(src->getHash(), time, scale, view, rod, &isProject);
    }
    rod->x1 = 0; rod->y1 = 0; rod->x2 = 1; rod->y2 = 1;
    return eStatusOK;
}

StatusEnum
GeoBuilder::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

// ---------------------------------------------------------------------------
// MaterialProvider
// ---------------------------------------------------------------------------

void
GeoBuilder::getMaterialBaseColor(double time, double& r, double& g, double& b) const
{
    KnobColorPtr c = _imp->baseColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 0.8; g = 0.8; b = 0.8; }
}
double GeoBuilder::getMaterialRoughness(double time) const
{ KnobDoublePtr k = _imp->roughness.lock(); return k ? k->getValueAtTime(time) : 0.5; }
double GeoBuilder::getMaterialMetallic(double time) const
{ KnobDoublePtr k = _imp->metallic.lock(); return k ? k->getValueAtTime(time) : 0.0; }
double GeoBuilder::getMaterialSpecular(double time) const
{ KnobDoublePtr k = _imp->specular.lock(); return k ? k->getValueAtTime(time) : 0.5; }
void
GeoBuilder::getMaterialEmission(double time, double& r, double& g, double& b, double& strength) const
{
    KnobColorPtr c = _imp->emissionColor.lock();
    if (c) { r = c->getValueAtTime(time, 0); g = c->getValueAtTime(time, 1); b = c->getValueAtTime(time, 2); }
    else { r = 1.0; g = 1.0; b = 1.0; }
    KnobDoublePtr s = _imp->emissionStrength.lock();
    strength = s ? s->getValueAtTime(time) : 0.0;
}
double GeoBuilder::getMaterialTransmission(double time) const
{ KnobDoublePtr k = _imp->transmission.lock(); return k ? k->getValueAtTime(time) : 0.0; }
double GeoBuilder::getMaterialIOR(double time) const
{ KnobDoublePtr k = _imp->ior.lock(); return k ? k->getValueAtTime(time) : 1.45; }
std::string GeoBuilder::getMaterialTextureFile() const
{ KnobFilePtr k = _imp->textureFile.lock(); return k ? k->getValue() : std::string(); }
std::string GeoBuilder::getMaterialDiffuseColorspace() const
{
    KnobChoicePtr k = _imp->diffuseColorspace.lock();
    int idx = k ? k->getValue() : 0;
    const char* names[] = {"sRGB", "Linear", "ACEScg", "Raw"};
    return (idx >= 0 && idx < 4) ? names[idx] : "sRGB";
}
bool GeoBuilder::hasMaterialInput() const
{
    EffectInstancePtr inp = skipDots(getInput(0));
    return inp && dynamic_cast<MaterialProvider*>(inp.get()) != nullptr;
}
MaterialProvider* GeoBuilder::getConnectedMaterial() const
{
    EffectInstancePtr inp = skipDots(getInput(0));
    return inp ? dynamic_cast<MaterialProvider*>(inp.get()) : nullptr;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_GeoBuilder.cpp"
