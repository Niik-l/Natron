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

#ifndef NATRON_GUI_SHUFFLEWIDGET_H
#define NATRON_GUI_SHUFFLEWIDGET_H

#include <QWidget>
#include <QPoint>
#include <vector>
#include <string>

#include "Global/Macros.h"

class QComboBox;

NATRON_NAMESPACE_ENTER

/**
 * @brief Custom widget showing two-row channel routing
 * with draggable noodles.
 *
 * Row 1 (top): Input B channel routing
 * Row 2 (bottom): Input A channel routing
 *
 * Each row has its own Input Layer / Output Layer combo and sockets.
 * Cross-row connections are supported: routing values 0-99 address row 1
 * input channels, 100-199 address row 2 input channels.
 */
class ShuffleWidget : public QWidget
{
    Q_OBJECT

public:
    ShuffleWidget(QWidget* parent = NULL);

    // --- Row 1 (B input) ---
    void setInputChannels(const std::vector<std::string>& channels);
    void setOutputChannels(const std::vector<std::string>& channels);
    std::vector<int> getRouting() const { return _routing; }
    void setRouting(const std::vector<int>& routing);

    void setInputLayerChoices(const std::vector<std::string>& choices, int current);
    void setOutputLayerChoices(const std::vector<std::string>& choices, int current);
    int getInputLayerIndex() const;
    int getOutputLayerIndex() const;

    // --- Row 2 (A input) ---
    void setInputChannels2(const std::vector<std::string>& channels);
    void setOutputChannels2(const std::vector<std::string>& channels);
    std::vector<int> getRouting2() const { return _routing2; }
    void setRouting2(const std::vector<int>& routing);

    void setInputLayerChoices2(const std::vector<std::string>& choices, int current);
    void setOutputLayerChoices2(const std::vector<std::string>& choices, int current);
    int getInputLayerIndex2() const;
    int getOutputLayerIndex2() const;

Q_SIGNALS:
    // Row 1 signals
    void routingChanged();
    void inputLayerChanged(int index);
    void outputLayerChanged(int index);

    // Row 2 signals
    void routing2Changed();
    void inputLayer2Changed(int index);
    void outputLayer2Changed(int index);

    // New layer request (when user picks "new" from an output combo).
    // rowIndex = 0 for Row 1's "new", 1 for Row 2's "new".
    void newLayerRequested(int rowIndex);

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE;
    virtual QSize sizeHint() const OVERRIDE;

private:
    // Socket positions for row 1
    QPointF inputSocketPos(int idx) const;
    QPointF outputSocketPos(int idx) const;

    // Socket positions for row 2
    QPointF inputSocketPos2(int idx) const;
    QPointF outputSocketPos2(int idx) const;

    int hitTestInputSocket(const QPoint& pos) const;
    int hitTestOutputSocket(const QPoint& pos) const;
    int hitTestInputSocket2(const QPoint& pos) const;
    int hitTestOutputSocket2(const QPoint& pos) const;

    // Hit test for constant assignment buttons (black/white circles next to output sockets)
    // Returns: 0 = no hit, -2 = black button, -3 = white button
    int hitTestConstantButton(const QPoint& pos, int& outIdx) const;
    int hitTestConstantButton2(const QPoint& pos, int& outIdx) const;

    QColor channelColor(int idx) const;

    // Row 1 top offset (below combos)
    float row1Top() const;
    // Row 2 top offset
    float row2Top() const;
    // Height of one row section (combo + channels)
    float rowSectionHeight(int numChannels) const;

private Q_SLOTS:
    void onOutputLayerComboTextChanged(int index);
    void onOutputLayer2ComboTextChanged(int index);

private:

    // --- Row 1 data ---
    std::vector<std::string> _inputChannels;
    std::vector<std::string> _outputChannels;
    std::vector<int> _routing; // routing[outIdx] = inIdx (0-99 = row1, 100-199 = row2)

    // --- Row 2 data ---
    std::vector<std::string> _inputChannels2;
    std::vector<std::string> _outputChannels2;
    std::vector<int> _routing2; // routing2[outIdx] = inIdx (0-99 = row1, 100-199 = row2)

    // Drag state
    int _draggingFromOutput; // -1 = not dragging from output side
    int _draggingFromInput;  // -1 = not dragging from input side
    int _activeRow;          // 0 = row 1, 1 = row 2 (which row the drag is in)
    QPoint _dragPos;

    // Row 1 combos
    QComboBox* _inputSourceCombo;  // "B" or "A"
    QComboBox* _inputLayerCombo;
    QComboBox* _outputLayerCombo;

    // Row 2 combos
    QComboBox* _inputSourceCombo2; // "B" or "A"
    QComboBox* _inputLayerCombo2;
    QComboBox* _outputLayerCombo2;

    static const int SOCKET_RADIUS = 7;
    static const int HIT_RADIUS = 11;
    static const int ROW_HEIGHT = 28;
    static const int MARGIN = 16;
    static const int ROW_GAP = 12;
    static const int CONST_BTN_RADIUS = 5;
    static const int CONST_BTN_GAP = 4;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SHUFFLEWIDGET_H
