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
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "PyNode.h"

#include <cassert>
#include <stdexcept>

#include "Engine/Node.h"
#include "Engine/KnobTypes.h"
#include "Engine/KnobFile.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/NodeGroup.h"
#include "Engine/PyRoto.h"
#include "Engine/PyTracker.h"
#include "Engine/TimeLine.h"
#include "Engine/Hash64.h"

NATRON_NAMESPACE_ENTER
NATRON_PYTHON_NAMESPACE_ENTER

ImageLayer::ImageLayer(const QString& layerName,
                       const QString& componentsPrettyName,
                       const QStringList& componentsName)
    : _layerName(layerName)
    , _componentsPrettyName(componentsPrettyName)
    , _componentsName(componentsName)
    , _comps()
{
    std::vector<std::string> channels( componentsName.size() );
    int i = 0;

    for (QStringList::const_iterator it = componentsName.begin(); it != componentsName.end(); ++it, ++i) {
        channels[i] = it->toStdString();
    }
    _comps.reset( new ImagePlaneDesc(layerName.toStdString(), layerName.toStdString(), componentsPrettyName.toStdString(), channels) );
}

ImageLayer::ImageLayer(const ImagePlaneDesc& comps)
    : _layerName( QString::fromUtf8( comps.getPlaneLabel().c_str() ) )
    , _componentsPrettyName( QString::fromUtf8( comps.getChannelsLabel().c_str() ) )
{
    const std::vector<std::string>& channels = comps.getChannels();

    for (std::size_t i = 0; i < channels.size(); ++i) {
        _componentsName.push_back( QString::fromUtf8( channels[i].c_str() ) );
    }
    _comps.reset( new ImagePlaneDesc(comps) );
}

int
ImageLayer::getHash(const ImageLayer& layer)
{
    Hash64 h;

    Hash64_appendQString( &h, QString::fromUtf8( layer._comps->getChannelsLabel().c_str() ) );
    const std::vector<std::string>& comps = layer._comps->getChannels();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        Hash64_appendQString( &h, QString::fromUtf8( comps[i].c_str() ) );
    }

    return (int)h.value();
}

bool
ImageLayer::isColorPlane() const
{
    return _comps->isColorPlane();
}

int
ImageLayer::getNumComponents() const
{
    return _comps->getNumComponents();
}

const QString&
ImageLayer::getLayerName() const
{
    return _layerName;
}

const QStringList&
ImageLayer::getComponentsNames() const
{
    return _componentsName;
}

const QString&
ImageLayer::getComponentsPrettyName() const
{
    return _componentsPrettyName;
}

bool
ImageLayer::operator==(const ImageLayer& other) const
{
    return _comps == other._comps;
}

bool
ImageLayer::operator<(const ImageLayer& other) const
{
    return _comps < other._comps;
}

/*
 * These are default presets image components
 */
ImageLayer
ImageLayer::getNoneComponents()
{
    return ImageLayer( ImagePlaneDesc::getNoneComponents() );
}

ImageLayer
ImageLayer::getRGBAComponents()
{
    return ImageLayer( ImagePlaneDesc::getRGBAComponents() );
}

ImageLayer
ImageLayer::getRGBComponents()
{
    return ImageLayer( ImagePlaneDesc::getRGBComponents() );
}

ImageLayer
ImageLayer::getAlphaComponents()
{
    return ImageLayer( ImagePlaneDesc::getAlphaComponents() );
}

ImageLayer
ImageLayer::getBackwardMotionComponents()
{
    return ImageLayer( ImagePlaneDesc::getBackwardMotionComponents() );
}

ImageLayer
ImageLayer::getForwardMotionComponents()
{
    return ImageLayer( ImagePlaneDesc::getForwardMotionComponents() );
}

ImageLayer
ImageLayer::getDisparityLeftComponents()
{
    return ImageLayer( ImagePlaneDesc::getDisparityLeftComponents() );
}

ImageLayer
ImageLayer::getDisparityRightComponents()
{
    return ImageLayer( ImagePlaneDesc::getDisparityRightComponents() );
}

UserParamHolder::UserParamHolder()
    : _holder(0)
{
}

UserParamHolder::UserParamHolder(KnobHolder* holder)
    : _holder(holder)
{
}

void
UserParamHolder::setHolder(KnobHolder* holder)
{
    assert(!_holder);
    _holder = holder;
}

KnobHolder*
UserParamHolder::holder() const
{
    KnobHolder* h = _holder.data();
    if (h) {
        return h;
    }
    // The wrapper is generated with exception-handling="yes" (typesystem_engine.xml):
    // shiboken catches this and raises it as a Python RuntimeError.
    throw std::runtime_error("This node has been destroyed (Effect.destroy()); its parameters can no longer be edited");
}

Effect::Effect(const NodePtr& node)
    : Group()
    , UserParamHolder(node ? node->getEffectInstance().get() : 0)
    , _node(node)
{
    if (node) {
        NodeGroupPtr grp;
        if ( node->getEffectInstance() ) {
            grp = std::dynamic_pointer_cast<NodeGroup>( node->getEffectInstance()->shared_from_this() );
            init( std::dynamic_pointer_cast<NodeCollection>(grp) );
        }
    }
}

Effect::~Effect()
{
}

NodePtr
Effect::getInternalNode() const
{
    return _node.lock();
}

NodePtr
Effect::liveNode() const
{
    NodePtr node = _node.lock();
    if ( node && node->getEffectInstance() ) {
        return node;
    }
    // The wrapper is generated with exception-handling="yes" (typesystem_engine.xml):
    // shiboken catches this and raises it as a Python RuntimeError.
    throw std::runtime_error("This node has been destroyed (Effect.destroy()) and can no longer be used");
}

bool
Effect::isReaderNode()
{
    NodePtr n = liveNode();

    if (!n) {
        return false;
    }

    return n->getEffectInstance()->isReader();
}

bool
Effect::isWriterNode()
{
    NodePtr n = liveNode();

    if (!n) {
        return false;
    }

    return n->getEffectInstance()->isWriter();
}

bool
Effect::isOutputNode()
{
    NodePtr n = liveNode();

    if (!n) {
        return false;
    }

    return n->getEffectInstance()->isOutput();
}

void
Effect::destroy(bool autoReconnect)
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->destroyNode(false, autoReconnect);
}

int
Effect::getMaxInputCount() const
{
    NodePtr node = liveNode();

    if (!node) {
        return 0;
    }

    return node->getNInputs();
}

bool
Effect::canConnectInput(int inputNumber,
                        const Effect* node) const
{
    if (!node) {
        return false;
    }

    NodePtr thisNode = liveNode();
    if (!thisNode) {
        return false;
    }
    NodePtr otherNode = node->liveNode();
    if (!otherNode) {
        return false;
    }
    Node::CanConnectInputReturnValue ret = thisNode->canConnectInput(otherNode, inputNumber);

    return ret == Node::eCanConnectInput_ok ||
           ret == Node::eCanConnectInput_differentFPS ||
           ret == Node::eCanConnectInput_differentPars;
}

bool
Effect::connectInput(int inputNumber,
                     const Effect* input)
{
    if ( canConnectInput(inputNumber, input) ) {
        return getInternalNode()->connectInput(input->getInternalNode(), inputNumber);  // both checked live above
    } else {
        return false;
    }
}

void
Effect::disconnectInput(int inputNumber)
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->disconnectInput(inputNumber);
}

Effect*
Effect::getInput(int inputNumber) const
{
    NodePtr thisNode = liveNode();

    if (!thisNode) {
        return NULL;
    }
    NodePtr node = thisNode->getRealInput(inputNumber);

    if (node) {
        return new Effect(node);
    }

    return NULL;
}

Effect*
Effect::getInput(const QString& inputLabel) const
{
    NodePtr node = liveNode();
    if (!node) {
        return 0;
    }
    int maxInputs = node->getNInputs();
    for (int i = 0; i < maxInputs; ++i) {
        if (QString::fromUtf8(node->getInputLabel(i).c_str()) == inputLabel) {
            NodePtr ret = node->getRealInput(i);
            if (!ret) {
                return 0;
            }

            return new Effect(ret);
        }
    }
    return 0;
}

QString
Effect::getScriptName() const
{
    NodePtr node = liveNode();

    if (!node) {
        return QString();
    }

    return QString::fromUtf8( node->getScriptName_mt_safe().c_str() );
}

bool
Effect::setScriptName(const QString& scriptName)
{
    NodePtr node = liveNode();

    if (!node) {
        return false;
    }
    try {
        node->setScriptName( scriptName.toStdString() );
    } catch (...) {
        return false;
    }

    return true;
}

QString
Effect::getLabel() const
{
    NodePtr node = liveNode();

    if (!node) {
        return QString();
    }

    return QString::fromUtf8( node->getLabel_mt_safe().c_str() );
}

void
Effect::setLabel(const QString& name)
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->setLabel( name.toStdString() );
}

QString
Effect::getInputLabel(int inputNumber)
{
    NodePtr node = liveNode();

    if (!node) {
        return QString();
    }
    try {
        return QString::fromUtf8( node->getInputLabel(inputNumber).c_str() );
    } catch (const std::exception& e) {
        node->getApp()->appendToScriptEditor( e.what() );
    }

    return QString();
}

QString
Effect::getPluginID() const
{
    NodePtr node = liveNode();

    if (!node) {
        return QString();
    }

    return QString::fromUtf8( node->getPluginID().c_str() );
}

Param*
Effect::createParamWrapperForKnob(const KnobIPtr& knob)
{
    int dims = knob->getDimension();
    KnobIntPtr isInt = std::dynamic_pointer_cast<KnobInt>(knob);
    KnobDoublePtr isDouble = std::dynamic_pointer_cast<KnobDouble>(knob);
    KnobBoolPtr isBool = std::dynamic_pointer_cast<KnobBool>(knob);
    KnobChoicePtr isChoice = std::dynamic_pointer_cast<KnobChoice>(knob);
    KnobColorPtr isColor = std::dynamic_pointer_cast<KnobColor>(knob);
    KnobStringPtr isString = std::dynamic_pointer_cast<KnobString>(knob);
    KnobFilePtr isFile = std::dynamic_pointer_cast<KnobFile>(knob);
    KnobOutputFilePtr isOutputFile = std::dynamic_pointer_cast<KnobOutputFile>(knob);
    KnobPathPtr isPath = std::dynamic_pointer_cast<KnobPath>(knob);
    KnobButtonPtr isButton = std::dynamic_pointer_cast<KnobButton>(knob);
    KnobGroupPtr isGroup = std::dynamic_pointer_cast<KnobGroup>(knob);
    KnobPagePtr isPage = std::dynamic_pointer_cast<KnobPage>(knob);
    KnobParametricPtr isParametric = std::dynamic_pointer_cast<KnobParametric>(knob);
    KnobSeparatorPtr isSep = std::dynamic_pointer_cast<KnobSeparator>(knob);

    if (isInt) {
        switch (dims) {
        case 1:

            return new IntParam(isInt);
        case 2:

            return new Int2DParam(isInt);
        case 3:

            return new Int3DParam(isInt);
        default:
            break;
        }
    } else if (isDouble) {
        switch (dims) {
        case 1:

            return new DoubleParam(isDouble);
        case 2:

            return new Double2DParam(isDouble);
        case 3:

            return new Double3DParam(isDouble);
        default:
            break;
        }
    } else if (isBool) {
        return new BooleanParam(isBool);
    } else if (isChoice) {
        return new ChoiceParam(isChoice);
    } else if (isColor) {
        return new ColorParam(isColor);
    } else if (isString) {
        return new StringParam(isString);
    } else if (isFile) {
        return new FileParam(isFile);
    } else if (isOutputFile) {
        return new OutputFileParam(isOutputFile);
    } else if (isPath) {
        return new PathParam(isPath);
    } else if (isGroup) {
        return new GroupParam(isGroup);
    } else if (isPage) {
        return new PageParam(isPage);
    } else if (isParametric) {
        return new ParametricParam(isParametric);
    } else if (isButton) {
        return new ButtonParam(isButton);
    } else if (isSep) {
        return new SeparatorParam(isSep);
    }

    return NULL;
} // Effect::createParamWrapperForKnob

std::list<Param*>
Effect::getParams() const
{
    std::list<Param*> ret;
    NodePtr node = liveNode();

    if (!node) {
        return ret;
    }
    const KnobsVec& knobs = node->getKnobs();

    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        Param* p = createParamWrapperForKnob(*it);
        if (p) {
            ret.push_back(p);
        }
    }

    return ret;
}

Param*
Effect::getParam(const QString& name) const
{
    NodePtr node = liveNode();

    if (!node) {
        return NULL;
    }

    QString fallbackSearchName;
    if (node->getApp()->isCreatingPythonGroup()) {
        // Before Natron 2.2.3, all dynamic choice parameters for multiplane had a string parameter.
        // The string parameter had the same name as the choice parameter plus "Choice" appended.
        // If we found such a parameter, retrieve the string from it.
        QString str = QString::fromUtf8("Choice");
        if (name.endsWith(str)) {
            fallbackSearchName = name.mid(0, name.size() - str.size());
        }
    }
    KnobIPtr knob = node->getKnobByName( name.toStdString() );

    if (knob) {
        return createParamWrapperForKnob(knob);
    } else {
        if (!fallbackSearchName.isEmpty()) {
            return getParam(fallbackSearchName);
        }
        return NULL;
    }
}

int
Effect::getCurrentTime() const
{
    NodePtr node = liveNode();

    if (!node) {
        return 0;
    }

    return node->getEffectInstance()->getCurrentTime();
}

void
Effect::setPosition(double x,
                    double y)
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->setPosition(x, y);
}

void
Effect::getPosition(double* x,
                    double* y) const
{
    *x = *y = 0.;
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->getPosition(x, y);
}

void
Effect::setSize(double w,
                double h)
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->setSize(w, h);
}

void
Effect::getSize(double* w,
                double* h) const
{
    *w = *h = 0.;
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->getSize(w, h);
}

void
Effect::getColor(double* r,
                 double *g,
                 double* b) const
{
    *r = *g = *b = 0.;
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    bool hasColor = node->getColor(r, g, b);
    Q_UNUSED(hasColor);
}

void
Effect::setColor(double r,
                 double g,
                 double b)
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->setColor(r, g, b);
}

bool
Effect::isNodeSelected() const
{
    NodePtr node = liveNode();

    if (!node) {
        return false;
    }

    return node->isUserSelected();
}

void
Effect::beginChanges()
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->getEffectInstance()->beginChanges();
    node->beginInputEdition();
}

void
Effect::endChanges()
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    node->getEffectInstance()->endChanges();
    node->endInputEdition(true);
}

IntParam*
UserParamHolder::createIntParam(const QString& name,
                                const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobIntPtr knob = h->createIntKnob(name.toStdString(), label.toStdString(), 1);

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new IntParam(knob);
    } else {
        return 0;
    }
}

Int2DParam*
UserParamHolder::createInt2DParam(const QString& name,
                                  const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobIntPtr knob = h->createIntKnob(name.toStdString(), label.toStdString(), 2);

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new Int2DParam(knob);
    } else {
        return 0;
    }
}

Int3DParam*
UserParamHolder::createInt3DParam(const QString& name,
                                  const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobIntPtr knob = h->createIntKnob(name.toStdString(), label.toStdString(), 3);

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new Int3DParam(knob);
    } else {
        return 0;
    }
}

DoubleParam*
UserParamHolder::createDoubleParam(const QString& name,
                                   const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobDoublePtr knob = h->createDoubleKnob(name.toStdString(), label.toStdString(), 1);

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new DoubleParam(knob);
    } else {
        return 0;
    }
}

Double2DParam*
UserParamHolder::createDouble2DParam(const QString& name,
                                     const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobDoublePtr knob = h->createDoubleKnob(name.toStdString(), label.toStdString(), 2);

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new Double2DParam(knob);
    } else {
        return 0;
    }
}

Double3DParam*
UserParamHolder::createDouble3DParam(const QString& name,
                                     const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobDoublePtr knob = h->createDoubleKnob(name.toStdString(), label.toStdString(), 3);

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new Double3DParam(knob);
    } else {
        return 0;
    }
}

BooleanParam*
UserParamHolder::createBooleanParam(const QString& name,
                                    const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobBoolPtr knob = h->createBoolKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new BooleanParam(knob);
    } else {
        return 0;
    }
}

ChoiceParam*
UserParamHolder::createChoiceParam(const QString& name,
                                   const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobChoicePtr knob = h->createChoiceKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ChoiceParam(knob);
    } else {
        return 0;
    }
}

ColorParam*
UserParamHolder::createColorParam(const QString& name,
                                  const QString& label,
                                  bool useAlpha)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobColorPtr knob = h->createColorKnob(name.toStdString(), label.toStdString(), useAlpha ? 4 : 3);

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ColorParam(knob);
    } else {
        return 0;
    }
}

StringParam*
UserParamHolder::createStringParam(const QString& name,
                                   const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobStringPtr knob = h->createStringKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new StringParam(knob);
    } else {
        return 0;
    }
}

FileParam*
UserParamHolder::createFileParam(const QString& name,
                                 const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobFilePtr knob = h->createFileKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new FileParam(knob);
    } else {
        return 0;
    }
}

OutputFileParam*
UserParamHolder::createOutputFileParam(const QString& name,
                                       const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobOutputFilePtr knob = h->createOuptutFileKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new OutputFileParam(knob);
    } else {
        return 0;
    }
}

PathParam*
UserParamHolder::createPathParam(const QString& name,
                                 const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobPathPtr knob = h->createPathKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new PathParam(knob);
    } else {
        return 0;
    }
}

ButtonParam*
UserParamHolder::createButtonParam(const QString& name,
                                   const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobButtonPtr knob = h->createButtonKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ButtonParam(knob);
    } else {
        return 0;
    }
}

SeparatorParam*
UserParamHolder::createSeparatorParam(const QString& name,
                                      const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobSeparatorPtr knob = h->createSeparatorKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new SeparatorParam(knob);
    } else {
        return 0;
    }
}

GroupParam*
UserParamHolder::createGroupParam(const QString& name,
                                  const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobGroupPtr knob = h->createGroupKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new GroupParam(knob);
    } else {
        return 0;
    }
}

PageParam*
UserParamHolder::createPageParam(const QString& name,
                                 const QString& label)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobPagePtr knob = h->createPageKnob( name.toStdString(), label.toStdString() );
    if (knob) {
        return new PageParam(knob);
    } else {
        return 0;
    }
}

ParametricParam*
UserParamHolder::createParametricParam(const QString& name,
                                       const QString& label,
                                       int nbCurves)
{
    KnobHolder* h = holder();
    if (!h) {
        return 0;
    }
    KnobParametricPtr knob = h->createParametricKnob(name.toStdString(), label.toStdString(), nbCurves);

    if (knob) {
        KnobPagePtr userPage = h->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ParametricParam(knob);
    } else {
        return 0;
    }
}

bool
UserParamHolder::removeParam(Param* param)
{
    KnobHolder* h = holder();
    if (!h) {
        return false;
    }
    if (!param) {
        return false;
    }
    if ( !param->getInternalKnob() ) {
        return false;
    }
    if ( !param->getInternalKnob()->isUserKnob() ) {
        return false;
    }

    h->deleteKnob(param->getInternalKnob().get(), true);

    return true;
}

PageParam*
Effect::getUserPageParam() const
{
    NodePtr node = liveNode();

    if (!node) {
        return NULL;
    }
    KnobPagePtr page = node->getEffectInstance()->getOrCreateUserPageKnob();

    assert(page);

    return new PageParam(page);
}

void
UserParamHolder::refreshUserParamsGUI()
{
    KnobHolder* h = holder();
    if (!h) {
        return;
    }
    h->recreateUserKnobs(false);
}


Roto*
Effect::getRotoContext() const
{
    NodePtr node = liveNode();

    if (!node) {
        return 0;
    }
    RotoContextPtr roto = node->getRotoContext();

    if (roto) {
        return new Roto(roto);
    }

    return 0;
}

Tracker*
Effect::getTrackerContext() const
{
    NodePtr node = liveNode();

    if (!node) {
        return 0;
    }
    TrackerContextPtr t = node->getTrackerContext();

    if (t) {
        return new Tracker(t);
    }

    return 0;
}

RectD
Effect::getRegionOfDefinition(double time,
                              int view) const
{
    RectD rod;
    NodePtr node = liveNode();

    if (!node) {
        return rod;
    }
    U64 hash = node->getHashValue();
    bool isProject;
    StatusEnum stat = node->getEffectInstance()->getRegionOfDefinition_public(hash, time, RenderScale::identity, ViewIdx(view), &rod, &isProject);
    if (stat != eStatusOK) {
        return RectD();
    }

    return rod;
}

void
Effect::setSubGraphEditable(bool editable)
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }
    NodeGroup* isGroup = node->isEffectGroup();
    if (isGroup) {
        isGroup->setSubGraphEditable(editable);
    }
}

bool
Effect::addUserPlane(const QString& planeName,
                     const QStringList& channels)
{
    if ( planeName.isEmpty() ||
         ( channels.size() < 1) ||
         ( channels.size() > 4) ) {
        return false;
    }
    std::string compsGlobal;
    std::vector<std::string> chans( channels.size() );
    int i = 0;
    for (QStringList::const_iterator it = channels.begin(); it != channels.end(); ++it, ++i) {
        std::string c = it->toStdString();
        compsGlobal.append(c);
        chans[i] = c;
    }
    ImagePlaneDesc comp(planeName.toStdString(),planeName.toStdString(), compsGlobal, chans);
    NodePtr node = liveNode();

    if (!node) {
        return false;
    }

    return node->addUserComponents(comp);
}

std::list<ImageLayer>
Effect::getAvailableLayers(int inputNb) const
{
    std::list<ImageLayer> ret;
    NodePtr node = liveNode();

    if (!node) {
        return ret;
    }
    double time(node->getApp()->getTimeLine()->currentFrame());

    std::list<ImagePlaneDesc> availComps;
    node->getEffectInstance()->getAvailableLayers(time, ViewIdx(0), inputNb, &availComps);
    for (std::list<ImagePlaneDesc>::iterator it = availComps.begin(); it != availComps.end(); ++it) {
        ret.push_back(ImageLayer(*it));
    }


    return ret;
}

RectI
Effect::getOutputFormat() const
{
    NodePtr node = liveNode();

    if (!node) {
        return RectI();
    }

    return node->getEffectInstance()->getOutputFormat();
}

double
Effect::getFrameRate() const
{
    NodePtr node = liveNode();

    if (!node) {
        return 24.;
    }

    return node->getEffectInstance()->getFrameRate();
}

double
Effect::getPixelAspectRatio() const
{
    NodePtr node = liveNode();

    if (!node) {
        return 1.;
    }

    return node->getEffectInstance()->getAspectRatio(-1);
}

ImageBitDepthEnum
Effect::getBitDepth() const
{
    NodePtr node = liveNode();

    if (!node) {
        return eImageBitDepthFloat;
    }

    return node->getEffectInstance()->getBitDepth(-1);
}

ImagePremultiplicationEnum
Effect::getPremult() const
{
    NodePtr node = liveNode();

    if (!node) {
        return eImagePremultiplicationPremultiplied;
    }

    return node->getEffectInstance()->getPremult();
}

void
Effect::setPagesOrder(const QStringList& pages)
{
    NodePtr node = liveNode();

    if (!node) {
        return;
    }

    std::list<std::string> order;
    for (QStringList::const_iterator it = pages.begin(); it != pages.end(); ++it) {
        order.push_back( it->toStdString() );
    }
    node->setPagesOrder(order);
}

NATRON_PYTHON_NAMESPACE_EXIT
NATRON_NAMESPACE_EXIT
