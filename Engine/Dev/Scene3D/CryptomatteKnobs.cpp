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

#include "CryptomatteKnobs.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

void
createCryptomatteKnobs(EffectInstance* effect, bool perInstanceOption)
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(effect, QObject::tr("Cryptomatte"));
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(effect, QObject::tr("Crypto Asset"));
        k->setName(kCryptomatteAssetKnobName);
        k->setAnimationEnabled(false);
        k->setHintToolTip(QObject::tr("The name this object is filed under in the CryptoAsset layer of a Cycles render "
                                      "(objects sharing a name form one matte). Empty: the nearest Group3D above that "
                                      "sets a Crypto Asset, else the top-most group's name, else this node's name."));
        page->addKnob(k);
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(effect, QObject::tr("Crypto Custom"));
        k->setName(kCryptomatteCustomKnobName);
        k->setAnimationEnabled(false);
        k->setHintToolTip(QObject::tr("The name this object is filed under in the CryptoCustom layer of a Cycles render: a "
                                      "free tag (\"hero\", \"background\", \"glass\"...), so any grouping can become a "
                                      "matte. Empty: inherited from the nearest group above that sets one, else "
                                      "\"default\"."));
        page->addKnob(k);
    }
    if (perInstanceOption) {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(effect, QObject::tr("Crypto Per Instance"));
        k->setName(kCryptomattePerInstanceKnobName);
        k->setDefaultValue(false);
        k->setAnimationEnabled(false);
        k->setHintToolTip(QObject::tr("Give every instance its own Cryptomatte object ID, named <node>/<instance index> "
                                      "(ParticleInstance1/0, /1, ...), so single instances can be matted. Off: all "
                                      "instances share the node's ID. The asset layer keeps the whole system as one "
                                      "matte either way. Indices follow the particle order of the current frame."));
        page->addKnob(k);
    }
}

bool
getCryptomatteKnobBool(const NodePtr& node, const char* knobName)
{
    if (!node) {
        return false;
    }
    KnobBoolPtr k = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(knobName));
    return k ? k->getValue() : false;
}

std::string
getCryptomatteKnobValue(const NodePtr& node, const char* knobName)
{
    if (!node) {
        return std::string();
    }
    KnobStringPtr k = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(knobName));
    return k ? k->getValue() : std::string();
}

NATRON_NAMESPACE_EXIT
