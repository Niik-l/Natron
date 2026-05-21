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

#include "ShuffleWidget.h"

#include <QComboBox>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <cmath>

NATRON_NAMESPACE_ENTER

ShuffleWidget::ShuffleWidget(QWidget* parent)
    : QWidget(parent)
    , _draggingFromOutput(-1)
    , _draggingFromInput(-1)
    , _activeRow(0)
    , _inputSourceCombo(NULL)
    , _inputLayerCombo(NULL)
    , _outputLayerCombo(NULL)
    , _inputSourceCombo2(NULL)
    , _inputLayerCombo2(NULL)
    , _outputLayerCombo2(NULL)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_Hover, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // --- Row 1 combos ---
    _inputSourceCombo = new QComboBox(this);
    _inputSourceCombo->setFixedHeight(20);
    _inputSourceCombo->setFixedWidth(40);
    _inputSourceCombo->addItem(QString::fromUtf8("B"));
    _inputSourceCombo->addItem(QString::fromUtf8("A"));
    _inputSourceCombo->setCurrentIndex(0); // default B

    _inputLayerCombo = new QComboBox(this);
    _inputLayerCombo->setFixedHeight(20);
    connect(_inputLayerCombo, SIGNAL(currentIndexChanged(int)), this, SIGNAL(inputLayerChanged(int)));

    _outputLayerCombo = new QComboBox(this);
    _outputLayerCombo->setFixedHeight(20);
    connect(_outputLayerCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(onOutputLayerComboTextChanged(int)));

    // --- Row 2 combos ---
    _inputSourceCombo2 = new QComboBox(this);
    _inputSourceCombo2->setFixedHeight(20);
    _inputSourceCombo2->setFixedWidth(40);
    _inputSourceCombo2->addItem(QString::fromUtf8("B"));
    _inputSourceCombo2->addItem(QString::fromUtf8("A"));
    _inputSourceCombo2->setCurrentIndex(1); // default A

    _inputLayerCombo2 = new QComboBox(this);
    _inputLayerCombo2->setFixedHeight(20);
    connect(_inputLayerCombo2, SIGNAL(currentIndexChanged(int)), this, SIGNAL(inputLayer2Changed(int)));

    _outputLayerCombo2 = new QComboBox(this);
    _outputLayerCombo2->setFixedHeight(20);
    connect(_outputLayerCombo2, SIGNAL(currentIndexChanged(int)), this, SLOT(onOutputLayer2ComboTextChanged(int)));

    // Default: RGBA passthrough for both rows
    _inputChannels = {"red", "green", "blue", "alpha"};
    _outputChannels = {"red", "green", "blue", "alpha"};
    _routing = {0, 1, 2, 3};

    _inputChannels2 = {"red", "green", "blue", "alpha"};
    _outputChannels2 = {"red", "green", "blue", "alpha"};
    _routing2 = {-1, -1, -1, -1}; // no auto-connect — user decides
}

// ==================== Row 1 setters ====================

void
ShuffleWidget::setInputChannels(const std::vector<std::string>& channels)
{
    _inputChannels = channels;
    _routing.resize(_outputChannels.size(), -1);
    for (size_t i = 0; i < _routing.size() && i < _inputChannels.size(); ++i) {
        _routing[i] = (int)i;
    }
    update();
}

void
ShuffleWidget::setOutputChannels(const std::vector<std::string>& channels)
{
    _outputChannels = channels;
    _routing.resize(_outputChannels.size(), -1);
    for (size_t i = 0; i < _routing.size() && i < _inputChannels.size(); ++i) {
        _routing[i] = (int)i;
    }
    update();
}

void
ShuffleWidget::setRouting(const std::vector<int>& routing)
{
    _routing = routing;
    _routing.resize(_outputChannels.size(), -1);
    update();
}

void
ShuffleWidget::setInputLayerChoices(const std::vector<std::string>& choices, int current)
{
    _inputLayerCombo->blockSignals(true);
    _inputLayerCombo->clear();
    for (size_t i = 0; i < choices.size(); ++i) {
        _inputLayerCombo->addItem(QString::fromUtf8(choices[i].c_str()));
    }
    if (current >= 0 && current < (int)choices.size()) {
        _inputLayerCombo->setCurrentIndex(current);
    }
    _inputLayerCombo->blockSignals(false);
}

void
ShuffleWidget::setOutputLayerChoices(const std::vector<std::string>& choices, int current)
{
    _outputLayerCombo->blockSignals(true);
    _outputLayerCombo->clear();
    for (size_t i = 0; i < choices.size(); ++i) {
        _outputLayerCombo->addItem(QString::fromUtf8(choices[i].c_str()));
    }
    // Always append "new" option
    _outputLayerCombo->addItem(QString::fromUtf8("new"));
    if (current >= 0 && current < (int)choices.size()) {
        _outputLayerCombo->setCurrentIndex(current);
    }
    _outputLayerCombo->blockSignals(false);
}

int ShuffleWidget::getInputLayerIndex() const { return _inputLayerCombo->currentIndex(); }
int ShuffleWidget::getOutputLayerIndex() const { return _outputLayerCombo->currentIndex(); }

// ==================== Row 2 setters ====================

void
ShuffleWidget::setInputChannels2(const std::vector<std::string>& channels)
{
    _inputChannels2 = channels;
    _routing2.resize(_outputChannels2.size(), -1);
    // Don't auto-connect — user decides
    update();
}

void
ShuffleWidget::setOutputChannels2(const std::vector<std::string>& channels)
{
    _outputChannels2 = channels;
    _routing2.resize(_outputChannels2.size(), -1);
    // Don't auto-connect — user decides
    update();
}

void
ShuffleWidget::setRouting2(const std::vector<int>& routing)
{
    _routing2 = routing;
    _routing2.resize(_outputChannels2.size(), -1);
    update();
}

void
ShuffleWidget::setInputLayerChoices2(const std::vector<std::string>& choices, int current)
{
    _inputLayerCombo2->blockSignals(true);
    _inputLayerCombo2->clear();
    for (size_t i = 0; i < choices.size(); ++i) {
        _inputLayerCombo2->addItem(QString::fromUtf8(choices[i].c_str()));
    }
    if (current >= 0 && current < (int)choices.size()) {
        _inputLayerCombo2->setCurrentIndex(current);
    }
    _inputLayerCombo2->blockSignals(false);
}

void
ShuffleWidget::setOutputLayerChoices2(const std::vector<std::string>& choices, int current)
{
    _outputLayerCombo2->blockSignals(true);
    _outputLayerCombo2->clear();
    for (size_t i = 0; i < choices.size(); ++i) {
        _outputLayerCombo2->addItem(QString::fromUtf8(choices[i].c_str()));
    }
    // Always append "new" option
    _outputLayerCombo2->addItem(QString::fromUtf8("new"));
    if (current >= 0 && current < (int)choices.size()) {
        _outputLayerCombo2->setCurrentIndex(current);
    }
    _outputLayerCombo2->blockSignals(false);
}

int ShuffleWidget::getInputLayerIndex2() const { return _inputLayerCombo2->currentIndex(); }
int ShuffleWidget::getOutputLayerIndex2() const { return _outputLayerCombo2->currentIndex(); }

// ==================== "new" layer detection ====================

void
ShuffleWidget::onOutputLayerComboTextChanged(int index)
{
    if (index >= 0 && _outputLayerCombo->itemText(index) == QString::fromUtf8("new")) {
        Q_EMIT newLayerRequested(0); // Row 1
    } else {
        Q_EMIT outputLayerChanged(index);
    }
}

void
ShuffleWidget::onOutputLayer2ComboTextChanged(int index)
{
    if (index >= 0 && _outputLayerCombo2->itemText(index) == QString::fromUtf8("new")) {
        Q_EMIT newLayerRequested(1); // Row 2
    } else {
        Q_EMIT outputLayer2Changed(index);
    }
}

// ==================== Color ====================

QColor
ShuffleWidget::channelColor(int idx) const
{
    switch (idx) {
        case 0: return QColor(200, 60, 60);   // Red
        case 1: return QColor(60, 180, 60);   // Green
        case 2: return QColor(70, 70, 210);   // Blue
        case 3: return QColor(160, 160, 160); // Alpha (grey)
        default: return QColor(120, 120, 120);
    }
}

// ==================== Geometry helpers ====================

float
ShuffleWidget::row1Top() const
{
    return (float)MARGIN;
}

float
ShuffleWidget::rowSectionHeight(int numChannels) const
{
    return (float)(ROW_HEIGHT + numChannels * ROW_HEIGHT + 4);
}

float
ShuffleWidget::row2Top() const
{
    int numRows1 = std::max((int)_inputChannels.size(), (int)_outputChannels.size());
    return row1Top() + rowSectionHeight(numRows1) + (float)ROW_GAP;
}

// ==================== Socket positions ====================

QPointF
ShuffleWidget::inputSocketPos(int idx) const
{
    float x = width() * 0.38f;
    float top = row1Top();
    float y = top + ROW_HEIGHT + ROW_HEIGHT * 0.5f + idx * ROW_HEIGHT;
    return QPointF(x, y);
}

QPointF
ShuffleWidget::outputSocketPos(int idx) const
{
    float x = width() * 0.62f;
    float top = row1Top();
    float y = top + ROW_HEIGHT + ROW_HEIGHT * 0.5f + idx * ROW_HEIGHT;
    return QPointF(x, y);
}

QPointF
ShuffleWidget::inputSocketPos2(int idx) const
{
    float x = width() * 0.38f;
    float top = row2Top();
    float y = top + ROW_HEIGHT + ROW_HEIGHT * 0.5f + idx * ROW_HEIGHT;
    return QPointF(x, y);
}

QPointF
ShuffleWidget::outputSocketPos2(int idx) const
{
    float x = width() * 0.62f;
    float top = row2Top();
    float y = top + ROW_HEIGHT + ROW_HEIGHT * 0.5f + idx * ROW_HEIGHT;
    return QPointF(x, y);
}

// ==================== Hit testing ====================

int
ShuffleWidget::hitTestInputSocket(const QPoint& pos) const
{
    int bestIdx = -1;
    float bestDist = (float)(HIT_RADIUS * HIT_RADIUS);
    for (int i = 0; i < (int)_inputChannels.size(); ++i) {
        QPointF sp = inputSocketPos(i);
        float dx = pos.x() - sp.x();
        float dy = pos.y() - sp.y();
        float dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }
    return bestIdx;
}

int
ShuffleWidget::hitTestOutputSocket(const QPoint& pos) const
{
    int bestIdx = -1;
    float bestDist = (float)(HIT_RADIUS * HIT_RADIUS);
    for (int i = 0; i < (int)_outputChannels.size(); ++i) {
        QPointF sp = outputSocketPos(i);
        float dx = pos.x() - sp.x();
        float dy = pos.y() - sp.y();
        float dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }
    return bestIdx;
}

int
ShuffleWidget::hitTestInputSocket2(const QPoint& pos) const
{
    int bestIdx = -1;
    float bestDist = (float)(HIT_RADIUS * HIT_RADIUS);
    for (int i = 0; i < (int)_inputChannels2.size(); ++i) {
        QPointF sp = inputSocketPos2(i);
        float dx = pos.x() - sp.x();
        float dy = pos.y() - sp.y();
        float dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }
    return bestIdx;
}

int
ShuffleWidget::hitTestOutputSocket2(const QPoint& pos) const
{
    int bestIdx = -1;
    float bestDist = (float)(HIT_RADIUS * HIT_RADIUS);
    for (int i = 0; i < (int)_outputChannels2.size(); ++i) {
        QPointF sp = outputSocketPos2(i);
        float dx = pos.x() - sp.x();
        float dy = pos.y() - sp.y();
        float dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }
    return bestIdx;
}

// ==================== Constant button hit testing ====================

int
ShuffleWidget::hitTestConstantButton(const QPoint& pos, int& outIdx) const
{
    float hitR2 = (float)((CONST_BTN_RADIUS + 3) * (CONST_BTN_RADIUS + 3));
    for (int i = 0; i < (int)_outputChannels.size(); ++i) {
        QPointF sp = outputSocketPos(i);
        // Black button: to the left of the output socket
        float bx = sp.x() - SOCKET_RADIUS - CONST_BTN_GAP - CONST_BTN_RADIUS;
        float by = sp.y();
        float dx = pos.x() - bx;
        float dy = pos.y() - by;
        if (dx * dx + dy * dy < hitR2) {
            outIdx = i;
            return -2; // black
        }
        // White button: further left
        float wx = bx - CONST_BTN_RADIUS * 2 - CONST_BTN_GAP;
        dx = pos.x() - wx;
        dy = pos.y() - by;
        if (dx * dx + dy * dy < hitR2) {
            outIdx = i;
            return -3; // white
        }
    }
    return 0;
}

int
ShuffleWidget::hitTestConstantButton2(const QPoint& pos, int& outIdx) const
{
    float hitR2 = (float)((CONST_BTN_RADIUS + 3) * (CONST_BTN_RADIUS + 3));
    for (int i = 0; i < (int)_outputChannels2.size(); ++i) {
        QPointF sp = outputSocketPos2(i);
        // Black button: to the left of the output socket
        float bx = sp.x() - SOCKET_RADIUS - CONST_BTN_GAP - CONST_BTN_RADIUS;
        float by = sp.y();
        float dx = pos.x() - bx;
        float dy = pos.y() - by;
        if (dx * dx + dy * dy < hitR2) {
            outIdx = i;
            return -2; // black
        }
        // White button: further left
        float wx = bx - CONST_BTN_RADIUS * 2 - CONST_BTN_GAP;
        dx = pos.x() - wx;
        dy = pos.y() - by;
        if (dx * dx + dy * dy < hitR2) {
            outIdx = i;
            return -3; // white
        }
    }
    return 0;
}

// ==================== Size ====================

QSize
ShuffleWidget::sizeHint() const
{
    int numRows1 = std::max((int)_inputChannels.size(), (int)_outputChannels.size());
    int numRows2 = std::max((int)_inputChannels2.size(), (int)_outputChannels2.size());
    int totalH = MARGIN * 2 + (int)rowSectionHeight(numRows1) + ROW_GAP + (int)rowSectionHeight(numRows2) + 8;
    return QSize(380, totalH);
}

// ==================== Paint ====================

static void drawRowSection(QPainter& p,
                           float boxTop, float boxBottom, float midX, float widgetW,
                           QComboBox* inputCombo, QComboBox* outputCombo,
                           const QString& inputLabel, const QString& outputLabel)
{
    Q_UNUSED(inputLabel);
    Q_UNUSED(outputLabel);

    // Left box (Input Layer)
    QRectF leftBox(4, boxTop, midX - 8, boxBottom - boxTop);
    p.setPen(QPen(QColor(70, 70, 70), 1));
    p.setBrush(QColor(48, 48, 48));
    p.drawRoundedRect(leftBox, 3, 3);

    // Right box (Output Layer)
    QRectF rightBox(midX + 4, boxTop, midX - 8, boxBottom - boxTop);
    p.drawRoundedRect(rightBox, 3, 3);

    // Position combo boxes inside the header area
    int comboH = 20;
    int comboMargin = 8;
    // Leave space for the "In" source combo (45px wide)
    int srcComboW = 45;
    inputCombo->setGeometry(comboMargin + srcComboW + 2, (int)boxTop + 3, (int)midX - comboMargin * 2 - srcComboW - 2, comboH);
    outputCombo->setGeometry((int)midX + comboMargin, (int)boxTop + 3, (int)midX - comboMargin * 2, comboH);

    // Separator lines under combos
    p.setPen(QPen(QColor(65, 65, 65), 1));
    float sepY = boxTop + 26; // ROW_HEIGHT
    p.drawLine(QPointF(8, sepY), QPointF(midX - 8, sepY));
    p.drawLine(QPointF(midX + 8, sepY), QPointF(widgetW - 8, sepY));
}

void
ShuffleWidget::paintEvent(QPaintEvent* /*e*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Background
    p.fillRect(rect(), QColor(42, 42, 42));

    float midX = width() * 0.5f;

    // ---- Row 1 ----
    {
        int numRows1 = std::max((int)_inputChannels.size(), (int)_outputChannels.size());
        float boxTop = row1Top();
        float boxBottom = boxTop + rowSectionHeight(numRows1);

        drawRowSection(p, boxTop, boxBottom, midX, (float)width(),
                       _inputLayerCombo, _outputLayerCombo,
                       QString::fromUtf8("B Input"), QString::fromUtf8("Output"));

        // Position row 1 "In" source combo
        _inputSourceCombo->setGeometry(8, (int)boxTop + 3, 40, 20);
    }

    // ---- Row 2 ----
    {
        int numRows2 = std::max((int)_inputChannels2.size(), (int)_outputChannels2.size());
        float boxTop2 = row2Top();
        float boxBottom2 = boxTop2 + rowSectionHeight(numRows2);

        drawRowSection(p, boxTop2, boxBottom2, midX, (float)width(),
                       _inputLayerCombo2, _outputLayerCombo2,
                       QString::fromUtf8("A Input"), QString::fromUtf8("Output"));

        // Position row 2 "In" source combo
        _inputSourceCombo2->setGeometry(8, (int)boxTop2 + 3, 40, 20);
    }

    p.setFont(font());

    // ==================== Row 1 noodles ====================
    for (int outIdx = 0; outIdx < (int)_routing.size(); ++outIdx) {
        int inVal = _routing[outIdx];
        if (inVal < 0) continue; // skip disconnected (-1), black (-2), white (-3)

        // Skip if currently dragging this output in row 1
        if (_activeRow == 0 && outIdx == _draggingFromOutput) continue;

        QPointF from;
        QColor col;

        if (inVal < 100) {
            // Same-row connection (row 1 input)
            if (inVal >= (int)_inputChannels.size()) continue;
            from = inputSocketPos(inVal);
            col = channelColor(inVal);
        } else {
            // Cross-row connection (row 2 input)
            int idx2 = inVal - 100;
            if (idx2 >= (int)_inputChannels2.size()) continue;
            from = inputSocketPos2(idx2);
            col = channelColor(idx2).lighter(130);
        }
        QPointF to = outputSocketPos(outIdx);

        QPen noodlePen(col, 2.5f);
        p.setPen(noodlePen);

        QPainterPath path;
        path.moveTo(from);
        float cx = (from.x() + to.x()) * 0.5f;
        path.cubicTo(QPointF(cx, from.y()), QPointF(cx, to.y()), to);
        p.drawPath(path);
    }

    // ==================== Row 2 noodles ====================
    for (int outIdx = 0; outIdx < (int)_routing2.size(); ++outIdx) {
        int inVal = _routing2[outIdx];
        if (inVal < 0) continue; // skip disconnected (-1), black (-2), white (-3)

        // Skip if currently dragging this output in row 2
        if (_activeRow == 1 && outIdx == _draggingFromOutput) continue;

        QPointF from;
        QColor col;

        if (inVal >= 100) {
            // Same-row connection (row 2 input)
            int idx2 = inVal - 100;
            if (idx2 >= (int)_inputChannels2.size()) continue;
            from = inputSocketPos2(idx2);
            col = channelColor(idx2);
        } else {
            // Cross-row connection (row 1 input)
            if (inVal >= (int)_inputChannels.size()) continue;
            from = inputSocketPos(inVal);
            col = channelColor(inVal).lighter(130);
        }
        QPointF to = outputSocketPos2(outIdx);

        QPen noodlePen(col, 2.5f);
        p.setPen(noodlePen);

        QPainterPath path;
        path.moveTo(from);
        float cx = (from.x() + to.x()) * 0.5f;
        path.cubicTo(QPointF(cx, from.y()), QPointF(cx, to.y()), to);
        p.drawPath(path);
    }

    // ==================== Drag noodle ====================
    if (_draggingFromOutput >= 0) {
        QPointF to;
        if (_activeRow == 0) {
            to = outputSocketPos(_draggingFromOutput);
        } else {
            to = outputSocketPos2(_draggingFromOutput);
        }
        QPointF from(_dragPos);

        QPen dragPen(QColor(255, 200, 50), 2.5f, Qt::DashLine);
        p.setPen(dragPen);

        QPainterPath path;
        path.moveTo(from);
        float cx = (from.x() + to.x()) * 0.5f;
        path.cubicTo(QPointF(cx, from.y()), QPointF(cx, to.y()), to);
        p.drawPath(path);
    }

    if (_draggingFromInput >= 0) {
        QPointF from;
        if (_activeRow == 0) {
            from = inputSocketPos(_draggingFromInput);
        } else {
            from = inputSocketPos2(_draggingFromInput);
        }
        QPointF to(_dragPos);

        QPen dragPen(QColor(255, 200, 50), 2.5f, Qt::DashLine);
        p.setPen(dragPen);

        QPainterPath path;
        path.moveTo(from);
        float cx = (from.x() + to.x()) * 0.5f;
        path.cubicTo(QPointF(cx, from.y()), QPointF(cx, to.y()), to);
        p.drawPath(path);
    }

    // ==================== Row 1 sockets + labels ====================
    for (int i = 0; i < (int)_inputChannels.size(); ++i) {
        QPointF sp = inputSocketPos(i);
        QColor col = channelColor(i);

        p.setPen(QColor(180, 180, 180));
        QRectF labelRect(0, sp.y() - ROW_HEIGHT * 0.5f, sp.x() - SOCKET_RADIUS - 6, ROW_HEIGHT);
        p.drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter,
                   QString::fromUtf8(_inputChannels[i].c_str()));

        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawEllipse(sp, SOCKET_RADIUS, SOCKET_RADIUS);
    }

    for (int i = 0; i < (int)_outputChannels.size(); ++i) {
        QPointF sp = outputSocketPos(i);
        QColor col = channelColor(i);

        int routingVal = (i < (int)_routing.size()) ? _routing[i] : -1;

        // Draw constant buttons (black and white circles to the left of output socket)
        {
            // Black button position
            float bx = sp.x() - SOCKET_RADIUS - CONST_BTN_GAP - CONST_BTN_RADIUS;
            float by = sp.y();
            // White button position (further left)
            float wx = bx - CONST_BTN_RADIUS * 2 - CONST_BTN_GAP;

            // White (1.0) button
            if (routingVal == -3) {
                // Active: filled white with yellow outline
                p.setPen(QPen(QColor(255, 200, 50), 1.5f));
                p.setBrush(QColor(220, 220, 220));
            } else {
                p.setPen(QPen(QColor(100, 100, 100), 1.0f));
                p.setBrush(QColor(180, 180, 180));
            }
            p.drawEllipse(QPointF(wx, by), CONST_BTN_RADIUS, CONST_BTN_RADIUS);

            // Black (0.0) button
            if (routingVal == -2) {
                // Active: filled dark with yellow outline
                p.setPen(QPen(QColor(255, 200, 50), 1.5f));
                p.setBrush(QColor(30, 30, 30));
            } else {
                p.setPen(QPen(QColor(100, 100, 100), 1.0f));
                p.setBrush(QColor(40, 40, 40));
            }
            p.drawEllipse(QPointF(bx, by), CONST_BTN_RADIUS, CONST_BTN_RADIUS);
        }

        p.setPen(Qt::NoPen);

        bool connected = (routingVal >= 0);
        bool isConstant = (routingVal == -2 || routingVal == -3);
        if (connected) {
            p.setBrush(col);
        } else if (isConstant) {
            // Constant: half-fill effect
            p.setBrush(routingVal == -3 ? QColor(200, 200, 200) : QColor(30, 30, 30));
            p.setPen(QPen(QColor(255, 200, 50), 1.5f));
        } else {
            p.setBrush(QColor(50, 50, 50));
            p.setPen(QPen(col, 1.5f));
        }
        p.drawEllipse(sp, SOCKET_RADIUS, SOCKET_RADIUS);

        p.setPen(QColor(180, 180, 180));
        QRectF labelRect(sp.x() + SOCKET_RADIUS + 6, sp.y() - ROW_HEIGHT * 0.5f,
                         width() - sp.x() - SOCKET_RADIUS - 6, ROW_HEIGHT);
        p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8(_outputChannels[i].c_str()));
    }

    // ==================== Row 2 sockets + labels ====================
    for (int i = 0; i < (int)_inputChannels2.size(); ++i) {
        QPointF sp = inputSocketPos2(i);
        QColor col = channelColor(i);

        p.setPen(QColor(180, 180, 180));
        QRectF labelRect(0, sp.y() - ROW_HEIGHT * 0.5f, sp.x() - SOCKET_RADIUS - 6, ROW_HEIGHT);
        p.drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter,
                   QString::fromUtf8(_inputChannels2[i].c_str()));

        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawEllipse(sp, SOCKET_RADIUS, SOCKET_RADIUS);
    }

    for (int i = 0; i < (int)_outputChannels2.size(); ++i) {
        QPointF sp = outputSocketPos2(i);
        QColor col = channelColor(i);

        int routingVal2 = (i < (int)_routing2.size()) ? _routing2[i] : -1;

        // Draw constant buttons (black and white circles to the left of output socket)
        {
            float bx = sp.x() - SOCKET_RADIUS - CONST_BTN_GAP - CONST_BTN_RADIUS;
            float by = sp.y();
            float wx = bx - CONST_BTN_RADIUS * 2 - CONST_BTN_GAP;

            // White (1.0) button
            if (routingVal2 == -3) {
                p.setPen(QPen(QColor(255, 200, 50), 1.5f));
                p.setBrush(QColor(220, 220, 220));
            } else {
                p.setPen(QPen(QColor(100, 100, 100), 1.0f));
                p.setBrush(QColor(180, 180, 180));
            }
            p.drawEllipse(QPointF(wx, by), CONST_BTN_RADIUS, CONST_BTN_RADIUS);

            // Black (0.0) button
            if (routingVal2 == -2) {
                p.setPen(QPen(QColor(255, 200, 50), 1.5f));
                p.setBrush(QColor(30, 30, 30));
            } else {
                p.setPen(QPen(QColor(100, 100, 100), 1.0f));
                p.setBrush(QColor(40, 40, 40));
            }
            p.drawEllipse(QPointF(bx, by), CONST_BTN_RADIUS, CONST_BTN_RADIUS);
        }

        p.setPen(Qt::NoPen);

        bool connected = (routingVal2 >= 0);
        bool isConstant = (routingVal2 == -2 || routingVal2 == -3);
        if (connected) {
            p.setBrush(col);
        } else if (isConstant) {
            p.setBrush(routingVal2 == -3 ? QColor(200, 200, 200) : QColor(30, 30, 30));
            p.setPen(QPen(QColor(255, 200, 50), 1.5f));
        } else {
            p.setBrush(QColor(50, 50, 50));
            p.setPen(QPen(col, 1.5f));
        }
        p.drawEllipse(sp, SOCKET_RADIUS, SOCKET_RADIUS);

        p.setPen(QColor(180, 180, 180));
        QRectF labelRect(sp.x() + SOCKET_RADIUS + 6, sp.y() - ROW_HEIGHT * 0.5f,
                         width() - sp.x() - SOCKET_RADIUS - 6, ROW_HEIGHT);
        p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8(_outputChannels2[i].c_str()));
    }
}

// ==================== Mouse handling ====================

void
ShuffleWidget::mousePressEvent(QMouseEvent* e)
{
    // Right-click disconnects a wire
    if (e->button() == Qt::RightButton) {
        int outIdx = hitTestOutputSocket(e->pos());
        if (outIdx >= 0 && outIdx < (int)_routing.size()) {
            _routing[outIdx] = -1;
            Q_EMIT routingChanged();
            update();
            return;
        }
        outIdx = hitTestOutputSocket2(e->pos());
        if (outIdx >= 0 && outIdx < (int)_routing2.size()) {
            _routing2[outIdx] = -1;
            Q_EMIT routing2Changed();
            update();
            return;
        }
    }

    if (e->button() == Qt::LeftButton) {
        // Check constant buttons first (before socket drag)
        {
            int constOutIdx = -1;
            int constVal = hitTestConstantButton(e->pos(), constOutIdx);
            if (constVal != 0 && constOutIdx >= 0 && constOutIdx < (int)_routing.size()) {
                // Toggle: if already set to this constant, disconnect; otherwise set it
                if (_routing[constOutIdx] == constVal) {
                    _routing[constOutIdx] = -1; // disconnect
                } else {
                    _routing[constOutIdx] = constVal;
                }
                Q_EMIT routingChanged();
                update();
                return;
            }
            constVal = hitTestConstantButton2(e->pos(), constOutIdx);
            if (constVal != 0 && constOutIdx >= 0 && constOutIdx < (int)_routing2.size()) {
                if (_routing2[constOutIdx] == constVal) {
                    _routing2[constOutIdx] = -1;
                } else {
                    _routing2[constOutIdx] = constVal;
                }
                Q_EMIT routing2Changed();
                update();
                return;
            }
        }

        // Try row 1 output sockets first
        int outIdx = hitTestOutputSocket(e->pos());
        if (outIdx >= 0) {
            _draggingFromOutput = outIdx;
            _draggingFromInput = -1;
            _activeRow = 0;
            _dragPos = e->pos();
            update();
            return;
        }

        // Try row 2 output sockets
        outIdx = hitTestOutputSocket2(e->pos());
        if (outIdx >= 0) {
            _draggingFromOutput = outIdx;
            _draggingFromInput = -1;
            _activeRow = 1;
            _dragPos = e->pos();
            update();
            return;
        }

        // Try row 1 input sockets
        int inIdx = hitTestInputSocket(e->pos());
        if (inIdx >= 0) {
            _draggingFromInput = inIdx;
            _draggingFromOutput = -1;
            _activeRow = 0;
            _dragPos = e->pos();
            update();
            return;
        }

        // Try row 2 input sockets
        inIdx = hitTestInputSocket2(e->pos());
        if (inIdx >= 0) {
            _draggingFromInput = inIdx;
            _draggingFromOutput = -1;
            _activeRow = 1;
            _dragPos = e->pos();
            update();
            return;
        }
    }
    QWidget::mousePressEvent(e);
}

void
ShuffleWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (_draggingFromOutput >= 0 || _draggingFromInput >= 0) {
        _dragPos = e->pos();
        update();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void
ShuffleWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (_draggingFromOutput >= 0) {
        // Dragged from an output socket — drop on an input socket to rewire
        // Drop on empty = disconnect
        bool connected = false;

        if (_activeRow == 0) {
            int inIdx = hitTestInputSocket(e->pos());
            if (inIdx >= 0 && _draggingFromOutput < (int)_routing.size()) {
                _routing[_draggingFromOutput] = inIdx;
                connected = true;
            } else {
                inIdx = hitTestInputSocket2(e->pos());
                if (inIdx >= 0 && _draggingFromOutput < (int)_routing.size()) {
                    _routing[_draggingFromOutput] = 100 + inIdx;
                    connected = true;
                }
            }
            if (!connected && _draggingFromOutput < (int)_routing.size()) {
                _routing[_draggingFromOutput] = -1;
            }
            Q_EMIT routingChanged();
        } else {
            int inIdx = hitTestInputSocket2(e->pos());
            if (inIdx >= 0 && _draggingFromOutput < (int)_routing2.size()) {
                _routing2[_draggingFromOutput] = 100 + inIdx;
                connected = true;
            } else {
                inIdx = hitTestInputSocket(e->pos());
                if (inIdx >= 0 && _draggingFromOutput < (int)_routing2.size()) {
                    _routing2[_draggingFromOutput] = inIdx;
                    connected = true;
                }
            }
            if (!connected && _draggingFromOutput < (int)_routing2.size()) {
                _routing2[_draggingFromOutput] = -1; // disconnect
            }
            Q_EMIT routing2Changed();
        }

        _draggingFromOutput = -1;
        update();
        return;
    }

    if (_draggingFromInput >= 0) {
        // Dragged from an input socket -- drop on an output socket to rewire
        if (_activeRow == 0) {
            // Dragging from row 1 input
            int outIdx = hitTestOutputSocket(e->pos());
            if (outIdx >= 0 && outIdx < (int)_routing.size()) {
                _routing[outIdx] = _draggingFromInput; // row 1 input: 0-99
                Q_EMIT routingChanged();
            } else {
                // Check row 2 output (cross-row: row 1 input -> row 2 output)
                outIdx = hitTestOutputSocket2(e->pos());
                if (outIdx >= 0 && outIdx < (int)_routing2.size()) {
                    _routing2[outIdx] = _draggingFromInput; // row 1 input: 0-99
                    Q_EMIT routing2Changed();
                }
            }
        } else {
            // Dragging from row 2 input
            int outIdx = hitTestOutputSocket2(e->pos());
            if (outIdx >= 0 && outIdx < (int)_routing2.size()) {
                _routing2[outIdx] = 100 + _draggingFromInput; // row 2 input: 100-199
                Q_EMIT routing2Changed();
            } else {
                // Check row 1 output (cross-row: row 2 input -> row 1 output)
                outIdx = hitTestOutputSocket(e->pos());
                if (outIdx >= 0 && outIdx < (int)_routing.size()) {
                    _routing[outIdx] = 100 + _draggingFromInput; // row 2 input: 100-199
                    Q_EMIT routingChanged();
                }
            }
        }

        _draggingFromInput = -1;
        update();
        return;
    }

    QWidget::mouseReleaseEvent(e);
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_ShuffleWidget.cpp"
