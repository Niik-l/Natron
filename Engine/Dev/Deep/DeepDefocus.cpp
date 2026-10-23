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

#include "DeepDefocus.h"

NATRON_NAMESPACE_ENTER

std::string
DeepDefocus::getPluginDescription() const
{
    return tr("Depth of field on deep compositing data.\n\n"
              "Every deep sample blurs at its own depth, so layers hidden behind the foreground show "
              "through its blur as they really are, transparent and anti-aliased edges keep their own "
              "depths, and volume samples blur along their depth range. Everything else is LensDefocus: "
              "match a camera (Camera input, the file's camera, focal length / film back / f-stop), real "
              "lens designs, bokeh shape, lens image stages, viewer focus picking and the debug outputs. "
              "The lens maps onto the deep image's own frame.\n\n"
              "Input: Deep (a deep image with colour: DeepRead, CyclesRender deep output, DeepMerge ...), "
              "Camera, Clean Background, Mask. Output: flat RGBA.").toStdString();
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DeepDefocus.cpp"
