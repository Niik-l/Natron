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

// Derived from vectorNuke (https://github.com/erinwnash/vectorNuke), GPL-2.0:
// the node semantics (fit modes, flip-Y, per-channel colors, layer grouping,
// feather handling) port the vectorNuke roto_builder.py behavior natively.

#ifndef NATRON_ENGINE_DEV_MASK_READMASK_H
#define NATRON_ENGINE_DEV_MASK_READMASK_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <memory>
#include <list>

#include "Global/Macros.h"

#include "Engine/RotoPaint.h"
#include "Engine/ViewIdx.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct ReadMaskPrivate;

/**
 * @brief "ReadMask": a vector-file importer that IS a Roto node.
 *
 * Parses a vector file (SVG in this version — the full path grammar, primitives,
 * transforms and styles) into real Bézier shapes inside this node's own RotoContext
 * and outputs the mask alpha like any Roto. The imported shapes are project-native:
 * fully editable in the Roto overlay/curve editor, serialized in the .natron project.
 *
 * Placement reproduces the vectorNuke fit model: Fit to Format (uniform 95% margin +
 * center), Center to Format (1:1 centered), Original (1:1 bottom-left) and Custom
 * scale/offset, with the top-left -> bottom-left Y flip on by default (tangent
 * handles flip accordingly). Fill/stroke colors and opacity import onto the shape
 * color/opacity knobs; layers become RotoLayer groups named after the file and the
 * SVG groups. With Feather Splines off (default) the imported shapes have zero
 * feather distance/fall-off, exactly like vectorNuke's clean single-line vectors.
 *
 * While file-driven, re-importing (file or knob changes) regenerates the shapes by
 * design — edits you make to imported shapes are wiped by the next import. The
 * "Bake Sequence" persistence path (per-frame keyframing, file-independent) is the
 * planned follow-up.
 *
 * Derived from vectorNuke, GPL-2.0.
 */
class ReadMask
    : public RotoNode
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new ReadMask(n); }

    ReadMask(NodePtr node);
    virtual ~ReadMask();

    // Versions are inherited from RotoPaint (1.0).

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_READMASK; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "ReadMask"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /// The mask importer is a pure source: no Bg clone sources, no Mask input —
    /// the RotoPaint input fan (Bg, Bg2..Bg10, Mask) would only be confusing here.
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }

    /// Vector-only surface: no brush/pencil/eraser tools, no stroke viewer knobs,
    /// no Stroke/Clone property tabs — raster paint is meaningless on a vector-file
    /// importer. Selection/point-edit/Bézier tools and the vector overlay knobs stay.
    virtual bool supportsRasterPaintTools() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

private Q_SLOTS:

    /// Connected to the File knob: re-imports when Auto Rebuild is on.
    void onFileKnobChanged(ViewSpec view, int dimension, int reason);

    /// Connected to the placement/option knobs: re-imports when Auto Rebuild is on.
    void onControlKnobChanged(ViewSpec view, int dimension, int reason);

    /// Connected to the Re-import button: imports regardless of Auto Rebuild.
    void onRefreshClicked();

private:

    /**
     * @brief Knob creation hook. Runs after the effect is attached to its node and
     * before the project loads knob values — the constructor is too early (knob value
     * evaluation routes through the node, which is not wired up while it is still
     * being constructed). RotoPaint seals initializeKnobs, so this is the latest safe
     * hook; it mirrors the RotoContext creating its "General" page before
     * initializeKnobs() runs.
     **/
    virtual void initializeData() OVERRIDE FINAL;

    void importFile(double time);
    void removeImportedItems();

    std::unique_ptr<ReadMaskPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MASK_READMASK_H
