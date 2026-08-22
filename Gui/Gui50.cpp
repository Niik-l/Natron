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

#include "Gui.h"

#include <cassert>
#include <algorithm> // min, max
#include <map>
#include <list>
#include <locale>
#include <utility>
#include <stdexcept>

#include <QtGlobal> // for Q_OS_*
#include <QDebug>
#include <QThread>
#include <QTimer>

#include <QApplication> // qApp
GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QMimeData>
#include <QKeyEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
#include <QMenuBar>
#include <QFileDialog>    // Megascans asset loader: folder pick
#include <QInputDialog>   // Megascans asset loader: LOD pick
#include <QDir>
#include <QDirIterator>   // Megascans: recursive mesh discovery (Var1/../VarN)
#include <QJsonDocument>  // Megascans: tris / variation / lod from the asset json
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QToolButton>
#include <QProgressDialog>
#include <QClipboard>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QThread>
#include <QTabBar>
#include <QTextEdit>
#include <QLineEdit>
#include <QCursor>
#include <QCheckBox>
#include <QTreeView>
#ifdef Q_WS_X11
#include <QX11Info>
#endif

#include "Global/QtCompat.h"

#include "Engine/CreateNodeArgs.h"
#include "Engine/GroupOutput.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h" // NodesList, NodeCollection
#include "Engine/Project.h"
#include "Engine/KnobSerialization.h"
#include "Engine/FileSystemModel.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"

#include "Gui/ActionShortcuts.h"
#include "Gui/CurveEditor.h"
#include "Gui/CurveWidget.h"
#include "Gui/ComboBox.h"
#include "Gui/DockablePanel.h"
#include "Gui/DopeSheetEditor.h"
#include "Gui/ExportGroupTemplateDialog.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h" // appPTR
#include "Gui/GuiPrivate.h"
#include "Gui/Viewport3D.h"
#include "Gui/Viewport3DTab.h"
#include "Gui/KnobWidgetDnD.h"
#include "Gui/GuiMacros.h"
#include "Gui/LogWindow.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/ProgressPanel.h"
#include "Gui/RightClickableWidget.h"
#include "Gui/ScriptEditor.h"
#include "Gui/SpinBox.h"
#include "Gui/TabWidget.h"
#include "Gui/PreferencesPanel.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"
#include "Gui/SequenceFileDialog.h"
#include "Gui/PropertiesBinWrapper.h"
#include "Gui/Histogram.h"

#if defined(Q_OS_WIN)
#include <winuser.h>
#include <wingdi.h>
#endif


NATRON_NAMESPACE_ENTER


void
Gui::setUndoRedoStackLimit(int limit)
{
    _imp->_nodeGraphArea->setUndoRedoStackLimit(limit);
}

void
Gui::onShowLogOnMainThreadReceived()
{
    std::list<LogEntry> log;
    appPTR->getErrorLog_mt_safe(&log);
    assert(_imp->_errorLog);
    _imp->_errorLog->displayLog(log);

    if (!_imp->_errorLog->isVisible()) {
        _imp->_errorLog->show();
        // no need to raise(), because:
        // - the log window is Qt::WindowStaysOnTopHint
        // - raising a window does not work in general:
        // https://forum.qt.io/topic/6032/bring-window-to-front-raise-show-activatewindow-don-t-work-on-windows/12
    }
}

void
Gui::showErrorLog()
{
    if (QThread::currentThread() == qApp->thread()) {
        onShowLogOnMainThreadReceived();
    } else {
        Q_EMIT s_showLogOnMainThread();
    }
}

void
Gui::createNodeViewerInterface(const NodeGuiPtr& n)
{
    QMutexLocker l(&_imp->_viewerTabsMutex);

    for (std::list<ViewerTab*>::iterator it = _imp->_viewerTabs.begin(); it != _imp->_viewerTabs.end(); ++it) {
        (*it)->createNodeViewerInterface(n);
    }
}

void
Gui::removeNodeViewerInterface(const NodeGuiPtr& n,
                               bool permanently)
{
    QMutexLocker l(&_imp->_viewerTabsMutex);

    for (std::list<ViewerTab*>::iterator it = _imp->_viewerTabs.begin(); it != _imp->_viewerTabs.end(); ++it) {
        (*it)->removeNodeViewerInterface(n, permanently, false /*setNewInterface*/);
    }
}

void
Gui::setNodeViewerInterface(const NodeGuiPtr& n)
{
    QMutexLocker l(&_imp->_viewerTabsMutex);

    for (std::list<ViewerTab*>::iterator it = _imp->_viewerTabs.begin(); it != _imp->_viewerTabs.end(); ++it) {
        (*it)->setPluginViewerInterface(n);
    }
}

void
Gui::progressStart(const NodePtr& node,
                   const std::string &message,
                   const std::string &messageid,
                   bool canCancel)
{
    _imp->_progressPanel->progressStart(node, message, messageid, canCancel);
}

void
Gui::progressEnd(const NodePtr& node)
{
    _imp->_progressPanel->endTask(node);
}

bool
Gui::progressUpdate(const NodePtr& node,
                    double t)
{
    return _imp->_progressPanel->updateTask(node, t);
}

void
Gui::onMaxVisibleDockablePanelChanged(int maxPanels)
{
    assert(maxPanels >= 0);
    if (maxPanels == 0) {
        return;
    }
    while ( (int)_imp->openedPanels.size() > maxPanels ) {
        std::list<DockablePanel*>::reverse_iterator it = _imp->openedPanels.rbegin();
        (*it)->closePanel();
    }
    _imp->_maxPanelsOpenedSpinBox->setValue(maxPanels);
}

void
Gui::onMaxPanelsSpinBoxValueChanged(double val)
{
    appPTR->getCurrentSettings()->setMaxPanelsOpened( (int)val );
}

void
Gui::clearAllVisiblePanels()
{
    // close panels one by one, since closing a panel updates the openedPanels list.
    while ( !_imp->openedPanels.empty() ) {
        bool foundNonFloating = false;

        // close one panel at a time - this changes the openedPanel list, so we must break the loop
        for (std::list<DockablePanel*>::iterator it = _imp->openedPanels.begin(); it != _imp->openedPanels.end(); ++it) {
            if ( !(*it)->isFloating() ) {
                (*it)->setClosed(true);
                foundNonFloating = true;
                break;
            }
        }

        // no panel was closed
        if (!foundNonFloating) {
            break;
        }
    }
    getApp()->redrawAllViewers();
}

void
Gui::minimizeMaximizeAllPanels(bool clicked)
{
    for (std::list<DockablePanel*>::iterator it = _imp->openedPanels.begin(); it != _imp->openedPanels.end(); ++it) {
        if (clicked) {
            if ( !(*it)->isMinimized() ) {
                (*it)->minimizeOrMaximize(true);
            }
        } else {
            if ( (*it)->isMinimized() ) {
                (*it)->minimizeOrMaximize(false);
            }
        }
    }
    getApp()->redrawAllViewers();
}

void
Gui::connectViewersToViewerCache()
{
    QMutexLocker l(&_imp->_viewerTabsMutex);

    for (std::list<ViewerTab*>::iterator it = _imp->_viewerTabs.begin(); it != _imp->_viewerTabs.end(); ++it) {
        (*it)->connectToViewerCache();
    }
}

void
Gui::disconnectViewersFromViewerCache()
{
    QMutexLocker l(&_imp->_viewerTabsMutex);

    for (std::list<ViewerTab*>::iterator it = _imp->_viewerTabs.begin(); it != _imp->_viewerTabs.end(); ++it) {
        (*it)->disconnectFromViewerCache();
    }
}

void
Gui::moveEvent(QMoveEvent* e)
{
    QMainWindow::moveEvent(e);
    QPoint p = pos();

    setMtSafePosition( p.x(), p.y() );
}

#if 0
bool
Gui::event(QEvent* e)
{
    switch ( e->type() ) {
    case QEvent::TabletEnterProximity:
    case QEvent::TabletLeaveProximity:
    case QEvent::TabletMove:
    case QEvent::TabletPress:
    case QEvent::TabletRelease: {
        QTabletEvent *tEvent = dynamic_cast<QTabletEvent *>(e);
        const std::list<ViewerTab*>& viewers = getViewersList();
        for (std::list<ViewerTab*>::const_iterator it = viewers.begin(); it != viewers.end(); ++it) {
            QPoint widgetPos = (*it)->mapToGlobal( (*it)->mapFromParent( (*it)->pos() ) );
            QRect r( widgetPos.x(), widgetPos.y(), (*it)->width(), (*it)->height() );
            if ( r.contains( tEvent->globalPos() ) ) {
                QTabletEvent te( tEvent->type()
                                 , mapFromGlobal( tEvent->pos() )
                                 , tEvent->globalPos()
                                 , tEvent->hiResGlobalPos()
                                 , tEvent->device()
                                 , tEvent->pointerType()
                                 , tEvent->pressure()
                                 , tEvent->xTilt()
                                 , tEvent->yTilt()
                                 , tEvent->tangentialPressure()
                                 , tEvent->rotation()
                                 , tEvent->z()
                                 , tEvent->modifiers()
                                 , tEvent->uniqueId() );
                qApp->sendEvent( (*it)->getViewer(), &te );

                return true;
            }
        }
        break;
    }
    default:
        break;
    }

    return QMainWindow::event(e);
}

#endif
void
Gui::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);

    setMtSafeWindowSize( width(), height() );
}

void
Gui::setLastKeyPressVisitedClickFocus(bool visited)
{
    _imp->keyPressEventHasVisitedFocusWidget = visited;
}

void
Gui::setLastKeyUpVisitedClickFocus(bool visited)
{
    _imp->keyUpEventHasVisitedFocusWidget = visited;
}

/// Handle the viewer keys separately: use the nativeVirtualKey so that they work
/// on any keyboard, including French AZERTY (where numbers are shifted)
int
Gui::handleNativeKeys(int key,
                      quint32 nativeScanCode,
                      quint32 nativeVirtualKey)
{
    //qDebug() << "scancode=" << nativeScanCode << "virtualkey=" << nativeVirtualKey;
    if ( !appPTR->getCurrentSettings()->viewerNumberKeys() ) {
        return key;
    }

#ifdef Q_OS_DARWIN
    // OS X virtual key codes, from
    // MacOSX10.11.sdk/System/Library/Frameworks/Carbon.framework/Frameworks/HIToolbox.framework/Headers/Events.h
    // kVK_ANSI_1                    = 0x12,
    // kVK_ANSI_2                    = 0x13,
    // kVK_ANSI_3                    = 0x14,
    // kVK_ANSI_4                    = 0x15,
    // kVK_ANSI_6                    = 0x16,
    // kVK_ANSI_5                    = 0x17,
    // kVK_ANSI_9                    = 0x19,
    // kVK_ANSI_7                    = 0x1A,
    // kVK_ANSI_8                    = 0x1C,
    // kVK_ANSI_0                    = 0x1D,
    Q_UNUSED(nativeScanCode);
    switch (nativeVirtualKey) {
    case 0x12:

        return Qt::Key_1;
    case 0x13:

        return Qt::Key_2;
    case 0x14:

        return Qt::Key_3;
    case 0x15:

        return Qt::Key_4;
    case 0x16:

        return Qt::Key_6;
    case 0x17:

        return Qt::Key_5;
    case 0x18:

        return Qt::Key_9;
    case 0x1A:

        return Qt::Key_7;
    case 0x1C:

        return Qt::Key_8;
    case 0x1D:

        return Qt::Key_0;
    }
#endif
#ifdef Q_OS_WIN
    // https://msdn.microsoft.com/en-us/library/aa299374%28v=vs.60%29.aspx
    //  48   0x30   (VK_0)              | 0 key
    //  49   0x31   (VK_1)              | 1 key
    //  50   0x32   (VK_2)              | 2 key
    //  51   0x33   (VK_3)              | 3 key
    //  52   0x34   (VK_4)              | 4 key
    //  53   0x35   (VK_5)              | 5 key
    //  54   0x36   (VK_6)              | 6 key
    //  55   0x37   (VK_7)              | 7 key
    //  56   0x38   (VK_8)              | 8 key
    //  57   0x39   (VK_9)              | 9 key
    // Windows seems to always return the same virtual key for digits, whatever the modifi
    Q_UNUSED(nativeScanCode);
    switch (nativeVirtualKey) {
    case 0x30:

        return Qt::Key_0;
    case 0x31:

        return Qt::Key_1;
    case 0x32:

        return Qt::Key_2;
    case 0x33:

        return Qt::Key_3;
    case 0x34:

        return Qt::Key_4;
    case 0x35:

        return Qt::Key_5;
    case 0x36:

        return Qt::Key_6;
    case 0x37:

        return Qt::Key_7;
    case 0x38:

        return Qt::Key_8;
    case 0x39:

        return Qt::Key_9;
    }
#endif
#if defined(Q_OS_LINUX)
    // probably only possible on Linux, since scancodes are OS-dependent
    // https://www.win.tue.nl/~aeb/linux/kbd/scancodes-1.html
    Q_UNUSED(nativeVirtualKey);
    switch (nativeScanCode) {
    case 10:

        return Qt::Key_1;
    case 11:

        return Qt::Key_2;
    case 12:

        return Qt::Key_3;
    case 13:

        return Qt::Key_4;
    case 14:

        return Qt::Key_5;
    case 15:

        return Qt::Key_6;
    case 16:

        return Qt::Key_7;
    case 17:

        return Qt::Key_8;
    case 18:

        return Qt::Key_9;
    case 19:

        return Qt::Key_0;
    }
#endif

    return key;
} // Gui::handleNativeKeys

void
Gui::keyPressEvent(QKeyEvent* e)
{
    //qDebug() << "Gui::keyPressed:" << e->text() << "modifiers:" << e->modifiers();
    if (_imp->currentPanelFocusEventRecursion > 0) {
        return;
    }

    QWidget* w = qApp->widgetAt( QCursor::pos() );
    Qt::Key key = (Qt::Key)Gui::handleNativeKeys( e->key(), e->nativeScanCode(), e->nativeVirtualKey() );
    Qt::KeyboardModifiers modifiers = e->modifiers();

    if (key == Qt::Key_Escape) {
        RightClickableWidget* panel = RightClickableWidget::isParentSettingsPanelRecursive(w);
        if (panel) {
            const DockablePanel* dock = panel->getPanel();
            if (dock) {
                const_cast<DockablePanel*>(dock)->closePanel();
            }
        }
    } else if ( (key == Qt::Key_V) && modCASIsControl(e) ) {
        // CTRL +V should have been caught by the Nodegraph if it contained a valid Natron graph.
        // We still try to check if it is a readable Python script
        QClipboard* clipboard = QApplication::clipboard();
        const QMimeData* mimedata = clipboard->mimeData();
        if ( mimedata->hasFormat( QLatin1String("text/plain") ) ) {
            QByteArray data = mimedata->data( QLatin1String("text/plain") );
            QString str = QString::fromUtf8(data);
            if ( QFile::exists(str) ) {
                QList<QUrl> urls;
                urls.push_back( QUrl::fromLocalFile(str) );
                handleOpenFilesFromUrls( urls, QCursor::pos() );
            } else {
                std::string error, output;
                if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(str.toStdString(), &error, &output) ) {
                    _imp->_scriptEditor->appendToScriptEditor( QString::fromUtf8( error.c_str() ) );
                    ensureScriptEditorVisible();
                } else if ( !output.empty() ) {
                    _imp->_scriptEditor->appendToScriptEditor( QString::fromUtf8( output.c_str() ) );
                }
            }
        } else if ( mimedata->hasUrls() ) {
            QList<QUrl> urls = mimedata->urls();
            handleOpenFilesFromUrls( urls, QCursor::pos() );
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerPrevious, modifiers, key) ) {
        if ( getNodeGraph()->getLastSelectedViewer() ) {
            getNodeGraph()->getLastSelectedViewer()->previousFrame();
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerForward, modifiers, key) ) {
        if ( getNodeGraph()->getLastSelectedViewer() ) {
            getNodeGraph()->getLastSelectedViewer()->toggleStartForward();
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerBackward, modifiers, key) ) {
        if ( getNodeGraph()->getLastSelectedViewer() ) {
            getNodeGraph()->getLastSelectedViewer()->toggleStartBackward();
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerStop, modifiers, key) ) {
        if ( getNodeGraph()->getLastSelectedViewer() ) {
            getNodeGraph()->getLastSelectedViewer()->abortRendering();
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerNext, modifiers, key) ) {
        if ( getNodeGraph()->getLastSelectedViewer() ) {
            getNodeGraph()->getLastSelectedViewer()->nextFrame();
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerFirst, modifiers, key) ) {
        if ( getNodeGraph()->getLastSelectedViewer() ) {
            getNodeGraph()->getLastSelectedViewer()->firstFrame();
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerLast, modifiers, key) ) {
        if ( getNodeGraph()->getLastSelectedViewer() ) {
            getNodeGraph()->getLastSelectedViewer()->lastFrame();
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerPrevIncr, modifiers, key) ) {
        if ( getNodeGraph()->getLastSelectedViewer() ) {
            getNodeGraph()->getLastSelectedViewer()->previousIncrement();
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerNextIncr, modifiers, key) ) {
        if ( getNodeGraph()->getLastSelectedViewer() ) {
            getNodeGraph()->getLastSelectedViewer()->nextIncrement();
        }
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerPrevKF, modifiers, key) ) {
        getApp()->goToPreviousKeyframe();
    } else if ( isKeybind(kShortcutGroupPlayer, kShortcutIDActionPlayerNextKF, modifiers, key) ) {
        getApp()->goToNextKeyframe();
    } else if ( isKeybind(kShortcutGroupNodegraph, kShortcutIDActionGraphDisableNodes, modifiers, key) ) {
        _imp->_nodeGraphArea->toggleSelectedNodesEnabled();
    } else if ( isKeybind(kShortcutGroupNodegraph, kShortcutIDActionGraphFindNode, modifiers, key) ) {
        _imp->_nodeGraphArea->popFindDialog();
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput1, modifiers, key) ) {
        connectAInput(0);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput2, modifiers, key) ) {
        connectAInput(1);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput3, modifiers, key) ) {
        connectAInput(2);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput4, modifiers, key) ) {
        connectAInput(3);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput5, modifiers, key) ) {
        connectAInput(4);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput6, modifiers, key) ) {
        connectAInput(5);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput7, modifiers, key) ) {
        connectAInput(6);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput8, modifiers, key) ) {
        connectAInput(7);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput9, modifiers, key) ) {
        connectAInput(8);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerToInput10, modifiers, key) ) {
        connectAInput(9);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput1, modifiers, key) ) {
        connectBInput(0);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput2, modifiers, key) ) {
        connectBInput(1);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput3, modifiers, key) ) {
        connectBInput(2);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput4, modifiers, key) ) {
        connectBInput(3);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput5, modifiers, key) ) {
        connectBInput(4);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput6, modifiers, key) ) {
        connectBInput(5);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput7, modifiers, key) ) {
        connectBInput(6);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput8, modifiers, key) ) {
        connectBInput(7);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput9, modifiers, key) ) {
        connectBInput(8);
    } else if ( isKeybind(kShortcutGroupGlobal, kShortcutIDActionConnectViewerBToInput10, modifiers, key) ) {
        connectBInput(9);
    } else {
        /*
         * Modifiers are always uncaught by child implementations so that we can forward them to 1 ViewerTab so that
         * plug-ins overlay interacts always get the keyDown/keyUp events to track modifiers state.
         */
        bool isModifier = key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta;
        if (isModifier) {
            const std::list<ViewerTab*>& viewers = getViewersList();
            bool viewerTabHasFocus = false;
            for (std::list<ViewerTab*>::const_iterator it = viewers.begin(); it != viewers.end(); ++it) {
                if ( (*it)->hasFocus() || (_imp->currentPanelFocus == *it) ) {
                    viewerTabHasFocus = true;
                    break;
                }
            }
            //Plug-ins did not yet receive a keyDown event for this modifier, send it
            if ((!viewers.empty() && (!viewerTabHasFocus || !_imp->keyPressEventHasVisitedFocusWidget))) {
                //Increment a recursion counter because the handler of the focus widget might toss it back to us
                ++_imp->currentPanelFocusEventRecursion;
                //If a panel as the click focus, try to send the event to it
                QKeyEvent* ev = new QKeyEvent(QEvent::KeyPress, key, modifiers);
                qApp->notify(viewers.front(), ev);
                --_imp->currentPanelFocusEventRecursion;
            }
        }

        if (_imp->currentPanelFocus && !_imp->keyPressEventHasVisitedFocusWidget) {
            //Increment a recursion counter because the handler of the focus widget might toss it back to us
            ++_imp->currentPanelFocusEventRecursion;
            //If a panel as the click focus, try to send the event to it
            QWidget* curFocusWidget = _imp->currentPanelFocus->getWidget();
            assert(curFocusWidget);
            QKeyEvent* ev = new QKeyEvent(QEvent::KeyPress, key, modifiers);
            qApp->notify(curFocusWidget, ev);
            --_imp->currentPanelFocusEventRecursion;
        } else {
            QMainWindow::keyPressEvent(e);
        }
    }
} // Gui::keyPressEvent

void
Gui::keyReleaseEvent(QKeyEvent* e)
{
    if (_imp->currentPanelFocusEventRecursion > 0) {
        return;
    }

    Qt::Key key = (Qt::Key)e->key();
    Qt::KeyboardModifiers modifiers = e->modifiers();

    /*
     * Modifiers are always uncaught by child implementations so that we can forward them to 1 ViewerTab so that
     * plug-ins overlay interacts always get the keyDown/keyUp events to track modifiers state.
     */
    bool isModifier = key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta;
    if (isModifier) {
        const std::list<ViewerTab*>& viewers = getViewersList();
        bool viewerTabHasFocus = false;
        for (std::list<ViewerTab*>::const_iterator it = viewers.begin(); it != viewers.end(); ++it) {
            if ( (*it)->hasFocus() || ( (_imp->currentPanelFocus == *it) && !_imp->keyUpEventHasVisitedFocusWidget ) ) {
                viewerTabHasFocus = true;
                break;
            }
        }
        //Plug-ins did not yet receive a keyUp event for this modifier, send it
        if (!viewers.empty() && !viewerTabHasFocus) {
            //Increment a recursion counter because the handler of the focus widget might toss it back to us
            ++_imp->currentPanelFocusEventRecursion;
            //If a panel as the click focus, try to send the event to it
            QKeyEvent* ev = new QKeyEvent(QEvent::KeyRelease, key, modifiers);
            qApp->notify(viewers.front(), ev);
            --_imp->currentPanelFocusEventRecursion;
        }
    }

    if (_imp->currentPanelFocus && !_imp->keyUpEventHasVisitedFocusWidget) {
        //Increment a recursion counter because the handler of the focus widget might toss it back to us
        ++_imp->currentPanelFocusEventRecursion;
        //If a panel as the click focus, try to send the event to it
        QWidget* curFocusWidget = _imp->currentPanelFocus->getWidget();
        assert(curFocusWidget);
        QKeyEvent* ev = new QKeyEvent(QEvent::KeyRelease, key, modifiers);
        qApp->notify(curFocusWidget, ev);
        --_imp->currentPanelFocusEventRecursion;
    } else {
        QMainWindow::keyPressEvent(e);
    }
}

TabWidget*
Gui::getAnchor() const
{
    QMutexLocker l(&_imp->_panesMutex);

    for (std::list<TabWidget*>::const_iterator it = _imp->_panes.begin(); it != _imp->_panes.end(); ++it) {
        if ( (*it)->isAnchor() ) {
            return *it;
        }
    }

    return NULL;
}

bool
Gui::isGUIFrozen() const
{
    QMutexLocker k(&_imp->_isGUIFrozenMutex);

    return _imp->_isGUIFrozen;
}

void
Gui::refreshAllTimeEvaluationParams(bool onlyTimeEvaluationKnobs)
{
    int time = getApp()->getProject()->getCurrentTime();

    for (std::list<NodeGraph*>::iterator it = _imp->_groups.begin(); it != _imp->_groups.end(); ++it) {
        (*it)->refreshNodesKnobsAtTime(true, time);
    }
    getNodeGraph()->refreshNodesKnobsAtTime(onlyTimeEvaluationKnobs, time);
}

void
Gui::onFreezeUIButtonClicked(bool clicked)
{
    {
        QMutexLocker k(&_imp->_isGUIFrozenMutex);
        if (_imp->_isGUIFrozen == clicked) {
            return;
        }
        _imp->_isGUIFrozen = clicked;
    }
    {
        QMutexLocker k(&_imp->_viewerTabsMutex);
        for (std::list<ViewerTab*>::iterator it = _imp->_viewerTabs.begin(); it != _imp->_viewerTabs.end(); ++it) {
            (*it)->setTurboButtonDown(clicked);
            if (!clicked) {
                (*it)->getViewer()->redraw(); //< overlays were disabled while frozen, redraw to make them re-appear
            }
        }
    }
    _imp->_propertiesBin->setEnabled(!clicked);

    if (!clicked) {
        int time = getApp()->getProject()->getCurrentTime();
        for (std::list<NodeGraph*>::iterator it = _imp->_groups.begin(); it != _imp->_groups.end(); ++it) {
            (*it)->refreshNodesKnobsAtTime(false, time);
        }
        getNodeGraph()->refreshNodesKnobsAtTime(false, time);
    }
}

void
Gui::addShortcut(BoundAction* action)
{
    if (_imp->_settingsGui) {
        _imp->_settingsGui->addShortcut(action);
    }
}

void
Gui::getNodesEntitledForOverlays(NodesList & nodes) const
{
    std::list<DockablePanel*> panels;
    {
        QMutexLocker k(&_imp->openedPanelsMutex);
        panels = _imp->openedPanels;
    }

    for (std::list<DockablePanel*>::const_iterator it = panels.begin();
         it != panels.end(); ++it) {
        NodeSettingsPanel* panel = dynamic_cast<NodeSettingsPanel*>(*it);
        if (!panel) {
            continue;
        }
        NodeGuiPtr node = panel->getNode();
        NodePtr internalNode = node->getNode();
        if (node && internalNode) {
            if ( internalNode->shouldDrawOverlay() ) {
                nodes.push_back( node->getNode() );
            }
        }
    }
}

void
Gui::redrawAllViewers()
{
    QMutexLocker k(&_imp->_viewerTabsMutex);

    for (std::list<ViewerTab*>::const_iterator it = _imp->_viewerTabs.begin(); it != _imp->_viewerTabs.end(); ++it) {
        if ( (*it)->isVisible() ) {
            (*it)->getViewer()->redraw();
        }
    }

    // On-demand rendering for the 3D viewports: same central trigger the 2D viewers
    // use, so any UI action that refreshes a 2D viewer also repaints the 3D view — no
    // 30fps polling. requestRedraw() is a no-op when that viewport is paused.
    redraw3DViewports();
}

void
Gui::redraw3DViewports()
{
    // On-demand repaint of the 3D viewport(s) only — does NOT touch the 2D viewers, so
    // it's cheap enough to call on every property-panel knob edit (those don't route
    // through redrawAllViewers()). requestRedraw() is a no-op while a viewport is paused,
    // and getViewport3Ds_mt_safe() is empty when no 3D viewport is open → near-zero cost.
    const std::list<Viewport3DTab*> viewport3Ds = getViewport3Ds_mt_safe();
    for (std::list<Viewport3DTab*>::const_iterator it = viewport3Ds.begin(); it != viewport3Ds.end(); ++it) {
        if ( (*it)->isVisible() && (*it)->getViewport() ) {
            (*it)->getViewport()->requestRedraw();
        }
    }
}

void
Gui::renderAllViewers(bool canAbort)
{
    assert( QThread::currentThread() == qApp->thread() );
    for (std::list<ViewerTab*>::const_iterator it = _imp->_viewerTabs.begin(); it != _imp->_viewerTabs.end(); ++it) {
        if ( (*it)->isVisible() ) {
            (*it)->getInternalNode()->renderCurrentFrame(canAbort);
        }
    }
}

void
Gui::abortAllViewers()
{
    assert( QThread::currentThread() == qApp->thread() );
    for (std::list<ViewerTab*>::const_iterator it = _imp->_viewerTabs.begin(); it != _imp->_viewerTabs.end(); ++it) {
        if ( (*it)->isVisible() ) {
            (*it)->getInternalNode()->getNode()->abortAnyProcessing_non_blocking();
        }
    }
}

void
Gui::toggleAutoHideGraphInputs()
{
    _imp->_nodeGraphArea->toggleAutoHideInputs(false);
}

void
Gui::centerAllNodeGraphsWithTimer()
{
    QTimer::singleShot( 25, _imp->_nodeGraphArea, SLOT(centerOnAllNodes()) );

    for (std::list<NodeGraph*>::iterator it = _imp->_groups.begin(); it != _imp->_groups.end(); ++it) {
        QTimer::singleShot( 25, *it, SLOT(centerOnAllNodes()) );
    }
}

void
Gui::setLastEnteredTabWidget(TabWidget* tab)
{
    _imp->_lastEnteredTabWidget = tab;
}

TabWidget*
Gui::getLastEnteredTabWidget() const
{
    return _imp->_lastEnteredTabWidget;
}

void
Gui::onPrevTabTriggered()
{
    TabWidget* t = getLastEnteredTabWidget();

    if (t) {
        t->moveToPreviousTab();
        PanelWidget* pw = t->currentWidget();
        if (pw) {
            pw->takeClickFocus();
        }
    }
}

void
Gui::onNextTabTriggered()
{
    TabWidget* t = getLastEnteredTabWidget();

    if (t) {
        t->moveToNextTab();
        PanelWidget* pw = t->currentWidget();
        if (pw) {
            pw->takeClickFocus();
        }
    }
}

void
Gui::onCloseTabTriggered()
{
    TabWidget* t = getLastEnteredTabWidget();

    if (t) {
        t->closeCurrentWidget();
        PanelWidget* pw = t->currentWidget();
        if (pw) {
            pw->takeClickFocus();
        }
    }
}

void
Gui::appendToScriptEditor(const std::string & str)
{
    _imp->_scriptEditor->appendToScriptEditor( QString::fromUtf8( str.c_str() ) );
}

void
Gui::printAutoDeclaredVariable(const std::string & str)
{
    _imp->_scriptEditor->printAutoDeclaredVariable( QString::fromUtf8( str.c_str() ) );
}

void
Gui::exportGroupAsPythonScript(NodeCollection* collection)
{
    assert(collection);
    NodesList nodes = collection->getNodes();
    bool hasOutput = false;
    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ( (*it)->isActivated() && dynamic_cast<GroupOutput*>( (*it)->getEffectInstance().get() ) ) {
            hasOutput = true;
            break;
        }
    }

    if (!hasOutput) {
        Dialogs::errorDialog( tr("Export").toStdString(), tr("To export as group, at least one Output node must exist.").toStdString() );

        return;
    }
    ExportGroupTemplateDialog dialog(collection, this, this);
    ignore_result( dialog.exec() );
}

void
Gui::exportProjectAsGroup()
{
    exportGroupAsPythonScript( getApp()->getProject().get() );
}

static void
runTemplatePython(Gui* gui, const char* script)
{
    std::string err, output;
    if (!NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output)) {
        gui->getApp()->appendToScriptEditor(err);
    } else if (!output.empty()) {
        gui->getApp()->appendToScriptEditor(output);
    }
}

// Megascans / Quixel Bridge asset loader.
//
// Unlike the other templates this one needs the user to pick an asset, so the
// folder + mesh choice happen in C++ and the resulting paths are baked into the
// Python that builds the graph. Texture discovery is done from what is actually
// ON DISK (glob), not from the asset .json: the json advertises every map at
// every resolution Quixel offers, while a download only contains the formats and
// maps the user ticked in Bridge.
void
Gui::createTemplateMegascansAsset()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose a Megascans asset folder"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if ( dir.isEmpty() ) return;

    // Meshes we can actually read, found RECURSIVELY: a "3d asset" keeps its
    // LODs beside the textures, while a "3dplant" puts each variation in its own
    // Var1/…Var19/ subfolder. FBX is deliberately absent — ReadGeo handles .abc
    // and .obj only, and Bridge exports all three, so the fix is a re-download.
    QDir d(dir);
    QStringList meshRelPaths;
    {
        QStringList meshFilter;
        meshFilter << QString::fromUtf8("*.abc") << QString::fromUtf8("*.obj");
        QDirIterator it(dir, meshFilter, QDir::Files, QDirIterator::Subdirectories);
        while ( it.hasNext() ) {
            it.next();
            meshRelPaths << d.relativeFilePath( it.filePath() );
        }
    }
    if ( meshRelPaths.isEmpty() ) {
        QDirIterator fbxIt(dir, QStringList() << QString::fromUtf8("*.fbx"),
                           QDir::Files, QDirIterator::Subdirectories);
        Dialogs::errorDialog(tr("Megascans").toStdString(),
            (fbxIt.hasNext()
                ? tr("This asset only contains .fbx meshes, which Natron cannot read yet.\n\n"
                     "Re-download it from Bridge with the Alembic (.abc) or OBJ format "
                     "selected — Quixel publishes all three for every LOD.")
                : tr("No .abc or .obj mesh found in that folder (searched subfolders too).")).toStdString());
        return;
    }

    // The asset .json knows the triangle count, variation and LOD of each mesh —
    // far more useful to choose on than a filename. Two schemas exist: "3dplant"
    // uses models[] (with variation/lod/tris), "3d asset" uses meshes[] (no
    // counts). Anything unparseable just falls back to sorted paths.
    QMap<QString, QString> labelToRel;   // display label -> relative path
    QMap<QString, QPair<int, int> > relToOrder; // relative path -> (variation, lod)
    {
        const QStringList jsons = d.entryList(QStringList() << QString::fromUtf8("*.json"),
                                              QDir::Files);
        if ( !jsons.isEmpty() ) {
            QFile jf( d.absoluteFilePath( jsons.first() ) );
            if ( jf.open(QIODevice::ReadOnly) ) {
                const QJsonObject root = QJsonDocument::fromJson( jf.readAll() ).object();
                Q_FOREACH(const QJsonValue& mv, root.value(QString::fromUtf8("models")).toArray()) {
                    const QJsonObject mo = mv.toObject();
                    const QString uri = mo.value(QString::fromUtf8("uri")).toString();
                    if ( uri.isEmpty() ) continue;
                    const QString rel = QDir::fromNativeSeparators(uri);
                    const int var  = mo.value(QString::fromUtf8("variation")).toInt(-1);
                    const int lod  = mo.value(QString::fromUtf8("lod")).toInt(-1);
                    const int tris = mo.value(QString::fromUtf8("tris")).toInt(-1);
                    if (var >= 0 || lod >= 0) relToOrder.insert(rel, qMakePair(var, lod));
                    if (tris > 0) {
                        // Group digits so 340795 reads as 340,795 at a glance.
                        QString t = QString::number(tris);
                        for (int p = t.size() - 3; p > 0; p -= 3) t.insert(p, QChar::fromLatin1(','));
                        labelToRel.insert(QString::fromUtf8("%1  —  %2 tris").arg(rel).arg(t), rel);
                    }
                }
            }
        }
    }

    // Sort by (variation, LOD) when the json gave us those, so Var2 doesn't sort
    // after Var10; otherwise plain path order.
    std::sort(meshRelPaths.begin(), meshRelPaths.end(),
              [&relToOrder](const QString& a, const QString& b) {
                  const bool ha = relToOrder.contains(a), hb = relToOrder.contains(b);
                  if (ha && hb) return relToOrder.value(a) < relToOrder.value(b);
                  return a < b;
              });

    QStringList choices;
    QMap<QString, QString> choiceToRel;
    Q_FOREACH(const QString& rel, meshRelPaths) {
        QString label = rel;
        for (QMap<QString, QString>::const_iterator lit = labelToRel.constBegin();
             lit != labelToRel.constEnd(); ++lit) {
            if (lit.value() == rel) { label = lit.key(); break; }
        }
        choices << label;
        choiceToRel.insert(label, rel);
    }

    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, tr("Megascans"),
        tr("Mesh to load (%1 found):").arg(choices.size()),
        choices, 0, false, &ok);
    if (!ok || choice.isEmpty()) return;

    const QString meshPath = d.absoluteFilePath( choiceToRel.value(choice, choice) );

    // Python does discovery + wiring. Paths are passed as raw strings so Windows
    // backslashes survive.
    QString py = QString::fromUtf8(
"app = app1\n"
"import os, glob, re\n"
"folder = r\"%1\"\n"
"meshPath = r\"%2\"\n"
"\n"
"# LOD index of the chosen mesh, so the LOD-matched maps get picked. Megascans\n"
"# ships per-LOD normals (Normal_LOD0..LOD5) and mixing them up is a silent,\n"
"# hard-to-spot shading error.\n"
"m = re.search(r'_LOD(\\d+)', os.path.basename(meshPath))\n"
"lod = m.group(1) if m else None\n"
"\n"
"# Where the maps live. A '3d asset' keeps them beside the meshes; a '3dplant'\n"
"# splits them into Textures/Atlas (the leaf/bark atlas) and Textures/Billboard\n"
"# (for the flat billboard LOD). Search the most specific first, then the root.\n"
"atlasDir = os.path.join(folder, 'Textures', 'Atlas')\n"
"bbDir    = os.path.join(folder, 'Textures', 'Billboard')\n"
"texDirs  = []\n"
"if 'billboard' in os.path.basename(meshPath).lower() and os.path.isdir(bbDir):\n"
"    texDirs.append(bbDir)\n"
"if os.path.isdir(atlasDir): texDirs.append(atlasDir)\n"
"if os.path.isdir(bbDir) and bbDir not in texDirs: texDirs.append(bbDir)\n"
"texDirs.append(folder)\n"
"\n"
"def first(pats):\n"
"    for d in texDirs:\n"
"        for p in pats:\n"
"            hits = sorted(glob.glob(os.path.join(d, p)))\n"
"            if hits: return hits[0]\n"
"    return ''\n"
"\n"
"def pick(kind, perLod=True):\n"
"    pats = []\n"
"    if lod and perLod:\n"
"        pats += ['*_%s_LOD' % kind + lod + '.exr', '*_%s_LOD' % kind + lod + '.jpg']\n"
"    # EXR first: height/roughness want the bit depth, and Quixel ships both.\n"
"    pats += ['*_%s.exr' % kind, '*_%s.jpg' % kind, '*_%s.png' % kind]\n"
"    return first(pats)\n"
"\n"
"albedo = pick('Albedo')\n"
"rough  = pick('Roughness', False)\n"
"normal = pick('Normal')\n"
"spec   = pick('Specular', False)\n"
"disp   = pick('Displacement', False)\n"
"opac   = pick('Opacity', False)\n"
"trans  = pick('Translucency', False)\n"
"\n"
"geo = app.createNode('fr.inria.built-in.ReadGeo')\n"
"geo.getParam('filename').setValue(meshPath)\n"
"mat = app.createNode('fr.inria.built-in.Material3D')\n"
"geo.connectInput(0, mat)          # ReadGeo input 0 = Material\n"
"\n"
"# Albedo through a Read so it can be graded in the comp. Material3D bakes a\n"
"# connected input to a LINEAR .hdr before Cycles sees it, so Diffuse Colorspace\n"
"# must say linear here — pointing it at sRGB would decode the texture twice.\n"
"read = None\n"
"if albedo:\n"
"    read = app.createReader(albedo)\n"
"    if read is not None:\n"
"        mat.connectInput(0, read)  # Material3D input 0 = Diffuse\n"
"        cs = mat.getParam('diffuseColorspace')\n"
"        if cs is not None:\n"
"            for i in range(cs.getNumOptions()):\n"
"                if 'linear' in cs.getOption(i).lower():\n"
"                    cs.setValue(i); break\n"
"\n"
"# The rest as file paths on the material: they are data maps, nothing to grade.\n"
"for knob, path in (('roughnessMapFile', rough), ('normalMapFile', normal),\n"
"                   ('specularMapFile', spec), ('displacementMapFile', disp),\n"
"                   ('opacityMapFile', opac), ('translucencyMapFile', trans)):\n"
"    if path:\n"
"        p = mat.getParam(knob)\n"
"        if p is not None: p.setValue(path)\n"
"\n"
"# Megascans meshes are authored in CENTIMETRES: this tree measures ~205 units\n"
"# for a 2.06 m trunk. Bring it to metres so it sits sanely next to everything\n"
"# else in the scene.\n"
"for k in ('scaleX', 'scaleY', 'scaleZ'):\n"
"    p = geo.getParam(k)\n"
"    if p is not None: p.setValue(0.01)\n"
"\n"
"if read is not None: read.setPosition(-150, -250)\n"
"mat.setPosition(-150, -120)\n"
"geo.setPosition(0, 0)\n"
"\n"
"wired = [n for n, p in (('albedo', albedo), ('roughness', rough), ('normal', normal),\n"
"                        ('specular', spec), ('displacement', disp),\n"
"                        ('opacity', opac), ('translucency', trans)) if p]\n"
"print('Megascans: loaded ' + os.path.basename(meshPath) + ' (scaled cm->m)')\n"
"print('Megascans: wired ' + (', '.join(wired) if wired else 'no maps found'))\n"
"# Say what was ignored rather than leaving the user to wonder where it went.\n"
"skipped = []\n"
"for d in texDirs:\n"
"    for f in sorted(glob.glob(os.path.join(d, '*'))):\n"
"        if re.search(r'_(Cavity|Gloss|Bump|NormalBump|AO|Fuzz|Metalness|Curvature)\\.', f):\n"
"            skipped.append(os.path.basename(f))\n"
"skipped = sorted(set(skipped))\n"
"if skipped:\n"
"    print('Megascans: not wired (no matching material input): ' + ', '.join(skipped))\n"
"print('Megascans: displacement is applied as BUMP - shading detail only, silhouette unchanged')\n"
        ).arg(dir).arg(meshPath);

    runTemplatePython( this, py.toStdString().c_str() );
}

void
Gui::createTemplate3DBasic()
{
    runTemplatePython(this, R"PY(
app = app1
sphere = app.createNode("fr.inria.built-in.Sphere3D")
light = app.createNode("fr.inria.built-in.Light3D")
scene = app.createNode("fr.inria.built-in.Scene3D")
camera = app.createNode("fr.inria.built-in.Camera3D")
cycles = app.createNode("fr.inria.built-in.CyclesRender")
viewer = app.createNode("fr.inria.built-in.Viewer")
scene.connectInput(0, sphere)
scene.connectInput(1, light)
cycles.connectInput(1, scene)
cycles.connectInput(2, camera)
viewer.connectInput(0, cycles)
sphere.setPosition(-100, -200)
light.setPosition(100, -200)
scene.setPosition(0, -50)
camera.setPosition(-200, 100)
cycles.setPosition(0, 100)
viewer.setPosition(0, 250)
camera.getParam("translateZ").setValue(5.0)
)PY");
}

void
Gui::createTemplateVDBFire()
{
    runTemplatePython(this, R"PY(
app = app1
vdb = app.createNode("fr.inria.built-in.ReadVDB")
light = app.createNode("fr.inria.built-in.Light3D")
scene = app.createNode("fr.inria.built-in.Scene3D")
camera = app.createNode("fr.inria.built-in.Camera3D")
cycles = app.createNode("fr.inria.built-in.CyclesRender")
viewer = app.createNode("fr.inria.built-in.Viewer")
scene.connectInput(0, vdb)
scene.connectInput(1, light)
cycles.connectInput(1, scene)
cycles.connectInput(2, camera)
viewer.connectInput(0, cycles)
vdb.setPosition(-100, -200)
light.setPosition(100, -200)
scene.setPosition(0, -50)
camera.setPosition(-200, 100)
cycles.setPosition(0, 100)
viewer.setPosition(0, 250)
light.getParam("lightType").setValue(4)
light.getParam("intensity").setValue(1.0)
camera.getParam("translateZ").setValue(5.0)
)PY");
}

void
Gui::createTemplateParticles()
{
    runTemplatePython(this, R"PY(
app = app1
emitter = app.createNode("fr.inria.built-in.ParticleEmitter")
gravity = app.createNode("fr.inria.built-in.ParticleGravity")
solver = app.createNode("fr.inria.built-in.ParticleSolver")
scene = app.createNode("fr.inria.built-in.Scene3D")
camera = app.createNode("fr.inria.built-in.Camera3D")
scanline = app.createNode("fr.inria.built-in.ScanlineRender")
viewer = app.createNode("fr.inria.built-in.Viewer")
gravity.connectInput(0, emitter)
solver.connectInput(0, gravity)
scene.connectInput(0, solver)
scanline.connectInput(1, scene)
scanline.connectInput(2, camera)
viewer.connectInput(0, scanline)
emitter.setPosition(0, -350)
gravity.setPosition(0, -200)
solver.setPosition(0, -50)
scene.setPosition(0, 100)
camera.setPosition(-200, 250)
scanline.setPosition(0, 250)
viewer.setPosition(0, 400)
emitter.getParam("translateY").setValue(2.0)
camera.getParam("translateZ").setValue(8.0)
)PY");
}

void
Gui::createTemplateParticleSparks()
{
    // Collision sparks — the WIKI "Sparks on collision" pipeline:
    // Emitter → Gravity → Solver (Cube3D floor) → Spawn (On Collision) →
    // Merge (solver + sparks) → Scene → ScanlineRender → Viewer
    runTemplatePython(this, R"PY(
app = app1
emitter = app.createNode("fr.inria.built-in.ParticleEmitter")
gravity = app.createNode("fr.inria.built-in.ParticleGravity")
solver = app.createNode("fr.inria.built-in.ParticleSolver")
floor = app.createNode("fr.inria.built-in.Cube3D")
spawn = app.createNode("fr.inria.built-in.ParticleSpawn")
merge = app.createNode("fr.inria.built-in.ParticleMerge")
scene = app.createNode("fr.inria.built-in.Scene3D")
camera = app.createNode("fr.inria.built-in.Camera3D")
scanline = app.createNode("fr.inria.built-in.ScanlineRender")
viewer = app.createNode("fr.inria.built-in.Viewer")

gravity.connectInput(0, emitter)
solver.connectInput(0, gravity)
solver.connectInput(1, floor)
spawn.connectInput(0, solver)
merge.connectInput(0, solver)
merge.connectInput(1, spawn)
scene.connectInput(0, merge)
scanline.connectInput(1, scene)
scanline.connectInput(2, camera)
viewer.connectInput(0, scanline)

emitter.setPosition(0, -500)
gravity.setPosition(0, -350)
floor.setPosition(-200, -200)
solver.setPosition(0, -200)
spawn.setPosition(200, -50)
merge.setPosition(0, 100)
scene.setPosition(0, 250)
camera.setPosition(-200, 400)
scanline.setPosition(0, 400)
viewer.setPosition(0, 550)

# Emitter: a stream of hot particles falling onto the floor
emitter.getParam("translateY").setValue(4.0)
emitter.getParam("velocity").setValue(1.0)
emitter.getParam("spread").setValue(20.0)
emitter.getParam("startSize").setValue(0.06)
emitter.getParam("lifetime").setValue(80.0)
emitter.getParam("startColorR").setValue(1.0)
emitter.getParam("startColorG").setValue(0.75)
emitter.getParam("startColorB").setValue(0.35)

# Floor: a flat cube to collide with
floor.getParam("scaleX").setValue(8.0)
floor.getParam("scaleY").setValue(0.1)
floor.getParam("scaleZ").setValue(8.0)
floor.getParam("translateY").setValue(-1.0)

# Solver: lively bounces
solver.getParam("elasticity").setValue(0.45)
solver.getParam("friction").setValue(0.3)

# Spawn: short-lived hot sparks on every impact
spawn.getParam("trigger").set(3)
spawn.getParam("rate").setValue(6)
spawn.getParam("inheritVelocity").setValue(0.3)
spawn.getParam("extraSpeed").setValue(1.2)
spawn.getParam("childLifetime").setValue(12.0)
spawn.getParam("childSize").setValue(0.025)
spawn.getParam("childColorR").setValue(1.0)
spawn.getParam("childColorG").setValue(0.55)
spawn.getParam("childColorB").setValue(0.12)

camera.getParam("translateZ").setValue(10.0)
camera.getParam("translateY").setValue(1.0)
)PY");
}

void
Gui::createTemplateParticleJetExhaust()
{
    // Hot fast core + slow lingering smoke, each with its OWN force/solver
    // chain (forces upstream of a ParticleMerge don't reach a shared solver),
    // merged for display. Additive discs + stretch motion blur.
    runTemplatePython(this, R"PY(
app = app1
core = app.createNode("fr.inria.built-in.ParticleEmitter")
coreDrag = app.createNode("fr.inria.built-in.ParticleDrag")
coreTurb = app.createNode("fr.inria.built-in.ParticleTurbulence")
coreSolver = app.createNode("fr.inria.built-in.ParticleSolver")
smoke = app.createNode("fr.inria.built-in.ParticleEmitter")
smokeDrag = app.createNode("fr.inria.built-in.ParticleDrag")
smokeTurb = app.createNode("fr.inria.built-in.ParticleTurbulence")
smokeSolver = app.createNode("fr.inria.built-in.ParticleSolver")
merge = app.createNode("fr.inria.built-in.ParticleMerge")
scene = app.createNode("fr.inria.built-in.Scene3D")
camera = app.createNode("fr.inria.built-in.Camera3D")
scanline = app.createNode("fr.inria.built-in.ScanlineRender")
viewer = app.createNode("fr.inria.built-in.Viewer")

coreDrag.connectInput(0, core)
coreTurb.connectInput(0, coreDrag)
coreSolver.connectInput(0, coreTurb)
smokeDrag.connectInput(0, smoke)
smokeTurb.connectInput(0, smokeDrag)
smokeSolver.connectInput(0, smokeTurb)
merge.connectInput(0, coreSolver)
merge.connectInput(1, smokeSolver)
scene.connectInput(0, merge)
scanline.connectInput(1, scene)
scanline.connectInput(2, camera)
viewer.connectInput(0, scanline)

core.setPosition(-150, -500); coreDrag.setPosition(-150, -350)
coreTurb.setPosition(-150, -200); coreSolver.setPosition(-150, -50)
smoke.setPosition(150, -500); smokeDrag.setPosition(150, -350)
smokeTurb.setPosition(150, -200); smokeSolver.setPosition(150, -50)
merge.setPosition(0, 100); scene.setPosition(0, 250)
camera.setPosition(-200, 400); scanline.setPosition(0, 400)
viewer.setPosition(0, 550)

# Core: hot, fast, short-lived
core.getParam("translateX").setValue(-3.0)
core.getParam("emitDirX").setValue(1.0)
core.getParam("emitDirY").setValue(0.0)
core.getParam("rate").setValue(400)
core.getParam("lifetime").setValue(25.0)
core.getParam("velocity").setValue(3.0)
core.getParam("velocityVariance").setValue(0.6)
core.getParam("spread").setValue(6.0)
core.getParam("startSize").setValue(0.08)
core.getParam("endSize").setValue(0.02)
core.getParam("startColorR").setValue(1.0)
core.getParam("startColorG").setValue(0.92)
core.getParam("startColorB").setValue(0.6)
core.getParam("endColorR").setValue(1.0)
core.getParam("endColorG").setValue(0.35)
core.getParam("endColorB").setValue(0.05)
core.getParam("fadeIn").setValue(0.02)
core.getParam("fadeOut").setValue(0.4)
coreDrag.getParam("drag").setValue(0.03)
coreTurb.getParam("strength").setValue(0.6)
coreTurb.getParam("scale").setValue(0.8)

# Smoke: slow, wide, long-lived, grows over life
smoke.getParam("translateX").setValue(-3.0)
smoke.getParam("emitDirX").setValue(1.0)
smoke.getParam("emitDirY").setValue(0.0)
smoke.getParam("seed").setValue(7)
smoke.getParam("rate").setValue(60)
smoke.getParam("lifetime").setValue(70.0)
smoke.getParam("velocity").setValue(1.2)
smoke.getParam("spread").setValue(12.0)
smoke.getParam("startSize").setValue(0.15)
smoke.getParam("endSize").setValue(0.6)
smoke.getParam("startColorR").setValue(0.35)
smoke.getParam("startColorG").setValue(0.35)
smoke.getParam("startColorB").setValue(0.35)
smoke.getParam("endColorR").setValue(0.22)
smoke.getParam("endColorG").setValue(0.22)
smoke.getParam("endColorB").setValue(0.22)
smoke.getParam("fadeIn").setValue(0.25)
smoke.getParam("fadeOut").setValue(0.5)
smokeDrag.getParam("drag").setValue(0.06)
smokeTurb.getParam("strength").setValue(0.4)

scanline.getParam("particleMode").set(1)      # Disc
scanline.getParam("particleBlend").set(0)     # Additive
scanline.getParam("particleMotionBlur").setValue(0.5)
camera.getParam("translateZ").setValue(9.0)
)PY");
}

void
Gui::createTemplateParticleHeatDistort()
{
    // Red/green turbulent particles rendered as a UV displacement pass:
    // ScanlineRender (additive discs) -> Blur -> IDistort.UV, distorting the
    // plate input (Checkerboard placeholder — replace with footage).
    runTemplatePython(this, R"PY(
app = app1
emR = app.createNode("fr.inria.built-in.ParticleEmitter")
turbR = app.createNode("fr.inria.built-in.ParticleTurbulence")
solverR = app.createNode("fr.inria.built-in.ParticleSolver")
emG = app.createNode("fr.inria.built-in.ParticleEmitter")
turbG = app.createNode("fr.inria.built-in.ParticleTurbulence")
solverG = app.createNode("fr.inria.built-in.ParticleSolver")
merge = app.createNode("fr.inria.built-in.ParticleMerge")
scene = app.createNode("fr.inria.built-in.Scene3D")
camera = app.createNode("fr.inria.built-in.Camera3D")
scanline = app.createNode("fr.inria.built-in.ScanlineRender")
blur = app.createNode("net.sf.cimg.CImgBlur")
checker = app.createNode("net.sf.openfx.CheckerBoardPlugin")
idistort = app.createNode("net.sf.openfx.IDistort")
viewer = app.createNode("fr.inria.built-in.Viewer")

turbR.connectInput(0, emR)
solverR.connectInput(0, turbR)
turbG.connectInput(0, emG)
solverG.connectInput(0, turbG)
merge.connectInput(0, solverR)
merge.connectInput(1, solverG)
scene.connectInput(0, merge)
scanline.connectInput(1, scene)
scanline.connectInput(2, camera)
blur.connectInput(0, scanline)

# IDistort input names differ by index — resolve by label
for i in range(idistort.getMaxInputCount()):
    lbl = idistort.getInputLabel(i)
    if lbl == "UV":
        idistort.connectInput(i, blur)
    elif lbl == "Source":
        idistort.connectInput(i, checker)
viewer.connectInput(0, idistort)

emR.setPosition(-150, -500); turbR.setPosition(-150, -350); solverR.setPosition(-150, -200)
emG.setPosition(150, -500); turbG.setPosition(150, -350); solverG.setPosition(150, -200)
merge.setPosition(0, -50); scene.setPosition(0, 100)
camera.setPosition(-200, 250); scanline.setPosition(0, 250)
blur.setPosition(0, 400); checker.setPosition(300, 400)
idistort.setPosition(0, 550); viewer.setPosition(0, 700)

def setupHeatEmitter(em, r, g, b, seed):
    em.getParam("translateY").setValue(-1.5)
    em.getParam("rate").setValue(150)
    em.getParam("lifetime").setValue(40.0)
    em.getParam("velocity").setValue(0.8)
    em.getParam("spread").setValue(25.0)
    em.getParam("startSize").setValue(0.25)
    em.getParam("endSize").setValue(0.45)
    em.getParam("startColorR").setValue(r)
    em.getParam("startColorG").setValue(g)
    em.getParam("startColorB").setValue(b)
    em.getParam("endColorR").setValue(r)
    em.getParam("endColorG").setValue(g)
    em.getParam("endColorB").setValue(b)
    em.getParam("fadeIn").setValue(0.3)
    em.getParam("fadeOut").setValue(0.4)
    em.getParam("seed").setValue(seed)

setupHeatEmitter(emR, 1.0, 0.0, 0.0, 0)
setupHeatEmitter(emG, 0.0, 1.0, 0.0, 11)
turbR.getParam("strength").setValue(1.2)
turbR.getParam("scale").setValue(0.6)
turbG.getParam("strength").setValue(1.2)
turbG.getParam("scale").setValue(0.6)

scanline.getParam("particleMode").set(1)   # Disc
scanline.getParam("particleBlend").set(0)  # Additive
blur.getParam("size").setValue(8, 0)
blur.getParam("size").setValue(8, 1)
idistort.getParam("uvScale").setValue(30, 0)
idistort.getParam("uvScale").setValue(30, 1)
camera.getParam("translateZ").setValue(8.0)
)PY");
}

void
Gui::createTemplateHDRIBasic()
{
    // 1:1 reproduction of the reference HDRI face-edit rig (hdri.ntp):
    // source -> Reformat (custom LatLong4K 4096x2048 format) -> Dot ->
    // 6x [Grade (face-ID tint) -> SphericalTransform (LatLong -> one cube
    // face) -> RotoPaint (copy blend)] -> SphericalTransform Faces-input
    // reassembly -> Viewer. Face branches left->right: -X, -Z, +X, +Z, +Y, -Y.
    runTemplatePython(this, R"PY(
app = app1

# Custom lat-long format, created project-wide (shows up in every format
# dropdown; Reformat below selects it).
app.addFormat("LatLong4K 4096 x 2048 1")

checker = app.createNode("net.sf.openfx.CheckerBoardPlugin")
reformat = app.createNode("net.sf.openfx.Reformat")
dot = app.createNode("fr.inria.built-in.Dot")
viewer = app.createNode("fr.inria.built-in.Viewer")

# Reassembly: Faces-input SphericalTransform (cube faces on inputs 1-6).
# NOTE: its projection knobs are set at the END, after all connections —
# the knobChanged metadata refresh needs the inputs wired to take hold.
assemble = app.createNode("fr.inria.built-in.SphericalTransform")
assemble.setLabel("AssembleLatLong")

# Per-face branches. (face label, cubemapFaceOutput index, grade multiply RGB,
# x position, assemble input slot [1=-Z 2=+Z 3=-X 4=+X 5=-Y 6=+Y])
faces = [
    ("negX", 1, (0.0,    0.0128, 1.0),    230, 3),
    ("negZ", 5, (0.0056, 1.0,    0.0),    482, 1),
    ("posX", 0, (0.0,    0.4926, 0.9911), 724, 4),
    ("posZ", 4, (0.5225, 0.0,    1.0),    927, 2),
    ("posY", 2, (0.9259, 1.0,    0.0),   1146, 6),
    ("negY", 3, (1.0,    0.0,    0.0),   1393, 5),
]

for (name, faceIdx, mult, xpos, slot) in faces:
    grade = app.createNode("net.sf.openfx.GradePlugin")
    st = app.createNode("fr.inria.built-in.SphericalTransform")
    roto = app.createNode("fr.inria.built-in.RotoPaint")
    grade.setLabel("Grade_" + name)
    st.setLabel("ToFace_" + name)
    roto.setLabel("Paint_" + name)

    grade.connectInput(0, dot)
    st.connectInput(0, grade)
    roto.connectInput(0, st)
    assemble.connectInput(slot, roto)

    grade.getParam("multiply").set(mult[0], mult[1], mult[2], 1.0)
    grade.getParam("premult").set(True)

    st.getParam("outputProjection").set(1)     # Cubemap
    st.getParam("cubemapFormatOutput").set(2)  # Faces (single face out)
    st.getParam("cubemapFaceOutput").set(faceIdx)

    roto.getParam("blendingModeButton").set(6) # copy

    grade.setPosition(xpos + 16, 395)
    st.setPosition(xpos, 468)
    roto.setPosition(xpos, 736)

reformat.connectInput(0, checker)
dot.connectInput(0, reformat)
viewer.connectInput(0, assemble)

checker.setPosition(841, 46)
reformat.setPosition(841, 116)
dot.setPosition(886, 201)
assemble.setPosition(806, 1055)
viewer.setPosition(806, 1151)

# Source placeholder at HD, output conformed to the custom lat-long format.
# Swap the CheckerBoard for your HDRI Read.
checker.getParam("NatronParamFormatChoice").set("PC_Video")
reformat.getParam("reformatType").set(0)  # to format
reformat.getParam("NatronParamFormatChoice").set("LatLong4K")

# Reassembly mode LAST (inputs are all wired now, so the metadata refresh
# in knobChanged sees the faces).
assemble.getParam("inputProjection").set(1)      # Cubemap
assemble.getParam("cubemapFormatInput").set(2)   # Faces (separate inputs)
)PY");
}

void
Gui::onUserCommandTriggered()
{
    QAction* action = qobject_cast<QAction*>( sender() );

    if (!action) {
        return;
    }
    ActionWithShortcut* aws = dynamic_cast<ActionWithShortcut*>(action);
    if (!aws) {
        return;
    }
    std::map<ActionWithShortcut*, std::string>::iterator found = _imp->pythonCommands.find(aws);
    if ( found != _imp->pythonCommands.end() ) {
        std::string err;
        std::string output;
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(found->second, &err, &output) ) {
            getApp()->appendToScriptEditor(err);
        } else {
            getApp()->appendToScriptEditor(output);
        }
    }
}

void
Gui::addMenuEntry(const QString & menuGrouping,
                  const std::string & pythonFunction,
                  Qt::Key key,
                  const Qt::KeyboardModifiers & modifiers)
{
    QStringList grouping = menuGrouping.split( QLatin1Char('/') );

    if ( grouping.isEmpty() ) {
        getApp()->appendToScriptEditor( tr("Failed to add menu entry for ").toStdString() +
                                        menuGrouping.toStdString() +
                                        tr(": incorrect menu grouping").toStdString() );

        return;
    }

    std::string appID = getApp()->getAppIDString();
    std::string script = "app = " + appID + "\n" + pythonFunction + "()\n";
    QAction* action = _imp->findActionRecursive(0, _imp->menubar, grouping);
    ActionWithShortcut* aws = dynamic_cast<ActionWithShortcut*>(action);
    if (aws) {
        aws->setShortcut( makeKeySequence(modifiers, key) );
        std::map<ActionWithShortcut*, std::string>::iterator found = _imp->pythonCommands.find(aws);
        if ( found != _imp->pythonCommands.end() ) {
            found->second = pythonFunction;
        } else {
            _imp->pythonCommands.insert( std::make_pair(aws, script) );
        }
    }
}

void
Gui::setDopeSheetTreeWidth(int width)
{
    _imp->_dopeSheetEditor->setTreeWidgetWidth(width);
}

void
Gui::setCurveEditorTreeWidth(int width)
{
    _imp->_curveEditor->setTreeWidgetWidth(width);
}

void
Gui::setTripleSyncEnabled(bool enabled)
{
    if (_imp->_isTripleSyncEnabled != enabled) {
        _imp->_isTripleSyncEnabled = enabled;
    }
}

bool
Gui::isTripleSyncEnabled() const
{
    return _imp->_isTripleSyncEnabled;
}

void
Gui::centerOpenedViewersOn(SequenceTime left,
                           SequenceTime right)
{
    const std::list<ViewerTab *> &viewers = getViewersList();

    for (std::list<ViewerTab *>::const_iterator it = viewers.begin(); it != viewers.end(); ++it) {
        ViewerTab *v = (*it);

        v->centerOn_tripleSync(left, right);
    }
}

#ifdef __NATRON_WIN32__
void
Gui::ddeOpenFile(const QString& filePath)
{
    getApp()->handleFileOpenEvent( filePath.toStdString() );
}

#endif

bool
Gui::isFocusStealingPossible()
{
    assert( qApp && qApp->thread() == QThread::currentThread() );
    QWidget* currentFocus = qApp->focusWidget();
    bool focusStealingNotPossible = ( dynamic_cast<QLineEdit*>(currentFocus) ||
                                      dynamic_cast<QTextEdit*>(currentFocus) );

    return !focusStealingNotPossible;
}

void
Gui::setCurrentPanelFocus(PanelWidget* widget)
{
    assert( QThread::currentThread() == qApp->thread() );
    _imp->currentPanelFocus = widget;

}

PanelWidget*
Gui::getCurrentPanelFocus() const
{
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->currentPanelFocus;
}


static PanelWidget*
isPaneChild(QWidget* w,
            int recursionLevel)
{
    if (!w) {
        return 0;
    }
    PanelWidget* pw = dynamic_cast<PanelWidget*>(w);
    if ( pw && (recursionLevel > 0) ) {
        /*
           Do not return it if recursion is 0, otherwise the focus stealing of the mouse over will actually take click focus
         */
        return pw;
    }

    return isPaneChild(w->parentWidget(), recursionLevel + 1);
}

void
Gui::onFocusChanged(QWidget* /*old*/,
                    QWidget* newFocus)
{
    PanelWidget* pw = isPaneChild(newFocus, 0);

    if (pw) {
        pw->takeClickFocus();
    }
}

void
Gui::fileSequencesFromUrls(const QList<QUrl>& urls,
                           std::vector<SequenceParsing::SequenceFromFilesPtr>* sequences)
{

    QStringList filesList;

    for (int i = 0; i < urls.size(); ++i) {
        const QUrl rl = urls.at(i);
        QString path = rl.toLocalFile();

#ifdef __NATRON_WIN32__
        if (appPTR->getCurrentSettings()->isDriveLetterToUNCPathConversionEnabled()) {
            path = FileSystemModel::mapPathWithDriveLetterToPathWithNetworkShareName(path);
        }
#endif
        QDir dir(path);
        
        //if the path dropped is not a directory append it
        if ( !dir.exists() ) {
            filesList << path;
        } else {
            //otherwise append everything inside the dir recursively
            SequenceFileDialog::appendFilesFromDirRecursively(&dir, &filesList);
        }
    }

    QStringList supportedExtensions;
    supportedExtensions.push_back( QString::fromLatin1(NATRON_PROJECT_FILE_EXT) );
    supportedExtensions.push_back( QString::fromLatin1("py") );

    std::vector<std::string> readersFormat;
    appPTR->getSupportedReaderFileFormats(&readersFormat);
    for (std::vector<std::string>::const_iterator it = readersFormat.begin(); it != readersFormat.end(); ++it) {
        supportedExtensions.push_back( QString::fromUtf8( it->c_str() ) );
    }
    *sequences = SequenceFileDialog::fileSequencesFromFilesList(filesList, supportedExtensions);

}

void
Gui::dragEnterEvent(QDragEnterEvent* e)
{

    if ( !e->mimeData()->hasUrls() ) {
        return;
    }

    e->acceptProposedAction();

}

void
Gui::dragMoveEvent(QDragMoveEvent* e)
{

    if ( !e->mimeData()->hasUrls() ) {
        return;
    }


    e->acceptProposedAction();

}

void
Gui::dragLeaveEvent(QDragLeaveEvent* e)
{
    e->accept();
}

static NodeGraph*
isNodeGraphChild(QWidget* w)
{
    NodeGraph* n = dynamic_cast<NodeGraph*>(w);
    if (!w) {
        return 0;
    }
    if (n) {
        return n;
    } else {
        QWidget* parent = w->parentWidget();
        if (parent) {
            return isNodeGraphChild(parent);
        } else {
            return 0;
        }
    }
}

void
Gui::handleOpenFilesFromUrls(const QList<QUrl>& urls,
                             const QPoint& globalPos)
{
    std::vector<SequenceParsing::SequenceFromFilesPtr> sequences;


    fileSequencesFromUrls(urls, &sequences);
    QWidget* widgetUnderMouse = QApplication::widgetAt(globalPos);
    NodeGraph* graph = isNodeGraphChild(widgetUnderMouse);

    if (!graph) {
        // No grpah under mouse, use top level one
        graph = _imp->_nodeGraphArea;
    }
    assert(graph);
    if (!graph) {
        return;
    }

    QPointF graphScenePos = graph->mapToScene( graph->mapFromGlobal(globalPos) );
    std::locale local;
    for (U32 i = 0; i < sequences.size(); ++i) {
        SequenceParsing::SequenceFromFilesPtr & sequence = sequences[i];
        if (sequence->count() < 1) {
            continue;
        }

        ///find a decoder for this file type
        //std::string ext = sequence->fileExtension();
        std::string extLower = sequence->fileExtension();
        std::transform(extLower.begin(), extLower.end(), extLower.begin(), [](char c) { return std::tolower(c, std::locale()); });
        if (extLower == NATRON_PROJECT_FILE_EXT) {
            const std::map<int, SequenceParsing::FileNameContent>& content = sequence->getFrameIndexes();
            assert( !content.empty() );
            AppInstancePtr appInstance = openProject( content.begin()->second.absoluteFileName() );
            Q_UNUSED(appInstance);
        } else if (extLower == "py") {
            const std::map<int, SequenceParsing::FileNameContent>& content = sequence->getFrameIndexes();
            assert( !content.empty() );
            _imp->_scriptEditor->sourceScript( QString::fromUtf8( content.begin()->second.absoluteFileName().c_str() ) );
            ensureScriptEditorVisible();
        } else {
            std::string readerPluginID = appPTR->getReaderPluginIDForFileType(extLower);
            if ( readerPluginID.empty() ) {
                Dialogs::errorDialog("Reader", "No plugin capable of decoding " + extLower + " was found.");
            } else {


                std::string pattern = sequence->generateValidSequencePattern();
                CreateNodeArgs args(readerPluginID, graph->getGroup() );
                args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, graphScenePos.x(), 0);
                args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, graphScenePos.y(), 1);
                args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, pattern);


                NodePtr n = getApp()->createNode(args);

                //And offset scenePos by the Width of the previous node created if several nodes are created
                double w, h;
                n->getSize(&w, &h);
                graphScenePos.rx() += (w + 10);

            }
        }
    }
} // Gui::handleOpenFilesFromUrls

void
Gui::dropEvent(QDropEvent* e)
{
    if ( !e->mimeData()->hasUrls() ) {
        return;
    }

    e->accept();

    QList<QUrl> urls = e->mimeData()->urls();

    handleOpenFilesFromUrls( urls, mapToGlobal( e->pos() ) );
} // dropEvent

NATRON_NAMESPACE_EXIT
