// src/ndsbrewer_meta/martini_progress_widget.h
//
// The martini-glass liquid-fill loading indicator (garnish matching
// ndsbrewer.icns/the SweepDS Emu mark) shown whenever NDSBrewer has real
// work in flight: at startup while the ROM list/registry loads, and
// while building forwarders. Ported from a standalone reference SVG+JS
// prototype (a `setLoadingProgress(0-100)` demo) into a plain Qt widget --
// nothing here executes JS; SetProgress() re-renders the SVG with the
// liquid level and percentage text patched in directly.
//
// Qt6::Svg/SvgWidgets aren't available in every environment this project
// otherwise builds in (see CMakeLists.txt's NDSBREWER_HAS_SVG comment),
// so when it's missing this falls back to a plain QProgressBar with the
// same SetProgress()/SetLabelText() API rather than losing the loading
// indicator (or the build) entirely.

#pragma once

#include <QWidget>

class QLabel;
#ifdef NDSBREWER_HAS_SVG
class QSvgWidget;
#else
class QProgressBar;
#endif

class MartiniProgressWidget : public QWidget {
    Q_OBJECT

public:
    explicit MartiniProgressWidget(QWidget* parent = nullptr);

    // 0-100. Values outside that range are clamped, matching the
    // reference prototype's own setLoadingProgress().
    void SetProgress(int percent);

    void SetLabelText(const QString& text);

private:
#ifdef NDSBREWER_HAS_SVG
    QSvgWidget* svg_ = nullptr;
#else
    QProgressBar* progress_bar_ = nullptr;
#endif
    QLabel* label_ = nullptr;
    int percent_ = 0;
};
