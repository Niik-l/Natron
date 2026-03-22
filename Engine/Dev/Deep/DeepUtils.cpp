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

#include "DeepUtils.h"

#include "DeepColorCorrect.h"
#include "DeepCrop.h"
#include "DeepExpression.h"
#include "DeepFromImage.h"
#include "DeepGrade.h"
#include "DeepHoldout.h"
#include "DeepMerge.h"
#include "DeepRead.h"
#include "DeepRecolor.h"
#include "DeepReformat.h"
#include "DeepSlice.h"
#include "DeepTransform.h"

NATRON_NAMESPACE_ENTER

DeepImagePtr
getDeepImageFromEffect(EffectInstance* effect)
{
    if (!effect) {
        return DeepImagePtr();
    }

    // Try each known deep node type.
    // When adding a new deep node that produces deep output via getDeepImage(),
    // add it to this list.

    if (DeepRead* n = dynamic_cast<DeepRead*>(effect)) return n->getDeepImage();
    if (DeepMerge* n = dynamic_cast<DeepMerge*>(effect)) return n->getDeepImage();
    if (DeepRecolor* n = dynamic_cast<DeepRecolor*>(effect)) return n->getDeepImage();
    if (DeepSlice* n = dynamic_cast<DeepSlice*>(effect)) return n->getDeepImage();
    if (DeepGrade* n = dynamic_cast<DeepGrade*>(effect)) return n->getDeepImage();
    if (DeepHoldout* n = dynamic_cast<DeepHoldout*>(effect)) return n->getDeepImage();
    if (DeepFromImage* n = dynamic_cast<DeepFromImage*>(effect)) return n->getDeepImage();
    if (DeepReformat* n = dynamic_cast<DeepReformat*>(effect)) return n->getDeepImage();
    if (DeepCrop* n = dynamic_cast<DeepCrop*>(effect)) return n->getDeepImage();
    if (DeepTransform* n = dynamic_cast<DeepTransform*>(effect)) return n->getDeepImage();
    if (DeepExpression* n = dynamic_cast<DeepExpression*>(effect)) return n->getDeepImage();
    if (DeepColorCorrect* n = dynamic_cast<DeepColorCorrect*>(effect)) return n->getDeepImage();

    return DeepImagePtr();
}

NATRON_NAMESPACE_EXIT
