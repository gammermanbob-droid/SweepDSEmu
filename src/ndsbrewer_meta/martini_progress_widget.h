// src/ndsbrewer_meta/martini_progress_widget.h
//
// The martini-glass liquid-fill loading indicator (garnish matching
// ndsbrewer.icns/the SweepDS Emu mark) shown whenever NDSBrewer has real
// work in flight: at startup while the ROM list/registry loads, and
// while building forwarders. Ported from a standalone reference SVG+JS
// prototype (a `setLoadingProgress(0-100)` demo) into a plain Qt widget --
// nothing here executes JS; SetProgress() re-renders the SVG with the
// liquid level and percentage text patched in directly.

#pragma once

#include <QWidget>

class QSvgWidget;
class QLabel;

class MartiniProgressWidget : public QWidget {
    Q_OBJECT

public:
    explicit MartiniProgressWidget(QWidget* parent = nullptr);

    // 0-100. Values outside that range are clamped, matching the
    // reference prototype's own setLoadingProgress().
    void SetProgress(int percent);

    void SetLabelText(const QString& text);

private:
    QSvgWidget* svg_ = nullptr;
    QLabel* label_ = nullptr;
    int percent_ = 0;
};
