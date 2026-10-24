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

#include "NodeGraphUndoRedo.h"

#include <algorithm> // min, max
#include <limits>
#include <map>
#include <set>
#include <vector>
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QDebug>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/CreateNodeArgs.h"
#include "Engine/GroupInput.h"
#include "Engine/GroupOutput.h"
#include "Engine/Node.h"
#include "Engine/NodeSerialization.h"
#include "Engine/Project.h"
#include "Engine/RotoLayer.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"

#include "Gui/BackdropGui.h"
#include "Gui/NodeClipBoard.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeGraph.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/Edge.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/MultiInstancePanel.h"


NATRON_NAMESPACE_ENTER

MoveMultipleNodesCommand::MoveMultipleNodesCommand(const NodesGuiList & nodes,
                                                   double dx,
                                                   double dy,
                                                   QUndoCommand *parent)
    : QUndoCommand(parent)
    , _firstRedoCalled(false)
    , _nodes(nodes)
    , _dx(dx)
    , _dy(dy)
{
    assert( !nodes.empty() );
}

void
MoveMultipleNodesCommand::move(double dx,
                               double dy)
{
    for (NodesGuiList::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        QPointF pos = (*it)->getPos_mt_safe();
        (*it)->setPosition(pos.x() + dx, pos.y() + dy);
    }
    setText( tr("Move node(s)") );
}

void
MoveMultipleNodesCommand::undo()
{
    move(-_dx, -_dy);
}

void
MoveMultipleNodesCommand::redo()
{
    if (_firstRedoCalled) {
        move(_dx, _dy);
    }
    _firstRedoCalled = true;
}

AddMultipleNodesCommand::AddMultipleNodesCommand(NodeGraph* graph,
                                                 const std::list<NodeGuiPtr> & nodes,
                                                 QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
    , _graph(graph)
    , _firstRedoCalled(false)
    , _isUndone(false)
{
    for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        _nodes.push_back(*it);
    }
}

AddMultipleNodesCommand::AddMultipleNodesCommand(NodeGraph* graph,
                                                 const NodeGuiPtr & node,
                                                 QUndoCommand* parent)
    : QUndoCommand(parent)
    , _nodes()
    , _graph(graph)
    , _firstRedoCalled(false)
    , _isUndone(false)
{
    _nodes.push_back(node);
}

AddMultipleNodesCommand::~AddMultipleNodesCommand()
{
    if (_isUndone) {
        for (std::list<NodeGuiWPtr>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
            NodeGuiPtr node = it->lock();
            if (node) {
                node->getNode()->destroyNode(false, false);
            }
        }
    }
    setText( tr("Add node") );
}

void
AddMultipleNodesCommand::undo()
{
    _isUndone = true;
    std::list<ViewerInstance*> viewersToRefresh;


    for (std::list<NodeGuiWPtr>::const_iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        NodeGuiPtr node = it->lock();
        // Destroyed since (Effect.destroy() from Python): nothing left to undo.
        if ( !node || !node->getNode() || !node->getNode()->getEffectInstance() ) {
            continue;
        }
        std::list<ViewerInstance* > viewers;
        node->getNode()->hasViewersConnected(&viewers);
        for (std::list<ViewerInstance* >::iterator it2 = viewers.begin(); it2 != viewers.end(); ++it2) {
            std::list<ViewerInstance*>::iterator foundViewer = std::find(viewersToRefresh.begin(), viewersToRefresh.end(), *it2);
            if ( foundViewer == viewersToRefresh.end() ) {
                viewersToRefresh.push_back(*it2);
            }
        }
        node->getNode()->deactivate(NodesList(), //outputs to disconnect
                                    true, //disconnect all nodes, disregarding the first parameter.
                                    true, //reconnect outputs to inputs of this node?
                                    true, //hide nodeGui?
                                    false); // triggerRender
    }

    _graph->clearSelection();

    _graph->getGui()->getApp()->triggerAutoSave();

    for (std::list<ViewerInstance* >::iterator it = viewersToRefresh.begin(); it != viewersToRefresh.end(); ++it) {
        (*it)->renderCurrentFrame(true);
    }
    _graph->updateNavigator();
}

void
AddMultipleNodesCommand::redo()
{
    _isUndone = false;
    std::list<ViewerInstance*> viewersToRefresh;
    std::list<NodeGuiPtr> nodes;
    for (std::list<NodeGuiWPtr>::const_iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        NodeGuiPtr node = it->lock();
        // Destroyed since (Effect.destroy() from Python): nothing left to redo.
        if ( !node || !node->getNode() || !node->getNode()->getEffectInstance() ) {
            continue;
        }
        nodes.push_back(node);
    }
    if ( nodes.empty() ) {
        return;
    }
    if (_firstRedoCalled) {
        for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            (*it)->getNode()->activate(NodesList(), //inputs to restore
                                       true, //restore all inputs ?
                                       false); //triggerRender
        }
    }


    if ( (nodes.size() != 1) || !nodes.front()->getNode()->isEffectGroup() ) {
        _graph->setSelection(nodes);
    }

    _graph->getGui()->getApp()->recheckInvalidExpressions();
    _graph->getGui()->getApp()->triggerAutoSave();

    for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        std::list<ViewerInstance* > viewers;
        (*it)->getNode()->hasViewersConnected(&viewers);
        for (std::list<ViewerInstance* >::iterator it2 = viewers.begin(); it2 != viewers.end(); ++it2) {
            std::list<ViewerInstance*>::iterator foundViewer = std::find(viewersToRefresh.begin(), viewersToRefresh.end(), *it2);
            if ( foundViewer == viewersToRefresh.end() ) {
                viewersToRefresh.push_back(*it2);
            }
        }
    }


    for (std::list<ViewerInstance* >::iterator it = viewersToRefresh.begin(); it != viewersToRefresh.end(); ++it) {
        if ( (*it)->getUiContext() ) {
            (*it)->renderCurrentFrame(true);
        }
    }

    _firstRedoCalled = true;
    _graph->updateNavigator();
}

RemoveMultipleNodesCommand::RemoveMultipleNodesCommand(NodeGraph* graph,
                                                       const std::list<NodeGuiPtr> & nodes,
                                                       QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
    , _graph(graph)
    , _isRedone(false)
{
    for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodeToRemove n;
        n.node = *it;

        ///find all outputs to restore
        const NodesWList & outputs = (*it)->getNode()->getGuiOutputs();
        for (NodesWList::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
            NodePtr output = it2->lock();
            if (!output) {
                continue;
            }
            bool restore = true;
            for (std::list<NodeGuiPtr> ::const_iterator it3 = nodes.begin(); it3 != nodes.end(); ++it3) {
                if ( (*it3)->getNode() == output ) {
                    ///we found the output in the selection, don't restore it
                    restore = false;
                }
            }
            if (restore) {
                n.outputsToRestore.push_back(output);
            }
        }
        _nodes.push_back(n);
    }
    setText( tr("Remove node(s)") );
}

RemoveMultipleNodesCommand::~RemoveMultipleNodesCommand()
{
    if (_isRedone) {
        for (std::list<NodeToRemove>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
            NodeGuiPtr n = it->node.lock();
            if (n) {
                n->getNode()->destroyNode(false, false);
            }
        }
    }
}

void
RemoveMultipleNodesCommand::undo()
{
    std::list<ViewerInstance*> viewersToRefresh;
    std::list<SequenceTime> allKeysToAdd;
    std::list<NodeToRemove>::iterator next = _nodes.begin();

    if ( next != _nodes.end() ) {
        ++next;
    }
    for (std::list<NodeToRemove>::iterator it = _nodes.begin();
         it != _nodes.end();
         ++it) {
        NodeGuiPtr node = it->node.lock();
        NodesList outputsToRestore;
        for (NodesWList::const_iterator it2 = it->outputsToRestore.begin(); it2 != it->outputsToRestore.end(); ++it2) {
            NodePtr output = it2->lock();
            if (output) {
                outputsToRestore.push_back(output);
            }
        }

        node->getNode()->activate(outputsToRestore, false, false);
        if ( node->isSettingsPanelVisible() ) {
            node->getNode()->showKeyframesOnTimeline( next == _nodes.end() );
        }
        std::list<ViewerInstance* > viewers;
        node->getNode()->hasViewersConnected(&viewers);
        for (std::list<ViewerInstance* >::iterator it2 = viewers.begin(); it2 != viewers.end(); ++it2) {
            std::list<ViewerInstance*>::iterator foundViewer = std::find(viewersToRefresh.begin(), viewersToRefresh.end(), *it2);
            if ( foundViewer == viewersToRefresh.end() ) {
                viewersToRefresh.push_back(*it2);
            }
        }

        // increment for next iteration
        if ( next != _nodes.end() ) {
            ++next;
        }
    } // for(it)
    for (std::list<ViewerInstance* >::iterator it = viewersToRefresh.begin(); it != viewersToRefresh.end(); ++it) {
        (*it)->renderCurrentFrame(true);
    }
    _graph->getGui()->getApp()->triggerAutoSave();
    _graph->getGui()->getApp()->redrawAllViewers();
    _graph->updateNavigator();

    _isRedone = false;
    _graph->scene()->update();
    _graph->updateNavigator();
} // RemoveMultipleNodesCommand::undo

void
RemoveMultipleNodesCommand::redo()
{
    _isRedone = true;

    std::list<ViewerInstance*> viewersToRefresh;
    std::list<NodeToRemove>::iterator next = _nodes.begin();
    if ( next != _nodes.end() ) {
        ++next;
    }
    for (std::list<NodeToRemove>::iterator it = _nodes.begin();
         it != _nodes.end();
         ++it) {
        NodeGuiPtr node = it->node.lock();
        ///Make a copy before calling deactivate which will modify the list
        NodesWList outputs = node->getNode()->getGuiOutputs();
        std::list<ViewerInstance* > viewers;
        node->getNode()->hasViewersConnected(&viewers);
        for (std::list<ViewerInstance* >::iterator it2 = viewers.begin(); it2 != viewers.end(); ++it2) {
            std::list<ViewerInstance*>::iterator foundViewer = std::find(viewersToRefresh.begin(), viewersToRefresh.end(), *it2);
            if ( foundViewer == viewersToRefresh.end() ) {
                viewersToRefresh.push_back(*it2);
            }
        }

        NodesList outputsToRestore;
        for (NodesWList::const_iterator it2 = it->outputsToRestore.begin(); it2 != it->outputsToRestore.end(); ++it2) {
            NodePtr output = it2->lock();
            if (output) {
                outputsToRestore.push_back(output);
            }
        }

        node->getNode()->deactivate(outputsToRestore, false, _nodes.size() == 1, true, false);


        if (_nodes.size() == 1) {
            ///If we're deleting a single node and there's a viewer in output,reconnect the viewer to another connected input it has
            for (NodesWList::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
                NodePtr output = it2->lock();

                if (!output) {
                    continue;
                }

                ///the output must be in the outputs to restore
                NodesList::const_iterator found = std::find(outputsToRestore.begin(), outputsToRestore.end(), output);

                if ( found != outputsToRestore.end() ) {
                    InspectorNode* inspector = dynamic_cast<InspectorNode*>( output.get() );
                    ///if the node is an inspector, when disconnecting the active input just activate another input instead
                    if (inspector) {
                        const std::vector<NodeWPtr> & inputs = inspector->getGuiInputs();
                        ///set as active input the first non null input
                        for (std::size_t i = 0; i < inputs.size(); ++i) {
                            NodePtr input = inputs[i].lock();
                            if (input) {
                                inspector->setActiveInputAndRefresh(i, true);
                                ///make sure we don't refresh it a second time
                                std::list<ViewerInstance*>::iterator foundViewer =
                                    std::find( viewersToRefresh.begin(), viewersToRefresh.end(), inspector->isEffectViewer() );
                                if ( foundViewer != viewersToRefresh.end() ) {
                                    viewersToRefresh.erase(foundViewer);
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        if ( node->isSettingsPanelVisible() ) {
            node->getNode()->hideKeyframesFromTimeline( next == _nodes.end() );
        }

        // increment for next iteration
        if ( next != _nodes.end() ) {
            ++next;
        }
    } // for(it)

    for (std::list<ViewerInstance* >::iterator it = viewersToRefresh.begin(); it != viewersToRefresh.end(); ++it) {
        (*it)->renderCurrentFrame(true);
    }

    _graph->getGui()->getApp()->triggerAutoSave();
    _graph->getGui()->getApp()->redrawAllViewers();
    _graph->updateNavigator();

    _graph->scene()->update();
    _graph->updateNavigator();
} // redo

ConnectCommand::ConnectCommand(NodeGraph* graph,
                               Edge* edge,
                               const NodeGuiPtr & oldSrc,
                               const NodeGuiPtr & newSrc,
                               QUndoCommand *parent)
    : QUndoCommand(parent),
    _oldSrc(oldSrc),
    _newSrc(newSrc),
    _dst( edge->getDest() ),
    _graph(graph),
    _inputNb( edge->getInputNumber() )
{
    NodeGuiPtr dst = _dst.lock();
    assert(dst);
    if (newSrc) {
        setText( tr("Connect %1 to %2")
                 .arg( QString::fromUtf8( dst->getNode()->getLabel().c_str() ) ).arg( QString::fromUtf8( newSrc->getNode()->getLabel().c_str() ) ) );
    } else {
        setText( tr("Disconnect %1")
                 .arg( QString::fromUtf8( dst->getNode()->getLabel().c_str() ) ) );
    }
}

void
ConnectCommand::undo()
{
    NodeGuiPtr newSrc = _newSrc.lock();
    NodeGuiPtr oldSrc = _oldSrc.lock();
    NodeGuiPtr dst = _dst.lock();

    doConnect(newSrc,
              oldSrc,
              dst,
              _inputNb);

    ViewerInstance* isDstAViewer = dst->getNode()->isEffectViewer();
    if (!isDstAViewer) {
        _graph->getGui()->getApp()->triggerAutoSave();
    }
    _graph->update();
} // undo

void
ConnectCommand::redo()
{
    NodeGuiPtr newSrc = _newSrc.lock();
    NodeGuiPtr oldSrc = _oldSrc.lock();
    NodeGuiPtr dst = _dst.lock();

    doConnect(oldSrc,
              newSrc,
              dst,
              _inputNb);

    ViewerInstance* isDstAViewer = dst->getNode()->isEffectViewer();
    if (!isDstAViewer) {
        _graph->getGui()->getApp()->triggerAutoSave();
    }
    _graph->update();
} // redo

void
ConnectCommand::doConnect(const NodeGuiPtr &oldSrc,
                          const NodeGuiPtr &newSrc,
                          const NodeGuiPtr& dst,
                          int inputNb)
{
    NodePtr internalDst =  dst->getNode();
    NodePtr internalNewSrc = newSrc ? newSrc->getNode() : NodePtr();
    NodePtr internalOldSrc = oldSrc ? oldSrc->getNode() : NodePtr();
    ViewerInstance* isViewer = internalDst->isEffectViewer();


    if (isViewer) {
        ///if the node is an inspector  disconnect any current connection between the inspector and the _newSrc
        for (int i = 0; i < internalDst->getNInputs(); ++i) {
            if ( (i != inputNb) && (internalDst->getInput(i) == internalNewSrc) ) {
                internalDst->disconnectInput(i);
            }
        }
    }
    if (internalOldSrc && internalNewSrc) {
        internalDst->replaceInput(internalNewSrc, inputNb);
    } else {
        if (internalOldSrc && internalNewSrc) {
            Node::CanConnectInputReturnValue ret = internalDst->canConnectInput(internalNewSrc, inputNb);
            bool connectionOk = ret == Node::eCanConnectInput_ok ||
                                ret == Node::eCanConnectInput_differentFPS ||
                                ret == Node::eCanConnectInput_differentPars;
            if (connectionOk) {
                internalDst->replaceInput(internalNewSrc, inputNb);
            } else {
                internalDst->disconnectInput( internalDst->getInputIndex( internalOldSrc.get() ) );
            }
        } else if (internalOldSrc && !internalNewSrc) {
            internalDst->disconnectInput( internalDst->getInputIndex( internalOldSrc.get() ) );
        } else if (!internalOldSrc && internalNewSrc) {
            Node::CanConnectInputReturnValue ret = internalDst->canConnectInput(internalNewSrc, inputNb);
            bool connectionOk = ret == Node::eCanConnectInput_ok ||
                                ret == Node::eCanConnectInput_differentFPS ||
                                ret == Node::eCanConnectInput_differentPars;
            if (connectionOk) {
                internalDst->connectInput(internalNewSrc, inputNb);
            } else {
                internalDst->disconnectInput( internalDst->getInputIndex( internalOldSrc.get() ) );
            }
        }
    }

    dst->refreshEdges();
    dst->refreshEdgesVisility();

    if (newSrc) {
        newSrc->refreshEdgesVisility();
    }
    if (oldSrc) {
        oldSrc->refreshEdgesVisility();
    }
} // ConnectCommand::doConnect

InsertNodeCommand::InsertNodeCommand(NodeGraph* graph,
                                     Edge* edge,
                                     const NodeGuiPtr & newSrc,
                                     QUndoCommand *parent)
    : ConnectCommand(graph, edge, edge->getSource(), newSrc, parent)
    , _inputEdge(0)
{
    assert(newSrc);
    setText( tr("Insert node") );
}

void
InsertNodeCommand::undo()
{
    NodeGuiPtr oldSrc = _oldSrc.lock();
    NodeGuiPtr newSrc = _newSrc.lock();
    NodeGuiPtr dst = _dst.lock();

    assert(newSrc);

    NodePtr oldSrcInternal = oldSrc ? oldSrc->getNode() : NodePtr();
    NodePtr newSrcInternal = newSrc->getNode();
    NodePtr dstInternal = dst->getNode();
    assert(newSrcInternal && dstInternal);

    doConnect(newSrc, oldSrc, dst, _inputNb);

    if (_inputEdge) {
        doConnect( _inputEdge->getSource(), NodeGuiPtr(), _inputEdge->getDest(), _inputEdge->getInputNumber() );
    }

    ViewerInstance* isDstAViewer = dst->getNode()->isEffectViewer();
    if (!isDstAViewer) {
        _graph->getGui()->getApp()->triggerAutoSave();
    }
    _graph->update();
}

void
InsertNodeCommand::redo()
{
    NodeGuiPtr oldSrc = _oldSrc.lock();
    NodeGuiPtr newSrc = _newSrc.lock();
    NodeGuiPtr dst = _dst.lock();

    assert(newSrc);

    NodePtr oldSrcInternal = oldSrc ? oldSrc->getNode() : NodePtr();
    NodePtr newSrcInternal = newSrc->getNode();
    NodePtr dstInternal = dst->getNode();
    assert(newSrcInternal && dstInternal);

    newSrcInternal->beginInputEdition();
    dstInternal->beginInputEdition();

    doConnect(oldSrc, newSrc, dst, _inputNb);


    ///find out if the node is already connected to what the edge is connected
    bool alreadyConnected = false;
    const std::vector<NodeWPtr> & inpNodes = newSrcInternal->getGuiInputs();
    if (oldSrcInternal) {
        for (std::size_t i = 0; i < inpNodes.size(); ++i) {
            if (inpNodes[i].lock() == oldSrcInternal) {
                alreadyConnected = true;
                break;
            }
        }
    }

    _inputEdge = 0;
    if (oldSrcInternal && !alreadyConnected) {
        ///push a second command... this is a bit dirty but I don't have time to add a whole new command just for this
        int prefInput = newSrcInternal->getPreferredInputForConnection();
        if (prefInput != -1) {
            _inputEdge = newSrc->getInputArrow(prefInput);
            assert(_inputEdge);
            doConnect( _inputEdge->getSource(), oldSrc, _inputEdge->getDest(), _inputEdge->getInputNumber() );
        }
    }

    ViewerInstance* isDstAViewer = dst->getNode()->isEffectViewer();
    if (!isDstAViewer) {
        _graph->getGui()->getApp()->triggerAutoSave();
    }

    newSrcInternal->endInputEdition(false);
    dstInternal->endInputEdition(false);

    std::list<ViewerInstance* > viewers;
    dstInternal->hasViewersConnected(&viewers);
    for (std::list<ViewerInstance* >::iterator it2 = viewers.begin(); it2 != viewers.end(); ++it2) {
        (*it2)->renderCurrentFrame(true);
    }

    _graph->update();
} // InsertNodeCommand::redo

ResizeBackdropCommand::ResizeBackdropCommand(const NodeGuiPtr& bd,
                                             int w,
                                             int h,
                                             QUndoCommand *parent)
    : QUndoCommand(parent)
    , _bd(bd)
    , _w(w)
    , _h(h)
    , _oldW(0)
    , _oldH(0)
{
    QRectF bbox = bd->boundingRect();

    _oldW = bbox.width();
    _oldH = bbox.height();
    setText( tr("Resize %1").arg( QString::fromUtf8( _bd->getNode()->getLabel().c_str() ) ) );
}

ResizeBackdropCommand::~ResizeBackdropCommand()
{
}

void
ResizeBackdropCommand::undo()
{
    _bd->resize(_oldW, _oldH);
}

void
ResizeBackdropCommand::redo()
{
    _bd->resize(_w, _h);
}

bool
ResizeBackdropCommand::mergeWith(const QUndoCommand *command)
{
    const ResizeBackdropCommand* rCmd = dynamic_cast<const ResizeBackdropCommand*>(command);

    if (!rCmd) {
        return false;
    }
    if (rCmd->_bd != _bd) {
        return false;
    }
    _w = rCmd->_w;
    _h = rCmd->_h;

    return true;
}

DecloneMultipleNodesCommand::DecloneMultipleNodesCommand(NodeGraph* graph,
                                                         const std::list<NodeGuiPtr> & nodes,
                                                         QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
    , _graph(graph)
{
    for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodeToDeclone n;
        n.node = *it;
        n.master = (*it)->getNode()->getMasterNode();
        assert( n.master.lock() );
        _nodes.push_back(n);
    }
    setText( tr("Declone node(s)") );
}

DecloneMultipleNodesCommand::~DecloneMultipleNodesCommand()
{
}

void
DecloneMultipleNodesCommand::undo()
{
    for (std::list<NodeToDeclone>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->node.lock()->getNode()->getEffectInstance()->slaveAllKnobs( it->master.lock()->getEffectInstance().get(), false );
    }

    _graph->getGui()->getApp()->triggerAutoSave();
}

void
DecloneMultipleNodesCommand::redo()
{
    for (std::list<NodeToDeclone>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->node.lock()->getNode()->getEffectInstance()->unslaveAllKnobs();
    }

    _graph->getGui()->getApp()->triggerAutoSave();
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

// Rearrange Nodes: a layered layout in the style of a clean Nuke script. Inputs above, outputs below;
// each node's main input chain (its "B pipe") is a straight vertical column, other inputs come in
// from the left and masks from the right, and a source sits just above the first node using it.
// Dots are elbows: on their input's row (a horizontal wire out of it), over the node they feed.
// Steps: rows by depth, invisible placeholders on links spanning rows, rows ordered to cross as few
// links as possible, main chains aligned into columns, columns spaced by the real node widths.

struct LayoutNode
{
    NodeGuiPtr gui;           // null for a placeholder on a link spanning several rows
    double w, h;
    double oldX;              // centre before the layout
    int rank;                 // row, 0 = top
    int comp;
    double x;                 // centre
    std::vector<int> up, down;        // neighbours on the row above (inputs) / below (outputs)
    std::vector<double> upW, downW;   // link weights
    int mainUp;               // index in up of the main input, -1 if none
    int root;                 // the column (top node of the aligned main chain) it belongs to
    bool dot;
    int sideIn;                       // a Dot on its input's row: that input
    std::vector<int> side;            // linked nodes on the same row (a Dot and its input)
    std::vector<double> sideW;
};

struct LayoutLink
{
    int src, dst;
    double w;      // 2 = main input (the column), 1 = other input, 0.5 = mask, out of a Dot 3, into one 0.1
    int slot;      // left-to-right order of a node's inputs: other inputs, main input, masks
    bool main;     // the node's main input: they share a column
};

const double kLayoutHGap = 50.;
const double kLayoutVGap = 140.;
const double kLayoutCompGap = 120.;

static int
layoutFind(std::vector<int>& parent, int i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }

    return i;
}

static int
countCrossings(const std::vector<LayoutNode>& n,
               const std::vector<int>& lower,
               const std::vector<int>& pos)
{
    // links between a row and the one above, as (position below, position above)
    std::vector<std::pair<int, int> > l;
    for (std::size_t i = 0; i < lower.size(); ++i) {
        const std::vector<int>& up = n[lower[i]].up;
        for (std::size_t k = 0; k < up.size(); ++k) {
            l.push_back( std::make_pair( (int)i, pos[up[k]] ) );
        }
    }
    int c = 0;
    for (std::size_t a = 0; a < l.size(); ++a) {
        for (std::size_t b = a + 1; b < l.size(); ++b) {
            if ( ( (l[a].first < l[b].first) && (l[a].second > l[b].second) ) ||
                 ( (l[a].first > l[b].first) && (l[a].second < l[b].second) ) ) {
                ++c;
            }
        }
    }

    return c;
}

static double
layoutGap(const LayoutNode& a,
          const LayoutNode& b)
{
    return (a.w + b.w) / 2. + ( (a.gui && b.gui) ? kLayoutHGap : kLayoutHGap / 2. );
}

// Moves a row's nodes as close as possible to where they want to be (least squares) while keeping
// their order and the gaps between them: isotonic regression (pool adjacent violators).
static void
placeRow(std::vector<LayoutNode>& n,
         const std::vector<int>& row,
         const std::vector<double>& desired)
{
    const std::size_t k = row.size();
    std::vector<double> offset(k, 0.);
    for (std::size_t i = 1; i < k; ++i) {
        offset[i] = offset[i - 1] + layoutGap(n[row[i - 1]], n[row[i]]);
    }
    std::vector<double> blockVal;
    std::vector<int> blockLen;
    for (std::size_t i = 0; i < k; ++i) {
        blockVal.push_back(desired[i] - offset[i]);
        blockLen.push_back(1);
        while (blockVal.size() > 1 && blockVal[blockVal.size() - 2] > blockVal.back()) {
            const double v = blockVal.back();
            const int len = blockLen.back();
            blockVal.pop_back();
            blockLen.pop_back();
            blockVal.back() = (blockVal.back() * blockLen.back() + v * len) / (blockLen.back() + len);
            blockLen.back() += len;
        }
    }
    std::size_t i = 0;
    for (std::size_t b = 0; b < blockVal.size(); ++b) {
        for (int j = 0; j < blockLen[b]; ++j, ++i) {
            n[row[i]].x = blockVal[b] + offset[i];
        }
    }
}

// New top-left positions (parent coordinates) for the selected nodes; backdrops stay put.
static void
layeredLayout(const NodesGuiList& selection,
              std::list<std::pair<NodeGuiPtr, QPointF> >& result)
{
    std::vector<LayoutNode> n;
    std::map<NodeGui*, int> index;
    double oldLeft = std::numeric_limits<double>::max();
    double oldTop = std::numeric_limits<double>::max();

    for (NodesGuiList::const_iterator it = selection.begin(); it != selection.end(); ++it) {
        if ( !*it || dynamic_cast<BackdropGui*>( it->get() ) || index.count( it->get() ) ) {
            continue;
        }
        const QSize s = (*it)->getSize();
        const QPointF p = (*it)->pos();
        LayoutNode ln;
        ln.gui = *it;
        ln.w = s.width();
        ln.h = s.height();
        ln.oldX = p.x() + ln.w / 2.;
        ln.rank = 0;
        ln.comp = 0;
        ln.x = 0.;
        ln.mainUp = -1;
        ln.root = 0;
        ln.dot = (*it)->getNode() && (*it)->getNode()->getPluginID() == PLUGINID_NATRON_DOT;
        ln.sideIn = -1;
        index[it->get()] = (int)n.size();
        n.push_back(ln);
        oldLeft = std::min( oldLeft, p.x() );
        oldTop = std::min( oldTop, p.y() );
    }
    const int numReal = (int)n.size();
    if (numReal == 0) {
        return;
    }

    // links between selected nodes, one per pair
    std::vector<LayoutLink> links;
    for (int v = 0; v < numReal; ++v) {
        const std::vector<Edge*>& inputs = n[v].gui->getInputsArrows();
        NodePtr node = n[v].gui->getNode();
        EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();
        bool mainSeen = false;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            NodeGuiPtr source = inputs[i] ? inputs[i]->getSource() : NodeGuiPtr();
            std::map<NodeGui*, int>::const_iterator found = source ? index.find( source.get() ) : index.end();
            if ( ( found == index.end() ) || (found->second == v) ) {
                continue;
            }
            const bool mask = effect && effect->isInputMask( (int)i );
            LayoutLink l;
            l.src = found->second;
            l.dst = v;
            l.main = false;
            if (mask) {
                l.w = 0.5;
                l.slot = 1000 + (int)i;
            } else if (!mainSeen) {
                mainSeen = true;
                l.w = 2.;
                l.slot = 0;
                l.main = true;
            } else {
                l.w = 1.;
                l.slot = -(int)i;
            }
            if (n[v].dot) {
                l.w = 0.1;      // a Dot follows the node it feeds, not its input
            } else if (n[l.src].dot) {
                l.w = std::max(l.w, 3.);
            }
            bool dup = false;
            for (std::size_t k = 0; k < links.size(); ++k) {
                if ( (links[k].src == l.src) && (links[k].dst == l.dst) ) {
                    if (l.w > links[k].w) {
                        links[k].w = l.w;
                        links[k].slot = l.slot;
                    }
                    links[k].main = links[k].main || l.main;
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                links.push_back(l);
            }
        }
    }

    // rows: the longest path from a source (a link closing a cycle is ignored)
    std::vector<std::vector<int> > ins(numReal), outs(numReal);
    for (std::size_t k = 0; k < links.size(); ++k) {
        ins[links[k].dst].push_back( (int)k );
    }
    std::vector<char> dropped(links.size(), 0);
    std::vector<int> state(numReal, 0);
    std::vector<int> topo;      // inputs before the nodes they feed
    for (int s = 0; s < numReal; ++s) {
        if (state[s]) {
            continue;
        }
        // iterative depth first search (a long chain must not overflow the stack)
        std::vector<std::pair<int, std::size_t> > stack;
        stack.push_back( std::make_pair(s, (std::size_t)0) );
        state[s] = 1;
        while ( !stack.empty() ) {
            const int v = stack.back().first;
            if ( stack.back().second < ins[v].size() ) {
                const int li = ins[v][stack.back().second++];
                const int u = links[li].src;
                if (state[u] == 1) {
                    dropped[li] = 1;
                } else if (state[u] == 0) {
                    state[u] = 1;
                    stack.push_back( std::make_pair(u, (std::size_t)0) );
                }
                continue;
            }
            int r = 0;
            for (std::size_t j = 0; j < ins[v].size(); ++j) {
                if (!dropped[ins[v][j]]) {
                    r = std::max(r, n[links[ins[v][j]].src].rank + (n[v].dot ? 0 : 1) );
                }
            }
            n[v].rank = r;
            state[v] = 2;
            topo.push_back(v);
            stack.pop_back();
        }
    }
    for (std::size_t k = 0; k < links.size(); ++k) {
        if (!dropped[k]) {
            outs[links[k].src].push_back( (int)k );
        }
    }
    // a source sits just above the first node using it (a camera next to its renders, not on top);
    // through Dots, which share their input's row. lowest[v]: the lowest row v may move down to.
    std::vector<int> lowest(numReal, std::numeric_limits<int>::max());
    for (std::size_t t = topo.size(); t-- > 0;) {
        const int v = topo[t];
        for (std::size_t j = 0; j < outs[v].size(); ++j) {
            const int d = links[outs[v][j]].dst;
            lowest[v] = std::min(lowest[v], n[d].dot ? lowest[d] : n[d].rank - 1);
        }
    }
    for (int v = 0; v < numReal; ++v) {
        bool hasInput = false;
        for (std::size_t j = 0; j < ins[v].size(); ++j) {
            hasInput = hasInput || !dropped[ins[v][j]];
        }
        if ( !hasInput && !n[v].dot && ( lowest[v] != std::numeric_limits<int>::max() ) ) {
            n[v].rank = std::max(n[v].rank, lowest[v]);
        }
    }
    for (std::size_t t = 0; t < topo.size(); ++t) {
        const int v = topo[t];
        if (n[v].dot) {
            for (std::size_t j = 0; j < ins[v].size(); ++j) {
                if (!dropped[ins[v][j]]) {
                    n[v].rank = n[links[ins[v][j]].src].rank;
                }
            }
        }
    }
    // an output sharing an input with other nodes (renders of one camera) lines up with the
    // lowest of them
    for (int v = 0; v < numReal; ++v) {
        if ( !outs[v].empty() || n[v].dot ) {
            continue;
        }
        int r = n[v].rank;
        for (std::size_t j = 0; j < ins[v].size(); ++j) {
            if (dropped[ins[v][j]]) {
                continue;
            }
            const std::vector<int>& sib = outs[links[ins[v][j]].src];
            for (std::size_t k = 0; k < sib.size(); ++k) {
                const int d = links[sib[k]].dst;
                if ( (d != v) && !n[d].dot ) {
                    r = std::max(r, n[d].rank);
                }
            }
        }
        n[v].rank = r;
    }

    // connected parts are laid out side by side
    std::vector<int> parent(numReal);
    for (int i = 0; i < numReal; ++i) {
        parent[i] = i;
    }
    for (std::size_t k = 0; k < links.size(); ++k) {
        if (!dropped[k]) {
            parent[layoutFind(parent, links[k].src)] = layoutFind(parent, links[k].dst);
        }
    }
    for (int i = 0; i < numReal; ++i) {
        n[i].comp = layoutFind(parent, i);
    }

    // adjacency, with placeholders on links spanning several rows; a node's inputs in slot order
    std::vector<int> order( links.size() );
    for (std::size_t k = 0; k < order.size(); ++k) {
        order[k] = (int)k;
    }
    std::stable_sort(order.begin(), order.end(), [&links](int a, int b) {
        return links[a].dst != links[b].dst ? links[a].dst < links[b].dst : links[a].slot < links[b].slot;
    });
    for (std::size_t o = 0; o < order.size(); ++o) {
        if (dropped[order[o]]) {
            continue;
        }
        const LayoutLink l = links[order[o]];
        if (n[l.src].rank >= n[l.dst].rank) {
            // same row: a Dot and its input
            n[l.src].side.push_back(l.dst);
            n[l.src].sideW.push_back(l.w);
            n[l.dst].side.push_back(l.src);
            n[l.dst].sideW.push_back(l.w);
            n[l.dst].sideIn = l.src;
            continue;
        }
        int above = l.src;
        for (int r = n[l.src].rank + 1; r <= n[l.dst].rank; ++r) {
            int below = l.dst;
            if (r < n[l.dst].rank) {
                LayoutNode d;
                d.w = 0.;
                d.h = 0.;
                d.oldX = n[l.src].oldX;
                d.rank = r;
                d.comp = n[l.dst].comp;
                d.x = 0.;
                d.mainUp = -1;
                d.root = 0;
                d.dot = false;
                d.sideIn = -1;
                below = (int)n.size();
                n.push_back(d);
            }
            if (l.main) {
                n[below].mainUp = (int)n[below].up.size();
            }
            n[below].up.push_back(above);
            n[below].upW.push_back(l.w);
            n[above].down.push_back(below);
            n[above].downW.push_back(l.w);
            above = below;
        }
    }

    int maxRank = 0;
    for (std::size_t i = 0; i < n.size(); ++i) {
        maxRank = std::max(maxRank, n[i].rank);
    }

    // parts ordered by where they were, left to right
    std::map<int, std::pair<double, int> > compOldX;     // comp -> (sum of x, count)
    for (int i = 0; i < numReal; ++i) {
        std::pair<double, int>& c = compOldX[n[i].comp];
        c.first += n[i].oldX;
        c.second += 1;
    }
    std::vector<std::pair<double, int> > comps;
    for (std::map<int, std::pair<double, int> >::const_iterator it = compOldX.begin(); it != compOldX.end(); ++it) {
        comps.push_back( std::make_pair(it->second.first / it->second.second, it->first) );
    }
    std::sort( comps.begin(), comps.end() );

    // row centres, shared by all parts, top to bottom
    std::vector<double> rowH(maxRank + 1, 0.);
    for (std::size_t i = 0; i < n.size(); ++i) {
        rowH[n[i].rank] = std::max(rowH[n[i].rank], n[i].h);
    }
    std::vector<double> rowY(maxRank + 1, 0.);
    for (int r = 1; r <= maxRank; ++r) {
        rowY[r] = rowY[r - 1] + (rowH[r - 1] / 2. + kLayoutVGap + rowH[r] / 2.);
    }

    double nextLeft = 0.;
    for (std::size_t c = 0; c < comps.size(); ++c) {
        const int comp = comps[c].second;
        std::vector<std::vector<int> > rows(maxRank + 1);

        // first order: depth first up from the outputs, each node's inputs in slot order
        std::vector<int> sinks;
        for (int i = 0; i < numReal; ++i) {
            if ( (n[i].comp == comp) && n[i].down.empty() ) {
                sinks.push_back(i);
            }
        }
        std::stable_sort(sinks.begin(), sinks.end(), [&n](int a, int b) { return n[a].oldX < n[b].oldX; });
        std::vector<char> seen(n.size(), 0);
        for (std::size_t s = 0; s < sinks.size(); ++s) {
            std::vector<int> stack(1, sinks[s]);
            while ( !stack.empty() ) {
                const int v = stack.back();
                stack.pop_back();
                if (seen[v]) {
                    continue;
                }
                seen[v] = 1;
                rows[n[v].rank].push_back(v);
                for (std::size_t k = n[v].up.size(); k-- > 0;) {
                    stack.push_back(n[v].up[k]);
                }
            }
        }
        for (std::size_t i = 0; i < n.size(); ++i) {
            if ( (n[i].comp == comp) && !seen[i] ) {
                rows[n[i].rank].push_back( (int)i );
            }
        }

        // fewer crossings: barycentre sweeps down and up, keep the best order found
        std::vector<int> pos(n.size(), 0);
        auto updatePos = [&]() {
            for (std::size_t r = 0; r < rows.size(); ++r) {
                for (std::size_t i = 0; i < rows[r].size(); ++i) {
                    pos[rows[r][i]] = (int)i;
                }
            }
        };
        auto crossings = [&]() {
            int total = 0;
            for (int r = 1; r <= maxRank; ++r) {
                total += countCrossings(n, rows[r], pos);
            }

            return total;
        };
        // a Dot on its input's row stays next to that input (only the input's other Dots between
        // them), on the side it was sorted to: its horizontal wire never runs behind another node
        auto regroup = [&](std::vector<int>& row) {
            std::map<int, std::vector<int> > attached;     // input -> its Dots, in row order
            std::set<int> isAttached;
            for (std::size_t i = 0; i < row.size(); ++i) {
                int a = row[i];
                for (int guard = 0; guard < 64 && n[a].sideIn >= 0; ++guard) {
                    a = n[a].sideIn;
                }
                if ( (a != row[i]) && !n[a].dot ) {
                    attached[a].push_back(row[i]);
                    isAttached.insert(row[i]);
                }
            }
            if ( attached.empty() ) {
                return;
            }
            std::vector<int> out;
            for (std::size_t i = 0; i < row.size(); ++i) {
                const int v = row[i];
                if ( isAttached.count(v) ) {
                    continue;
                }
                std::map<int, std::vector<int> >::const_iterator at = attached.find(v);
                if ( at == attached.end() ) {
                    out.push_back(v);
                    continue;
                }
                const int here = (int)i;
                for (std::size_t k = 0; k < at->second.size(); ++k) {
                    if (pos[at->second[k]] < here) {
                        out.push_back(at->second[k]);
                    }
                }
                out.push_back(v);
                for (std::size_t k = 0; k < at->second.size(); ++k) {
                    if (pos[at->second[k]] > here) {
                        out.push_back(at->second[k]);
                    }
                }
            }
            row = out;
        };
        updatePos();
        for (int r = 0; r <= maxRank; ++r) {
            regroup(rows[r]);
        }
        updatePos();
        std::vector<std::vector<int> > best = rows;
        int bestCross = crossings();
        for (int sweep = 0; sweep < 12 && bestCross > 0; ++sweep) {
            const bool downward = (sweep % 2 == 0);
            for (int step = 0; step < maxRank; ++step) {
                const int r = downward ? step + 1 : maxRank - 1 - step;
                std::vector<int>& row = rows[r];
                std::vector<double> key( row.size() );
                for (std::size_t i = 0; i < row.size(); ++i) {
                    const std::vector<int>& nb = downward ? n[row[i]].up : n[row[i]].down;
                    const std::vector<double>& nw = downward ? n[row[i]].upW : n[row[i]].downW;
                    double sum = 0., wsum = 0.;
                    for (std::size_t k = 0; k < nb.size(); ++k) {
                        sum += nw[k] * pos[nb[k]];
                        wsum += nw[k];
                    }
                    if ( (wsum == 0.) && n[row[i]].dot ) {
                        // a Dot (no inputs above): where the nodes it feeds are
                        const std::vector<int>& dn = n[row[i]].down;
                        for (std::size_t k = 0; k < dn.size(); ++k) {
                            const std::vector<int>& other = rows[n[dn[k]].rank];
                            sum += pos[dn[k]] * (double)row.size() / std::max<std::size_t>(1, other.size());
                            wsum += 1.;
                        }
                    }
                    key[i] = wsum > 0. ? sum / wsum : (double)i;
                }
                std::vector<int> idx( row.size() );
                for (std::size_t i = 0; i < idx.size(); ++i) {
                    idx[i] = (int)i;
                }
                std::stable_sort(idx.begin(), idx.end(), [&key](int a, int b) { return key[a] < key[b]; });
                std::vector<int> sorted( row.size() );
                for (std::size_t i = 0; i < idx.size(); ++i) {
                    sorted[i] = row[idx[i]];
                }
                row = sorted;
                for (std::size_t i = 0; i < row.size(); ++i) {
                    pos[row[i]] = (int)i;
                }
                regroup(row);
                for (std::size_t i = 0; i < row.size(); ++i) {
                    pos[row[i]] = (int)i;
                }
            }
            const int cross = crossings();
            if (cross < bestCross) {
                bestCross = cross;
                best = rows;
            }
        }
        rows = best;
        updatePos();

        // columns: a node shares the column of its main input. A node that is the main input of
        // several gives its column to the middle one; alignments never cross each other.
        for (int r = 0; r <= maxRank; ++r) {
            for (std::size_t i = 0; i < rows[r].size(); ++i) {
                n[rows[r][i]].root = rows[r][i];
            }
        }
        for (int r = 1; r <= maxRank; ++r) {
            std::map<int, std::vector<int> > wants;      // upper node -> lower nodes, in row order
            for (std::size_t i = 0; i < rows[r].size(); ++i) {
                const LayoutNode& v = n[rows[r][i]];
                if (v.mainUp >= 0) {
                    wants[v.up[v.mainUp]].push_back(rows[r][i]);
                }
            }
            std::set<int> chosen;
            for (std::map<int, std::vector<int> >::const_iterator it = wants.begin(); it != wants.end(); ++it) {
                chosen.insert( it->second[(it->second.size() - 1) / 2] );
            }
            int last = -1;
            for (std::size_t i = 0; i < rows[r].size(); ++i) {
                const int v = rows[r][i];
                if ( !chosen.count(v) ) {
                    continue;
                }
                const int u = n[v].up[n[v].mainUp];
                if (pos[u] > last) {
                    n[v].root = n[u].root;
                    last = pos[u];
                }
            }
        }

        // x, soft: start packed, then pull every column over the nodes it connects to
        for (int r = 0; r <= maxRank; ++r) {
            placeRow( n, rows[r], std::vector<double>(rows[r].size(), 0.) );
        }
        for (int iter = 0; iter < 30; ++iter) {
            std::map<int, std::pair<double, double> > want;    // column -> (sum, weight)
            for (int r = 0; r <= maxRank; ++r) {
                for (std::size_t i = 0; i < rows[r].size(); ++i) {
                    const LayoutNode& v = n[rows[r][i]];
                    std::pair<double, double>& p = want[v.root];
                    for (std::size_t k = 0; k < v.up.size(); ++k) {
                        p.first += v.upW[k] * n[v.up[k]].x;
                        p.second += v.upW[k];
                    }
                    for (std::size_t k = 0; k < v.down.size(); ++k) {
                        p.first += v.downW[k] * n[v.down[k]].x;
                        p.second += v.downW[k];
                    }
                    for (std::size_t k = 0; k < v.side.size(); ++k) {
                        p.first += v.sideW[k] * n[v.side[k]].x;
                        p.second += v.sideW[k];
                    }
                    // a little pull towards where it is now, so lone columns stay put
                    p.first += 0.1 * v.x;
                    p.second += 0.1;
                }
            }
            for (int step = 0; step <= maxRank; ++step) {
                const int r = (iter % 2 == 0) ? step : maxRank - step;
                std::vector<double> desired( rows[r].size() );
                for (std::size_t i = 0; i < rows[r].size(); ++i) {
                    const std::pair<double, double>& p = want[n[rows[r][i]].root];
                    desired[i] = p.first / p.second;
                }
                placeRow(n, rows[r], desired);
            }
        }

        // x, exact: one x per column, columns pushed right only as far as the gaps need
        std::map<int, std::pair<double, int> > colSoft;            // column -> (sum of x, count)
        std::map<int, std::vector<std::pair<int, double> > > rightOf; // column -> (column right of it, gap)
        std::map<int, int> indegree;
        for (int r = 0; r <= maxRank; ++r) {
            for (std::size_t i = 0; i < rows[r].size(); ++i) {
                const LayoutNode& v = n[rows[r][i]];
                std::pair<double, int>& s = colSoft[v.root];
                s.first += v.x;
                s.second += 1;
                indegree[v.root] += 0;
                if (i > 0) {
                    const LayoutNode& a = n[rows[r][i - 1]];
                    rightOf[a.root].push_back( std::make_pair( v.root, layoutGap(a, v) ) );
                    indegree[v.root] += 1;
                }
            }
        }
        std::vector<int> ready;
        for (std::map<int, int>::const_iterator it = indegree.begin(); it != indegree.end(); ++it) {
            if (it->second == 0) {
                ready.push_back(it->first);
            }
        }
        std::map<int, double> colX;
        for (std::map<int, std::pair<double, int> >::const_iterator it = colSoft.begin(); it != colSoft.end(); ++it) {
            colX[it->first] = it->second.first / it->second.second;
        }
        std::size_t done = 0;
        while ( !ready.empty() ) {
            const int col = ready.back();
            ready.pop_back();
            ++done;
            const std::vector<std::pair<int, double> >& next = rightOf[col];
            for (std::size_t k = 0; k < next.size(); ++k) {
                colX[next[k].first] = std::max(colX[next[k].first], colX[col] + next[k].second);
                if (--indegree[next[k].first] == 0) {
                    ready.push_back(next[k].first);
                }
            }
        }
        if ( done == colSoft.size() ) {
            for (int r = 0; r <= maxRank; ++r) {
                for (std::size_t i = 0; i < rows[r].size(); ++i) {
                    n[rows[r][i]].x = colX[n[rows[r][i]].root];
                }
            }
        } // else (crossing columns, should not happen) keep the soft positions: no overlaps either
        // this part starts right of the previous one
        double left = std::numeric_limits<double>::max();
        double right = -std::numeric_limits<double>::max();
        for (int r = 0; r <= maxRank; ++r) {
            for (std::size_t i = 0; i < rows[r].size(); ++i) {
                const LayoutNode& v = n[rows[r][i]];
                left = std::min(left, v.x - v.w / 2.);
                right = std::max(right, v.x + v.w / 2.);
            }
        }
        const double shift = nextLeft - left;
        for (int r = 0; r <= maxRank; ++r) {
            for (std::size_t i = 0; i < rows[r].size(); ++i) {
                n[rows[r][i]].x += shift;
            }
        }
        nextLeft += (right - left) + kLayoutCompGap;
    }

    // keep the graph where it was: same top-left corner
    const double dx = oldLeft;
    const double dy = oldTop - (rowY[0] - rowH[0] / 2.);
    for (int i = 0; i < numReal; ++i) {
        const LayoutNode& v = n[i];
        result.push_back( std::make_pair( v.gui, QPointF(v.x - v.w / 2. + dx, rowY[v.rank] - v.h / 2. + dy) ) );
    }
} // layeredLayout

static bool
hasNodeOutputsInList(const std::list<NodeGuiPtr> & nodes,
                     const NodeGuiPtr& node)
{
    const NodesWList& outputs = node->getNode()->getGuiOutputs();
    bool foundOutput = false;

    for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (*it != node) {
            NodePtr n = (*it)->getNode();

            for (NodesWList::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
                if (it2->lock() == n) {
                    foundOutput = true;
                    break;
                }
            }
            if (foundOutput) {
                break;
            }
        }
    }

    return foundOutput;
}

static bool
hasNodeInputsInList(const std::list<NodeGuiPtr> & nodes,
                    const NodeGuiPtr& node)
{
    const std::vector<NodeWPtr>& inputs = node->getNode()->getGuiInputs();
    bool foundInput = false;

    for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (*it != node) {
            NodePtr n = (*it)->getNode();

            for (std::size_t i = 0; i < inputs.size(); ++i) {
                if (inputs[i].lock() == n) {
                    foundInput = true;
                    break;
                }
            }
            if (foundInput) {
                break;
            }
        }
    }

    return foundInput;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT


RearrangeNodesCommand::RearrangeNodesCommand(const std::list<NodeGuiPtr> & nodes,
                                             QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
{
    std::list<std::pair<NodeGuiPtr, QPointF> > placed;
    layeredLayout(nodes, placed);
    for (std::list<std::pair<NodeGuiPtr, QPointF> >::const_iterator it = placed.begin(); it != placed.end(); ++it) {
        NodeToRearrange n;
        n.node = it->first;
        n.newPos = it->second;
        n.oldPos = n.node->pos();
        _nodes.push_back(n);
    }
    setText( tr("Rearrange nodes") );
}

void
RearrangeNodesCommand::undo()
{
    for (std::list<NodeToRearrange>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->node->refreshPosition(it->oldPos.x(), it->oldPos.y(), true);
    }
}

void
RearrangeNodesCommand::redo()
{
    for (std::list<NodeToRearrange>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->node->refreshPosition(it->newPos.x(), it->newPos.y(), true);
    }
}

DisableNodesCommand::DisableNodesCommand(const std::list<NodeGuiPtr> & nodes,
                                         QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
{
    for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        _nodes.push_back(*it);
    }
    setText( tr("Disable node(s)") );
}

void
DisableNodesCommand::undo()
{
    for (std::list<NodeGuiWPtr>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->lock()->getNode()->setNodeDisabled(false);
    }
}

void
DisableNodesCommand::redo()
{
    for (std::list<NodeGuiWPtr>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->lock()->getNode()->setNodeDisabled(true);
    }
}

EnableNodesCommand::EnableNodesCommand(const std::list<NodeGuiPtr> & nodes,
                                       QUndoCommand *parent)
    : QUndoCommand(parent)
    , _nodes()
{
    for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        _nodes.push_back(*it);
    }
    setText( tr("Enable node(s)") );
}

void
EnableNodesCommand::undo()
{
    for (std::list<NodeGuiWPtr>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->lock()->getNode()->setNodeDisabled(true);
    }
}

void
EnableNodesCommand::redo()
{
    for (std::list<NodeGuiWPtr>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        it->lock()->getNode()->setNodeDisabled(false);
    }
}

LoadNodePresetsCommand::LoadNodePresetsCommand(const NodeGuiPtr & node,
                                               const std::list<NodeSerializationPtr>& serialization,
                                               QUndoCommand *parent)
    : QUndoCommand(parent)
    , _firstRedoCalled(false)
    , _isUndone(false)
    , _node(node)
    , _newSerializations(serialization)
{
    setText( tr("Load presets") );
}

void
LoadNodePresetsCommand::getListAsShared(const std::list<NodeWPtr>& original,
                                        std::list<NodePtr>& shared) const
{
    for (std::list<NodeWPtr>::const_iterator it = original.begin(); it != original.end(); ++it) {
        shared.push_back( it->lock() );
    }
}

LoadNodePresetsCommand::~LoadNodePresetsCommand()
{
}

void
LoadNodePresetsCommand::undo()
{
    _isUndone = true;

    NodeGuiPtr node = _node.lock();
    NodePtr internalNode = node->getNode();
    MultiInstancePanelPtr panel = node->getMultiInstancePanel();
    internalNode->loadKnobs(*_oldSerialization.front(), true);
    if (panel) {
        std::list<NodePtr> newChildren, oldChildren;
        getListAsShared(_newChildren, newChildren);
        getListAsShared(_oldChildren, oldChildren);
        panel->removeInstances(newChildren);
        panel->addInstances(oldChildren);
    }
    internalNode->getEffectInstance()->incrHashAndEvaluate(true, true);
    internalNode->getApp()->triggerAutoSave();
}

void
LoadNodePresetsCommand::redo()
{
    NodeGuiPtr node = _node.lock();
    NodePtr internalNode = node->getNode();
    MultiInstancePanelPtr panel = node->getMultiInstancePanel();

    if (!_firstRedoCalled) {
        ///Serialize the current state of the node
        node->serializeInternal(_oldSerialization);

        if (panel) {
            const std::list<std::pair<NodeWPtr, bool> >& children = panel->getInstances();
            for (std::list<std::pair<NodeWPtr, bool> >::const_iterator it = children.begin();
                 it != children.end(); ++it) {
                _oldChildren.push_back( it->first.lock() );
            }
        }

        int k = 0;

        for (std::list<NodeSerializationPtr>::const_iterator it = _newSerializations.begin();
             it != _newSerializations.end(); ++it, ++k) {
            if (k > 0) {  /// this is a multi-instance child, create it
                NodePtr newNode = panel->createNewInstance(false);
                newNode->loadKnobs(**it);
                std::list<SequenceTime> keys;
                newNode->getAllKnobsKeyframes(&keys);
                newNode->getApp()->addMultipleKeyframeIndicatorsAdded(keys, true);
                _newChildren.push_back(newNode);
            }
        }
    }

    internalNode->loadKnobs(*_newSerializations.front(), true);
    if (panel) {
        std::list<NodePtr> oldChildren, newChildren;
        getListAsShared(_oldChildren, oldChildren);
        getListAsShared(_newChildren, newChildren);
        panel->removeInstances(oldChildren);
        if (_firstRedoCalled) {
            panel->addInstances(newChildren);
        }
    }

    NodesList allNodes;
    internalNode->getApp()->getProject()->getActiveNodesExpandGroups(&allNodes);

    std::map<std::string, std::string> oldNewScriptNames;
    internalNode->storeKnobsLinks(*_newSerializations.front(), oldNewScriptNames);
    internalNode->restoreKnobsLinks(allNodes, oldNewScriptNames, true); // may not fail
    internalNode->getEffectInstance()->incrHashAndEvaluate(true, true);
    internalNode->getApp()->triggerAutoSave();
    _firstRedoCalled = true;
} // LoadNodePresetsCommand::redo

RenameNodeUndoRedoCommand::RenameNodeUndoRedoCommand(const NodeGuiPtr & node,
                                                     const QString& oldName,
                                                     const QString& newName)
    : QUndoCommand()
    , _node(node)
    , _oldName(oldName)
    , _newName(newName)
{
    assert(node);
    setText( tr("Rename node") );
}

RenameNodeUndoRedoCommand::~RenameNodeUndoRedoCommand()
{
}

void
RenameNodeUndoRedoCommand::undo()
{
    NodeGuiPtr node = _node.lock();

    node->setName(_oldName);
}

void
RenameNodeUndoRedoCommand::redo()
{
    NodeGuiPtr node = _node.lock();

    node->setName(_newName);
}

static void
addTreeInputs(const std::list<NodeGuiPtr> & nodes,
              const NodeGuiPtr& node,
              ExtractedTree& tree,
              std::list<NodeGuiPtr> & markedNodes)
{
    if ( std::find(markedNodes.begin(), markedNodes.end(), node) != markedNodes.end() ) {
        return;
    }

    if ( std::find(nodes.begin(), nodes.end(), node) == nodes.end() ) {
        return;
    }

    if ( !hasNodeInputsInList(nodes, node) ) {
        ExtractedInput input;
        input.node = node;
        input.inputs = node->getNode()->getGuiInputs();
        tree.inputs.push_back(input);
        markedNodes.push_back(node);
    } else {
        tree.inbetweenNodes.push_back(node);
        markedNodes.push_back(node);
        const std::vector<Edge*>& inputs = node->getInputsArrows();
        for (std::vector<Edge*>::const_iterator it2 = inputs.begin(); it2 != inputs.end(); ++it2) {
            NodeGuiPtr input = (*it2)->getSource();
            if (input) {
                addTreeInputs(nodes, input, tree, markedNodes);
            }
        }
    }
}

static void
extractTreesFromNodes(const std::list<NodeGuiPtr> & nodes,
                      std::list<ExtractedTree>& trees)
{
    std::list<NodeGuiPtr> markedNodes;

    for (std::list<NodeGuiPtr> ::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        bool isOutput = !hasNodeOutputsInList(nodes, *it);
        if (isOutput) {
            ExtractedTree tree;
            tree.output.node = *it;
            NodePtr n = (*it)->getNode();
            const NodesWList& outputs = n->getGuiOutputs();
            for (NodesWList::const_iterator it2 = outputs.begin(); it2 != outputs.end(); ++it2) {
                NodePtr output = it2->lock();
                if (!output) {
                    continue;
                }
                int idx = output->inputIndex(n);
                tree.output.outputs.push_back( std::make_pair(idx, output) );
            }

            const std::vector<Edge*>& inputs = (*it)->getInputsArrows();
            for (U32 i = 0; i < inputs.size(); ++i) {
                NodeGuiPtr input = inputs[i]->getSource();
                if (input) {
                    addTreeInputs(nodes, input, tree, markedNodes);
                }
            }

            if ( tree.inputs.empty() ) {
                ExtractedInput input;
                input.node = *it;
                input.inputs = n->getGuiInputs();
                tree.inputs.push_back(input);
            }

            trees.push_back(tree);
        }
    }
}

///////////////

ExtractNodeUndoRedoCommand::ExtractNodeUndoRedoCommand(NodeGraph* graph,
                                                       const std::list<NodeGuiPtr> & nodes)
    : QUndoCommand()
    , _graph(graph)
    , _trees()
{
    extractTreesFromNodes(nodes, _trees);
    setText( tr("Extract node") );
}

ExtractNodeUndoRedoCommand::~ExtractNodeUndoRedoCommand()
{
}

void
ExtractNodeUndoRedoCommand::undo()
{
    std::set<ViewerInstance* > viewers;

    for (std::list<ExtractedTree>::iterator it = _trees.begin(); it != _trees.end(); ++it) {
        NodeGuiPtr output = it->output.node.lock();
        ///Connect and move output
        for (std::list<std::pair<int, NodeWPtr> >::iterator it2 = it->output.outputs.begin(); it2 != it->output.outputs.end(); ++it2) {
            NodePtr node = it2->second.lock();
            if (!node) {
                continue;
            }
            node->disconnectInput(it2->first);
            node->connectInput(output->getNode(), it2->first);
        }

        QPointF curPos = output->getPos_mt_safe();
        output->refreshPosition(curPos.x() - 200, curPos.y(), true);

        ///Connect and move inputs
        for (std::list<ExtractedInput>::iterator it2 = it->inputs.begin(); it2 != it->inputs.end(); ++it2) {
            NodeGuiPtr input = it2->node.lock();
            for (U32 i  =  0; i < it2->inputs.size(); ++i) {
                if ( it2->inputs[i].lock() ) {
                    input->getNode()->connectInput(it2->inputs[i].lock(), i);
                }
            }

            if (input != output) {
                QPointF curPos = input->getPos_mt_safe();
                input->refreshPosition(curPos.x() - 200, curPos.y(), true);
            }
        }

        ///Move all other nodes

        for (std::list<NodeGuiWPtr>::iterator it2 = it->inbetweenNodes.begin(); it2 != it->inbetweenNodes.end(); ++it2) {
            NodeGuiPtr node = it2->lock();
            QPointF curPos = node->getPos_mt_safe();
            node->refreshPosition(curPos.x() - 200, curPos.y(), true);
        }

        std::list<ViewerInstance* > tmp;
        output->getNode()->hasViewersConnected(&tmp);

        for (std::list<ViewerInstance* >::iterator it2 = tmp.begin(); it2 != tmp.end(); ++it2) {
            viewers.insert(*it2);
        }
    }

    for (std::set<ViewerInstance* >::iterator it = viewers.begin(); it != viewers.end(); ++it) {
        (*it)->renderCurrentFrame(true);
    }

    _graph->getGui()->getApp()->triggerAutoSave();
} // ExtractNodeUndoRedoCommand::undo

void
ExtractNodeUndoRedoCommand::redo()
{
    std::set<ViewerInstance* > viewers;

    for (std::list<ExtractedTree>::iterator it = _trees.begin(); it != _trees.end(); ++it) {
        std::list<ViewerInstance* > tmp;
        NodeGuiPtr output = it->output.node.lock();
        output->getNode()->hasViewersConnected(&tmp);

        for (std::list<ViewerInstance* >::iterator it2 = tmp.begin(); it2 != tmp.end(); ++it2) {
            viewers.insert(*it2);
        }

        bool outputsAlreadyDisconnected = false;

        ///Reconnect outputs to the input of the input of the ExtractedInputs if inputs.size() == 1
        if ( (it->output.outputs.size() == 1) && (it->inputs.size() == 1) ) {
            const ExtractedInput& selectedInput = it->inputs.front();
            const std::vector<NodeWPtr> &inputs = selectedInput.inputs;
            NodeGuiPtr selectedInputNode = selectedInput.node.lock();
            NodePtr inputToConnectTo;
            for (U32 i = 0; i < inputs.size(); ++i) {
                if ( inputs[i].lock() && !selectedInputNode->getNode()->getEffectInstance()->isInputOptional(i) &&
                     !selectedInputNode->getNode()->getEffectInstance()->isInputRotoBrush(i) ) {
                    inputToConnectTo = inputs[i].lock();
                    break;
                }
            }

            if (inputToConnectTo) {
                for (std::list<std::pair<int, NodeWPtr> >::iterator it2 = it->output.outputs.begin(); it2 != it->output.outputs.end(); ++it2) {
                    NodePtr node = it2->second.lock();
                    if (!node) {
                        continue;
                    }
                    node->disconnectInput(it2->first);
                    node->connectInput(inputToConnectTo, it2->first);
                }
                outputsAlreadyDisconnected = true;
            }
        }

        ///Disconnect and move output
        if (!outputsAlreadyDisconnected) {
            for (std::list<std::pair<int, NodeWPtr> >::iterator it2 = it->output.outputs.begin(); it2 != it->output.outputs.end(); ++it2) {
                NodePtr node = it2->second.lock();
                if (node) {
                    node->disconnectInput(it2->first);
                }
            }
        }

        QPointF curPos = output->getPos_mt_safe();
        output->refreshPosition(curPos.x() + 200, curPos.y(), true);


        ///Disconnect and move inputs
        for (std::list<ExtractedInput>::iterator it2 = it->inputs.begin(); it2 != it->inputs.end(); ++it2) {
            NodeGuiPtr node = it2->node.lock();
            for (U32 i  =  0; i < it2->inputs.size(); ++i) {
                if ( it2->inputs[i].lock() ) {
                    node->getNode()->disconnectInput(i);
                }
            }
            if (node != output) {
                QPointF curPos = node->getPos_mt_safe();
                node->refreshPosition(curPos.x() + 200, curPos.y(), true);
            }
        }

        ///Move all other nodes

        for (std::list<NodeGuiWPtr>::iterator it2 = it->inbetweenNodes.begin(); it2 != it->inbetweenNodes.end(); ++it2) {
            NodeGuiPtr node = it2->lock();
            QPointF curPos = node->getPos_mt_safe();
            node->refreshPosition(curPos.x() + 200, curPos.y(), true);
        }
    }

    for (std::set<ViewerInstance* >::iterator it = viewers.begin(); it != viewers.end(); ++it) {
        (*it)->renderCurrentFrame(true);
    }

    _graph->getGui()->getApp()->triggerAutoSave();
} // ExtractNodeUndoRedoCommand::redo

GroupFromSelectionCommand::GroupFromSelectionCommand(NodeGraph* graph,
                                                     const NodesGuiList & nodes)
    : QUndoCommand()
    , _graph(graph)
    , _group()
    , _firstRedoCalled(false)
    , _isRedone(false)
{
    setText( tr("Group from selection") );

    assert( !nodes.empty() );
    for (NodesGuiList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        _originalNodes.push_back(*it);
    }

}

GroupFromSelectionCommand::~GroupFromSelectionCommand()
{
}

void
GroupFromSelectionCommand::undo()
{
    std::list<NodeGuiPtr> nodesToSelect;

    for (OutputLinksMap::iterator it = _outputLinks.begin(); it != _outputLinks.end(); ++it) {
        NodePtr outputNode = it->first.lock();
        if (!outputNode) {
            continue;
        }
        NodePtr input = it->second.inputNode.lock();
        if (!input) {
            continue;
        }
        outputNode->replaceInput(input, it->second.inputIdx);
    }


    for (std::list<NodeGuiWPtr>::iterator it = _originalNodes.begin(); it != _originalNodes.end(); ++it) {
        NodeGuiPtr node = it->lock();
        if (node) {
            node->getNode()->activate(NodesList(), true, false);
            nodesToSelect.push_back(node);
        }
    }
    _graph->setSelection(nodesToSelect);
    _group.lock()->getNode()->deactivate(NodesList(),
                                         true,
                                         false,
                                         true,
                                         true,
                                         false);

    _isRedone = false;
}

void
GroupFromSelectionCommand::redo()
{
    // The group position will be at the centroid of all selected nodes
    QPointF groupPosition;

    NodesGuiList originalNodes;
    for (std::list<NodeGuiWPtr>::const_iterator it = _originalNodes.begin(); it != _originalNodes.end(); ++it) {
        NodeGuiPtr n = it->lock();
        if (!n) {
            continue;
        }
        originalNodes.push_back(n);

        QPointF nodePos = n->getPos_mt_safe();
        groupPosition += nodePos;
    }

    unsigned sz = originalNodes.size();
    if (sz) {
        groupPosition.rx() /= sz;
        groupPosition.ry() /= sz;
    }

    NodeGroupPtr isGrp;
    if (_firstRedoCalled) {
        isGrp = std::dynamic_pointer_cast<NodeGroup>(_group.lock()->getNode()->getEffectInstance());
        assert(isGrp);
        if (!isGrp) {
            throw std::logic_error("GroupFromSelectionCommand::redo()");
        }
    } else {
        NodesList internalNewNodes;
        // Create the actual Group node
        CreateNodeArgs groupArgs( PLUGINID_NATRON_GROUP, _graph->getGroup() );
        groupArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        groupArgs.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
        groupArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        groupArgs.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);

        NodePtr containerNode = _graph->getGui()->getApp()->createNode(groupArgs);

        isGrp = std::dynamic_pointer_cast<NodeGroup>( containerNode->getEffectInstance()->shared_from_this() );
        assert(isGrp);
        if (!isGrp) {
            throw std::logic_error("GroupFromSelectionCommand::redo()");
        }
        NodeGuiIPtr container_i = containerNode->getNodeGui();
        assert(container_i);
        _group = std::dynamic_pointer_cast<NodeGui>(container_i);
        assert( _group.lock() );

        // Set the position of the group
        container_i->setPosition( groupPosition.x(), groupPosition.y() );

        // Copy all the selected nodes and paste them in the newly created Group
        std::list<std::pair<std::string, NodeGuiPtr> > newNodes;
        _graph->copyNodesAndCreateInGroup(originalNodes, isGrp, newNodes);

        for (std::list<std::pair<std::string, NodeGuiPtr> >::iterator it = newNodes.begin(); it != newNodes.end(); ++it) {
            internalNewNodes.push_back( it->second->getNode() );
        }

        // Extract all trees from these new nodes
        std::list<Project::NodesTree> trees;
        Project::extractTreesFromNodes(internalNewNodes, trees);
        bool hasCreatedOutput = false;
        int inputNb = 0;
        for (std::list<Project::NodesTree>::iterator it = trees.begin(); it != trees.end(); ++it) {
            NodeGuiPtr foundOriginalOutputNode;
            if (!it->output.node->isActivated()) {
                continue;
            }
            const std::string outputNodeName = it->output.node->getScriptName_mt_safe();
            for (NodesGuiList::const_iterator it3 = originalNodes.begin(); it3 != originalNodes.end(); ++it3) {
                NodePtr n = (*it3)->getNode();
                if ( n && n->isActivated() && n->getScriptName_mt_safe() == outputNodeName) {
                    foundOriginalOutputNode = *it3;
                    break;
                }
            }
            assert(foundOriginalOutputNode);
            if (!foundOriginalOutputNode) {
                continue;
            }

            // For each input node of each tree branch within the group, add a Input node in input of that branch
            // to actually create the input on the Group node
            for (std::list<Project::TreeInput>::iterator it2 = it->inputs.begin(); it2 != it->inputs.end(); ++it2) {
                // Find the equivalent node in the original nodes and see which inputs we need to create
                NodeGuiPtr foundOriginalNode;
                if (!it2->node->isActivated()) {
                    continue;
                }
                const std::string originalNodeName = it2->node->getScriptName_mt_safe();
                for (NodesGuiList::const_iterator it3 = originalNodes.begin(); it3 != originalNodes.end(); ++it3) {
                    NodePtr n = (*it3)->getNode();
                    if ( n && n->isActivated() && n->getScriptName_mt_safe() == originalNodeName) {
                        foundOriginalNode = *it3;
                        break;
                    }
                }
                assert(foundOriginalNode);
                if (!foundOriginalNode) {
                    continue;
                }

                // For each connected input of the original node, create a corresponding Input node with the appropriate name
                NodePtr originalNodeInternal = foundOriginalNode->getNode();
                const std::vector<NodeWPtr>& originalNodeInputs = originalNodeInternal->getInputs();
                for (std::size_t i = 0; i < originalNodeInputs.size(); ++i) {
                    if ( !originalNodeInternal->isInputVisible(i) ) {
                        // do not create an Input node for an invisible input (example: Shadertoy node, which hides its inputs)
                        continue;
                    }
                    NodePtr originalInput = originalNodeInputs[i].lock();

                    //Create an input node corresponding to this input
                    CreateNodeArgs args(PLUGINID_NATRON_INPUT, isGrp);
                    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
                    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
                    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);


                    NodePtr input = _graph->getGui()->getApp()->createNode(args);
                    assert(input);
                    std::string inputLabel = originalNodeInternal->getLabel() + '_' + originalNodeInternal->getInputLabel(i);
                    input->setLabel(inputLabel);

                    // Position the input node correctly
                    double offsetX, offsetY;


                    double originalX, originalY;
                    foundOriginalNode->getPosition(&originalX, &originalY);
                    if (originalInput) {
                        double inputX, inputY;
                        originalInput->getPosition(&inputX, &inputY);
                        offsetX = inputX - originalX;
                        offsetY = inputY - originalY;
                    } else {
                        offsetX = originalX;
                        offsetY = originalY;
                    }
                    double thisInputX, thisInputY;
                    it2->node->getPosition(&thisInputX, &thisInputY);
                    
                    thisInputX += offsetX;
                    thisInputY += offsetY;

                    input->setPosition(thisInputX, thisInputY);

                    it2->node->connectInput(input, i);
                    if (originalInput) {
                        isGrp->getNode()->connectInput(originalInput, inputNb);
                    }
                    ++inputNb;

                } // for all node's inputs
            } // for all inputs in the tree
            //Create only a single output

            if (!hasCreatedOutput) {
                CreateNodeArgs args(PLUGINID_NATRON_OUTPUT, isGrp);
                args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
                args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
                args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
                NodePtr output = _graph->getGui()->getApp()->createNode(args);
                try {
                    output->setScriptName("Output");
                } catch (...) {
                }

                assert(output);

                double thisNodeX, thisNodeY;
                it->output.node->getPosition(&thisNodeX, &thisNodeY);
                double thisNodeW, thisNodeH;
                it->output.node->getSize(&thisNodeW, &thisNodeH);

                thisNodeY += thisNodeH * 2;

                output->setPosition(thisNodeX, thisNodeY);

                output->connectInput(it->output.node, 0);

                hasCreatedOutput = true;
            }
            // Connect all outputs of the original node to the new Group
            NodesWList originalOutputs;
            foundOriginalOutputNode->getNode()->getOutputs_mt_safe(originalOutputs);
            for (NodesWList::const_iterator it2 = originalOutputs.begin(); it2 != originalOutputs.end(); ++it2) {
                NodePtr output = it2->lock();
                if (!output) {
                    continue;
                }
                int inputIdx = output->getInputIndex(foundOriginalOutputNode->getNode().get());
                assert(inputIdx != -1);
                output->replaceInput(isGrp->getNode(), inputIdx);
                OutputLink& link = _outputLinks[*it2];
                link.inputNode = foundOriginalOutputNode->getNode();
                link.inputIdx = inputIdx;
            }
        } // for all trees
    } // !_firstRedoCalled

    if (_firstRedoCalled) {
        for (OutputLinksMap::iterator it = _outputLinks.begin(); it != _outputLinks.end(); ++it) {
            NodePtr outputNode = it->first.lock();
            if (!outputNode) {
                continue;
            }
            outputNode->replaceInput(isGrp->getNode(), it->second.inputIdx);
        }
    }



    for (NodesGuiList::iterator it = originalNodes.begin(); it != originalNodes.end(); ++it) {
        (*it)->getNode()->deactivate(NodesList(),
                                     true,
                                     false,
                                     true,
                                     false,
                                     false);
    }

    std::list<NodeGuiPtr> nodesToSelect;
    NodeGuiPtr nodeGroup = _group.lock();
    if (_firstRedoCalled && nodeGroup) {
        nodeGroup->getNode()->activate(NodesList(), true, false);
    }
    if (nodeGroup) {
        nodesToSelect.push_back(nodeGroup);
    }
    _graph->setSelection(nodesToSelect);


    std::list<ViewerInstance*> viewers;
    _graph->getGroup()->getViewers(&viewers);
    for (std::list<ViewerInstance*>::iterator it = viewers.begin(); it != viewers.end(); ++it) {
        (*it)->renderCurrentFrame(true);
    }
    NodeGraphI* graph_i = isGrp->getNodeGraph();
    assert(graph_i);
    NodeGraph* graph = dynamic_cast<NodeGraph*>(graph_i);
    assert(graph);
    if (graph) {
        graph->centerOnAllNodes();
    }
    _firstRedoCalled = true;
    _isRedone = true;
}

InlineGroupCommand::InlineGroupCommand(NodeGraph* graph,
                                       const std::list<NodeGuiPtr> & groupNodes)
    : QUndoCommand()
    , _graph(graph)
    , _groupNodes()
    , _firstRedoCalled(false)
{
    setText( tr("Inline group(s)") );

    for (std::list<NodeGuiPtr> ::const_iterator it = groupNodes.begin(); it != groupNodes.end(); ++it) {
        NodeGroup* group = (*it)->getNode()->isEffectGroup();
        assert(group);
        if (!group) {
            continue;
        }
        InlinedGroup expandedGroup;

        /*
         * First-off copy all the nodes within the group, except the Inputs and Output nodes
         */
        NodeClipBoard cb;
        NodesList nodes = group->getNodes();
        std::vector<NodePtr> groupInputs;
        NodePtr groupOutput = group->getOutputNode(true);
        group->getInputs(&groupInputs, true);

        std::list<NodeGuiPtr> nodesToCopy;
        for (NodesList::iterator it2 = nodes.begin(); it2 != nodes.end(); ++it2) {
            GroupInput* inp = dynamic_cast<GroupInput*>( (*it2)->getEffectInstance().get() );
            GroupOutput* output = dynamic_cast<GroupOutput*>( (*it2)->getEffectInstance().get() );
            if ( !inp && !output && !(*it2)->getParentMultiInstance() ) {
                NodeGuiIPtr gui_i = (*it2)->getNodeGui();
                assert(gui_i);
                NodeGuiPtr nodeGui = std::dynamic_pointer_cast<NodeGui>(gui_i);
                assert(nodeGui);
                nodesToCopy.push_back(nodeGui);
            }
        }

        {
            NodeGraphI *graph_i = group->getNodeGraph();
            assert(graph_i);
            {
                NodeGraph* thisGroupGraph = dynamic_cast<NodeGraph*>(graph_i);
                assert(thisGroupGraph);
                if (thisGroupGraph) {
                    thisGroupGraph->copyNodes(nodesToCopy, cb);
                }
            }
        }
        std::list<std::pair<std::string, NodeGuiPtr> > newNodes;
        _graph->pasteCliboard(cb, &newNodes);

        NodeGuiIPtr groupGui_i = group->getNode()->getNodeGui();
        assert(groupGui_i);
        NodeGuiPtr groupGui = std::dynamic_pointer_cast<NodeGui>(groupGui_i);
        assert(groupGui);
        expandedGroup.group = groupGui;

        QPointF groupNodePos = groupGui->mapToScene( groupGui->mapFromParent( groupGui->getPos_mt_safe() ) );

        //This is the BBox of the new inlined nodes
        RectD bbox;
        bbox.setupInfinity();
        for (std::list<std::pair<std::string, NodeGuiPtr> >::iterator it2 = newNodes.begin();
             it2 != newNodes.end(); ++it2) {
            QPointF p = it2->second->mapToScene( it2->second->mapFromParent( it2->second->getPos_mt_safe() ) );
            bbox.x1 = std::min( bbox.x1, p.x() );
            bbox.x2 = std::max( bbox.x2, p.x() );

            //Qt coord system is top down
            bbox.y1 = std::min( bbox.y1, p.y() );
            bbox.y2 = std::max( bbox.y2, p.y() );
            expandedGroup.inlinedNodes.push_back(it2->second);
        }

        QPointF bboxCenter( (bbox.x1 + bbox.x2) / 2., (bbox.y1 + bbox.y2) / 2. );
        // Remember the links from the Group node we are expending to its inputs and outputs

        // This is the y coord. of the bottom-most input
        double inputY = std::numeric_limits<int>::min();
        int maxInputs = group->getNode()->getNInputs();
        assert( maxInputs == (int)groupInputs.size() );
        for (int i = 0; i < maxInputs; ++i) {
            NodePtr input = group->getNode()->getInput(i);
            if (input) {
                NodeToConnect ntc;

                assert(groupInputs[i]);
                std::map<NodePtr, int> outputConnected;
                groupInputs[i]->getOutputsConnectedToThisNode(&outputConnected);

                NodeGuiIPtr inputGui_i = input->getNodeGui();
                assert(inputGui_i);
                NodeGuiPtr inputGui = std::dynamic_pointer_cast<NodeGui>(inputGui_i);
                assert(inputGui);
                ntc.input = inputGui;

                QPointF p = inputGui->mapToScene( inputGui->mapFromParent( inputGui->getPos_mt_safe() ) );
                if (p.y() > inputY) {
                    inputY = p.y();
                }

                for (std::map<NodePtr, int>::iterator it2 = outputConnected.begin(); it2 != outputConnected.end(); ++it2) {
                    NodeGuiPtr outputGui;
                    ///Find the new node that was inlined, based on the script name of the old node in the group
                    for (std::list<std::pair<std::string, NodeGuiPtr> >::iterator it3 = newNodes.begin(); it3 != newNodes.end(); ++it3) {
                        if ( it3->first == it2->first->getScriptName() ) {
                            outputGui = it3->second;
                            break;
                        }
                    }
                    assert(outputGui);
                    ntc.outputs.insert( std::make_pair(outputGui, it2->second) );
                }

                expandedGroup.connections[i] = ntc;
            }
        }

        std::list<NodeGuiPtr> outputsConnectedToGroup;
        QPointF firstInputPos;

        // This is the y coord of the top most output
        double outputY = std::numeric_limits<int>::max();
        if (groupOutput) {
            NodePtr groupOutputInput = groupOutput->getInput(0);
            if (groupOutputInput) {
                NodeToConnect outputConnection;
                NodeGuiPtr inputGui;
                ///Find the new node that was inlined, based on the script name of the old node in the group
                for (std::list<std::pair<std::string, NodeGuiPtr> >::iterator it3 = newNodes.begin(); it3 != newNodes.end(); ++it3) {
                    if ( it3->first == groupOutputInput->getScriptName() ) {
                        inputGui = it3->second;
                        break;
                    }
                }

                if (!inputGui) {
                    continue;
                }
                firstInputPos = inputGui->mapToScene( inputGui->mapFromParent( inputGui->getPos_mt_safe() ) );
                outputConnection.input = inputGui;

                std::map<NodePtr, int> outputConnected;
                group->getNode()->getOutputsConnectedToThisNode(&outputConnected);
                for (std::map<NodePtr, int>::iterator it2 = outputConnected.begin(); it2 != outputConnected.end(); ++it2) {
                    NodeGuiIPtr outputGui_i = it2->first->getNodeGui();
                    assert(outputGui_i);
                    NodeGuiPtr outputGui = std::dynamic_pointer_cast<NodeGui>(outputGui_i);
                    assert(outputGui);
                    outputsConnectedToGroup.push_back(outputGui);

                    QPointF p = outputGui->mapToScene( outputGui->mapFromParent( outputGui->getPos_mt_safe() ) );
                    if (p.y() < outputY) {
                        outputY = p.y();
                    }
                    outputConnection.outputs.insert( std::make_pair(outputGui, it2->second) );
                }
                expandedGroup.connections[-1] = outputConnection;
            }
        }

        // If there is no output to the group, the output is considered to be infinite (so we don't move any node)
        if (outputY == std::numeric_limits<int>::min()) {
            outputY = std::numeric_limits<int>::max();
        }

        const double ySpaceAvailable = outputY  - inputY;
        const double ySpaceNeeded = bbox.y2 - bbox.y1 + TO_DPIX(100);

        //  Move recursively the outputs of the group nodes so that it does not overlap the inlining of the group
        QRectF rectToClear(bbox.x1, bbox.y1, bbox.x2 - bbox.x1, ySpaceNeeded - ySpaceAvailable);
        for (std::list<NodeGuiPtr> ::iterator it2 = outputsConnectedToGroup.begin();
             it2 != outputsConnectedToGroup.end(); ++it2) {
            (*it2)->moveBelowPositionRecursively(rectToClear);
        }


        // Move all created nodes by this delta to fit in the space we've just made
        for (std::list<std::pair<std::string, NodeGuiPtr> >::iterator it2 = newNodes.begin();
             it2 != newNodes.end(); ++it2) {
            QPointF p = it2->second->mapToScene( it2->second->mapFromParent( it2->second->getPos_mt_safe() ) );
            QPointF delta = p - bboxCenter;
            p = groupNodePos + delta;
            p = it2->second->mapToParent( it2->second->mapFromScene(p) );
            it2->second->setPosition( p.x(), p.y() );
        }


        _groupNodes.push_back(expandedGroup);
    } // for (std::list<NodeGuiPtr> ::const_iterator it = groupNodes.begin(); it != groupNodes.end(); ++it) {
}

InlineGroupCommand::~InlineGroupCommand()
{
}

void
InlineGroupCommand::undo()
{
    std::set<ViewerInstance*> viewers;
    std::list<NodeGuiPtr> nodesToSelect;

    for (std::list<InlinedGroup>::iterator it = _groupNodes.begin(); it != _groupNodes.end(); ++it) {
        NodeGuiPtr groupNode = it->group.lock();
        if (groupNode) {
            groupNode->getNode()->activate(NodesList(), true, false);
            std::list<ViewerInstance*> connectedViewers;
            groupNode->getNode()->hasViewersConnected(&connectedViewers);
            for (std::list<ViewerInstance*>::iterator it2 = connectedViewers.begin(); it2 != connectedViewers.end(); ++it2) {
                viewers.insert(*it2);
            }
            for (std::list<NodeGuiWPtr>::iterator it2 = it->inlinedNodes.begin();
                 it2 != it->inlinedNodes.end(); ++it2) {
                NodeGuiPtr node = (*it2).lock();
                if (node) {
                    node->getNode()->deactivate(NodesList(), false, false, true, false, false);
                }
            }
        }
        nodesToSelect.push_back(groupNode);
    }
    _graph->setSelection(nodesToSelect);
    for (std::set<ViewerInstance*>::iterator it = viewers.begin(); it != viewers.end(); ++it) {
        (*it)->renderCurrentFrame(true);
    }

    _graph->getGui()->getApp()->triggerAutoSave();
}

void
InlineGroupCommand::redo()
{
    std::set<ViewerInstance*> viewers;
    std::list<NodeGuiPtr> nodesToSelect;

    for (std::list<InlinedGroup>::iterator it = _groupNodes.begin(); it != _groupNodes.end(); ++it) {
        NodeGuiPtr groupNode = it->group.lock();
        if (groupNode) {
            std::list<ViewerInstance*> connectedViewers;
            groupNode->getNode()->hasViewersConnected(&connectedViewers);
            for (std::list<ViewerInstance*>::iterator it2 = connectedViewers.begin(); it2 != connectedViewers.end(); ++it2) {
                viewers.insert(*it2);
            }
            groupNode->getNode()->deactivate(NodesList(), true, false, true, false, false);
            for (std::list<NodeGuiWPtr>::iterator it2 = it->inlinedNodes.begin();
                 it2 != it->inlinedNodes.end(); ++it2) {
                NodeGuiPtr node = (*it2).lock();
                if (node) {
                    if  (_firstRedoCalled) {
                        node->getNode()->activate(NodesList(), false, false);
                    }
                    nodesToSelect.push_back(node);
                }
            }

            for (std::map<int, NodeToConnect>::iterator it2 = it->connections.begin(); it2 != it->connections.end(); ++it2) {
                NodeGuiPtr input = it2->second.input.lock();
                if (!input) {
                    continue;
                }
                for (NodeGuisMap::iterator it3 = it2->second.outputs.begin();
                     it3 != it2->second.outputs.end(); ++it3) {
                    NodeGuiPtr node = it3->first.lock();
                    if (node) {
                        node->getNode()->disconnectInput(it3->second);
                        NodeCollection::connectNodes(it3->second, input->getNode(), node->getNode(), false);
                    }
                }
            }
        }
    }
    _graph->setSelection(nodesToSelect);

    for (std::set<ViewerInstance*>::iterator it = viewers.begin(); it != viewers.end(); ++it) {
        (*it)->renderCurrentFrame(true);
    }
    _graph->getGui()->getApp()->triggerAutoSave();
    _firstRedoCalled = true;
}

NATRON_NAMESPACE_EXIT
