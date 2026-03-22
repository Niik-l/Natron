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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "KnobGuiShuffle.h"

#include <QHBoxLayout>

#include "Engine/Dev/Channel/DevShuffle.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImagePlaneDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Gui/NewLayerDialog.h"
#include "Gui/ShuffleWidget.h"

NATRON_NAMESPACE_ENTER

KnobGuiShuffle::KnobGuiShuffle(KnobIPtr knob, KnobGuiContainerI *container)
    : KnobGui(knob, container)
    , _knob( std::dynamic_pointer_cast<KnobShuffle>(knob) )
    , _widget(NULL)
{
}

KnobGuiShuffle::~KnobGuiShuffle()
{
}

KnobIPtr
KnobGuiShuffle::getKnob() const
{
    return _knob.lock();
}

void
KnobGuiShuffle::createWidget(QHBoxLayout* layout)
{
    _widget = new ShuffleWidget( layout->parentWidget() );
    _widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // Row 1 connections
    QObject::connect(_widget, SIGNAL(routingChanged()), this, SLOT(onRoutingChanged()));
    QObject::connect(_widget, SIGNAL(inputLayerChanged(int)), this, SLOT(onInputLayerComboChanged(int)));
    QObject::connect(_widget, SIGNAL(outputLayerChanged(int)), this, SLOT(onOutputLayerComboChanged(int)));

    // Row 2 connections
    QObject::connect(_widget, SIGNAL(routing2Changed()), this, SLOT(onRouting2Changed()));
    QObject::connect(_widget, SIGNAL(inputLayer2Changed(int)), this, SLOT(onInputLayer2ComboChanged(int)));
    QObject::connect(_widget, SIGNAL(outputLayer2Changed(int)), this, SLOT(onOutputLayer2ComboChanged(int)));

    // New layer dialog
    QObject::connect(_widget, SIGNAL(newLayerRequested()), this, SLOT(onNewLayerRequested()));

    // Populate combos from the hidden knobs
    populateRow1Combos();
    populateRow2Combos();

    layout->addWidget(_widget);
}

void
KnobGuiShuffle::populateRow1Combos()
{
    KnobShufflePtr knob = _knob.lock();
    if (!knob) return;

    KnobHolder* holder = knob->getHolder();
    if (!holder) return;

    // Input layer combo (row 1)
    KnobIPtr ilKnob = holder->getKnobByName("inputLayer");
    if (ilKnob) {
        KnobChoice* ilChoice = dynamic_cast<KnobChoice*>(ilKnob.get());
        if (ilChoice) {
            std::vector<ChoiceOption> entries = ilChoice->getEntries_mt_safe();
            std::vector<std::string> names;
            for (size_t i = 0; i < entries.size(); ++i) names.push_back(entries[i].label);
            _widget->setInputLayerChoices(names, ilChoice->getValue());
        }
    }
    // Output layer combo (row 1)
    KnobIPtr olKnob = holder->getKnobByName("outputLayer");
    if (olKnob) {
        KnobChoice* olChoice = dynamic_cast<KnobChoice*>(olKnob.get());
        if (olChoice) {
            std::vector<ChoiceOption> entries = olChoice->getEntries_mt_safe();
            std::vector<std::string> names;
            for (size_t i = 0; i < entries.size(); ++i) names.push_back(entries[i].label);
            _widget->setOutputLayerChoices(names, olChoice->getValue());
        }
    }
}

void
KnobGuiShuffle::populateRow2Combos()
{
    KnobShufflePtr knob = _knob.lock();
    if (!knob) return;

    KnobHolder* holder = knob->getHolder();
    if (!holder) return;

    // Input layer combo (row 2)
    KnobIPtr il2Knob = holder->getKnobByName("inputLayer2");
    if (il2Knob) {
        KnobChoice* ilChoice = dynamic_cast<KnobChoice*>(il2Knob.get());
        if (ilChoice) {
            std::vector<ChoiceOption> entries = ilChoice->getEntries_mt_safe();
            std::vector<std::string> names;
            for (size_t i = 0; i < entries.size(); ++i) names.push_back(entries[i].label);
            _widget->setInputLayerChoices2(names, ilChoice->getValue());
        }
    }
    // Output layer combo (row 2)
    KnobIPtr ol2Knob = holder->getKnobByName("outputLayer2");
    if (ol2Knob) {
        KnobChoice* olChoice = dynamic_cast<KnobChoice*>(ol2Knob.get());
        if (olChoice) {
            std::vector<ChoiceOption> entries = olChoice->getEntries_mt_safe();
            std::vector<std::string> names;
            for (size_t i = 0; i < entries.size(); ++i) names.push_back(entries[i].label);
            _widget->setOutputLayerChoices2(names, olChoice->getValue());
        }
    }
}

// ==================== Row 1 slots ====================

void
KnobGuiShuffle::onRoutingChanged()
{
    KnobShufflePtr knob = _knob.lock();
    if (!knob) return;

    std::vector<int> routing = _widget->getRouting();

    // Update the R/G/B/A KnobInt knobs FIRST (before KnobShuffle, to avoid feedback loop)
    KnobHolder* holder = knob->getHolder();
    if (holder) {
        const char* knobNames[4] = {"outputR", "outputG", "outputB", "outputA"};
        for (int i = 0; i < 4; ++i) {
            KnobIPtr k = holder->getKnobByName(knobNames[i]);
            if (k) {
                KnobInt* intKnob = dynamic_cast<KnobInt*>(k.get());
                if (intKnob) {
                    int val = (i < (int)routing.size()) ? routing[i] : -1;
                    intKnob->setValue(val);
                }
            }
        }
    }

    // Then serialize routing to the KnobShuffle (triggers updateGUI which now reads correct values)
    std::string routingStr;
    for (size_t i = 0; i < routing.size(); ++i) {
        if (i > 0) routingStr += ",";
        routingStr += std::to_string(routing[i]);
    }
    knob->setValue(routingStr);
}

void
KnobGuiShuffle::onInputLayerComboChanged(int index)
{
    KnobShufflePtr knob = _knob.lock();
    if (!knob) return;
    KnobHolder* holder = knob->getHolder();
    if (!holder) return;

    KnobIPtr k = holder->getKnobByName("inputLayer");
    if (k) {
        KnobChoice* ck = dynamic_cast<KnobChoice*>(k.get());
        if (ck) ck->setValue(index);
    }
}

void
KnobGuiShuffle::onOutputLayerComboChanged(int index)
{
    KnobShufflePtr knob = _knob.lock();
    if (!knob) return;
    KnobHolder* holder = knob->getHolder();
    if (!holder) return;

    KnobIPtr k = holder->getKnobByName("outputLayer");
    if (k) {
        KnobChoice* ck = dynamic_cast<KnobChoice*>(k.get());
        if (ck) ck->setValue(index);
    }
}

// ==================== Row 2 slots ====================

void
KnobGuiShuffle::onRouting2Changed()
{
    KnobShufflePtr knob = _knob.lock();
    if (!knob) return;

    std::vector<int> routing2 = _widget->getRouting2();

    KnobHolder* holder = knob->getHolder();
    if (!holder) return;

    // Update the R2/G2/B2/A2 KnobInt knobs FIRST (before KnobShuffle)
    const char* knobNames2[4] = {"outputR2", "outputG2", "outputB2", "outputA2"};
    for (int i = 0; i < 4; ++i) {
        KnobIPtr k = holder->getKnobByName(knobNames2[i]);
        if (k) {
            KnobInt* intKnob = dynamic_cast<KnobInt*>(k.get());
            if (intKnob) {
                int val = (i < (int)routing2.size()) ? routing2[i] : -1;
                intKnob->setValue(val);
            }
        }
    }

    // Then serialize to KnobShuffle
    KnobIPtr r2Knob = holder->getKnobByName("shuffleRouting2");
    if (r2Knob) {
        KnobStringBase* strKnob = dynamic_cast<KnobStringBase*>(r2Knob.get());
        if (strKnob) {
            std::string routingStr;
            for (size_t i = 0; i < routing2.size(); ++i) {
                if (i > 0) routingStr += ",";
                routingStr += std::to_string(routing2[i]);
            }
            strKnob->setValue(routingStr);
        }
    }

}

void
KnobGuiShuffle::onInputLayer2ComboChanged(int index)
{
    KnobShufflePtr knob = _knob.lock();
    if (!knob) return;
    KnobHolder* holder = knob->getHolder();
    if (!holder) return;

    KnobIPtr k = holder->getKnobByName("inputLayer2");
    if (k) {
        KnobChoice* ck = dynamic_cast<KnobChoice*>(k.get());
        if (ck) ck->setValue(index);
    }
}

void
KnobGuiShuffle::onOutputLayer2ComboChanged(int index)
{
    KnobShufflePtr knob = _knob.lock();
    if (!knob) return;
    KnobHolder* holder = knob->getHolder();
    if (!holder) return;

    KnobIPtr k = holder->getKnobByName("outputLayer2");
    if (k) {
        KnobChoice* ck = dynamic_cast<KnobChoice*>(k.get());
        if (ck) ck->setValue(index);
    }
}

// ==================== New layer dialog ====================

void
KnobGuiShuffle::onNewLayerRequested()
{
    ImagePlaneDesc emptyPlane = ImagePlaneDesc::getNoneComponents();
    NewLayerDialog dialog(emptyPlane, _widget);
    if (dialog.exec() == QDialog::Accepted) {
        ImagePlaneDesc newComp = dialog.getComponents();
        if (newComp.getNumComponents() > 0) {
            // Store the layer locally in DevShuffle (not via addUserComponents)
            KnobShufflePtr knob = _knob.lock();
            if (knob && knob->getHolder()) {
                EffectInstance* effect = dynamic_cast<EffectInstance*>(knob->getHolder());
                if (effect) {
                    DevShuffle* devShuffle = dynamic_cast<DevShuffle*>(effect);
                    if (devShuffle) {
                        devShuffle->addUserLayer(newComp);
                        // addUserLayer already calls refreshLayerChoices/refreshLayerChoices2
                    }
                }
            }
        }
    }
}

// ==================== updateGUI ====================

void
KnobGuiShuffle::updateGUI(int /*dimension*/)
{
    if (!_widget) return;

    KnobShufflePtr knob = _knob.lock();
    if (!knob) return;

    KnobHolder* holder = knob->getHolder();
    if (!holder) return;

    // ---- Refresh Row 1 combos ----
    KnobIPtr ilKnob = holder->getKnobByName("inputLayer");
    if (ilKnob) {
        KnobChoice* ilChoice = dynamic_cast<KnobChoice*>(ilKnob.get());
        if (ilChoice) {
            std::vector<ChoiceOption> entries = ilChoice->getEntries_mt_safe();
            std::vector<std::string> names;
            for (size_t i = 0; i < entries.size(); ++i) names.push_back(entries[i].label);
            _widget->setInputLayerChoices(names, ilChoice->getValue());
        }
    }
    KnobIPtr olKnob = holder->getKnobByName("outputLayer");
    if (olKnob) {
        KnobChoice* olChoice = dynamic_cast<KnobChoice*>(olKnob.get());
        if (olChoice) {
            std::vector<ChoiceOption> entries = olChoice->getEntries_mt_safe();
            std::vector<std::string> names;
            for (size_t i = 0; i < entries.size(); ++i) names.push_back(entries[i].label);
            _widget->setOutputLayerChoices(names, olChoice->getValue());
        }
    }

    // ---- Refresh Row 2 combos ----
    KnobIPtr il2Knob = holder->getKnobByName("inputLayer2");
    if (il2Knob) {
        KnobChoice* ilChoice = dynamic_cast<KnobChoice*>(il2Knob.get());
        if (ilChoice) {
            std::vector<ChoiceOption> entries = ilChoice->getEntries_mt_safe();
            std::vector<std::string> names;
            for (size_t i = 0; i < entries.size(); ++i) names.push_back(entries[i].label);
            _widget->setInputLayerChoices2(names, ilChoice->getValue());
        }
    }
    KnobIPtr ol2Knob = holder->getKnobByName("outputLayer2");
    if (ol2Knob) {
        KnobChoice* olChoice = dynamic_cast<KnobChoice*>(ol2Knob.get());
        if (olChoice) {
            std::vector<ChoiceOption> entries = olChoice->getEntries_mt_safe();
            std::vector<std::string> names;
            for (size_t i = 0; i < entries.size(); ++i) names.push_back(entries[i].label);
            _widget->setOutputLayerChoices2(names, olChoice->getValue());
        }
    }

    // ---- Read Row 1 channel names from cached layers ----
    {
        EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
        DevShuffle* devShuffle = effect ? dynamic_cast<DevShuffle*>(effect) : NULL;

        KnobIPtr ilk = holder->getKnobByName("inputLayer");
        if (ilk) {
            KnobChoice* ilChoice = dynamic_cast<KnobChoice*>(ilk.get());
            if (ilChoice) {
                int layerIdx = ilChoice->getValue();
                std::vector<std::string> chanNames;

                if (devShuffle && layerIdx >= 0 && layerIdx < (int)devShuffle->getCachedLayers().size()) {
                    const ImagePlaneDesc& layer = devShuffle->getCachedLayers()[layerIdx];
                    std::string prefix = layer.getPlaneLabel();
                    if (prefix.empty()) {
                        prefix = layer.getPlaneID();
                    }
                    const std::vector<std::string>& channels = layer.getChannels();
                    for (size_t c = 0; c < channels.size(); ++c) {
                        chanNames.push_back(prefix + "." + channels[c]);
                    }
                }

                if (chanNames.empty()) {
                    chanNames.push_back("R");
                    chanNames.push_back("G");
                    chanNames.push_back("B");
                    chanNames.push_back("A");
                }
                _widget->setInputChannels(chanNames);
                _widget->setOutputChannels(chanNames);
            }
        }
    }

    // ---- Read Row 2 channel names from cached layers ----
    {
        EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
        DevShuffle* devShuffle = effect ? dynamic_cast<DevShuffle*>(effect) : NULL;

        KnobIPtr ilk = holder->getKnobByName("inputLayer2");
        if (ilk) {
            KnobChoice* ilChoice = dynamic_cast<KnobChoice*>(ilk.get());
            if (ilChoice) {
                int layerIdx = ilChoice->getValue();
                std::vector<std::string> chanNames;

                if (devShuffle && layerIdx >= 0 && layerIdx < (int)devShuffle->getCachedLayers2().size()) {
                    const ImagePlaneDesc& layer = devShuffle->getCachedLayers2()[layerIdx];
                    std::string prefix = layer.getPlaneLabel();
                    if (prefix.empty()) {
                        prefix = layer.getPlaneID();
                    }
                    const std::vector<std::string>& channels = layer.getChannels();
                    for (size_t c = 0; c < channels.size(); ++c) {
                        chanNames.push_back(prefix + "." + channels[c]);
                    }
                }

                if (chanNames.empty()) {
                    chanNames.push_back("R");
                    chanNames.push_back("G");
                    chanNames.push_back("B");
                    chanNames.push_back("A");
                }
                _widget->setInputChannels2(chanNames);
                _widget->setOutputChannels2(chanNames);
            }
        }
    }

    // ---- Read Row 1 routing from KnobInt values ----
    {
        const char* knobNames[4] = {"outputR", "outputG", "outputB", "outputA"};
        std::vector<int> routing;
        int numInputChans = (int)_widget->getRouting().size();
        if (numInputChans < 1) numInputChans = 4;
        for (int i = 0; i < numInputChans && i < 4; ++i) {
            KnobIPtr k = holder->getKnobByName(knobNames[i]);
            if (k) {
                KnobInt* ik = dynamic_cast<KnobInt*>(k.get());
                if (ik) {
                    routing.push_back(ik->getValue());
                }
            }
        }
        if (!routing.empty()) {
            _widget->setRouting(routing);
        }
    }

    // ---- Read Row 2 routing from KnobInt values ----
    {
        const char* knobNames[4] = {"outputR2", "outputG2", "outputB2", "outputA2"};
        std::vector<int> routing2;
        int numInputChans2 = (int)_widget->getRouting2().size();
        if (numInputChans2 < 1) numInputChans2 = 4;
        for (int i = 0; i < numInputChans2 && i < 4; ++i) {
            KnobIPtr k = holder->getKnobByName(knobNames[i]);
            if (k) {
                KnobInt* ik = dynamic_cast<KnobInt*>(k.get());
                if (ik) {
                    routing2.push_back(ik->getValue());
                }
            }
        }
        if (!routing2.empty()) {
            _widget->setRouting2(routing2);
        }
    }
}

void KnobGuiShuffle::_hide() { if (_widget) _widget->hide(); }
void KnobGuiShuffle::_show() { if (_widget) _widget->show(); }
void KnobGuiShuffle::setEnabled() {}

void
KnobGuiShuffle::removeSpecificGui()
{
    _widget = NULL;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_KnobGuiShuffle.cpp"
