// src/ndsbrewer_meta/martini_progress_widget.cpp

#include "ndsbrewer_meta/martini_progress_widget.h"

#include <algorithm>
#include <cmath>

#include <QLabel>
#include <QVBoxLayout>
#ifdef NDSBREWER_HAS_SVG
#include <QSvgWidget>
#else
#include <QProgressBar>
#endif

#ifdef NDSBREWER_HAS_SVG
namespace {

// Glass triangle vertices (viewBox coordinates, matching the reference
// martini_progress.html prototype's own glass path exactly:
// "M140,300 L660,300 L403,552 Z"). The liquid's fill polygon is built
// directly from these three points rather than drawn as a plain
// rectangle clipped with an SVG <clipPath> -- Qt's SVG renderer doesn't
// apply that clip-path (the rectangle rendered in full, well outside the
// glass outline), so containment is guaranteed geometrically here
// instead: the polygon's two slanted sides are literally segments of the
// same lines as the glass's own left/right edges, so at any fill level
// the liquid can never extend past them.
constexpr double kRimY = 300.0;
constexpr double kApexY = 552.0;
constexpr double kApexX = 403.0;
constexpr double kLeftRimX = 140.0;
constexpr double kRightRimX = 660.0;

double LeftEdgeX(double y) {
    return kLeftRimX + (kApexX - kLeftRimX) * (y - kRimY) / (kApexY - kRimY);
}

double RightEdgeX(double y) {
    return kRightRimX + (kApexX - kRightRimX) * (y - kRimY) / (kApexY - kRimY);
}

// Builds the liquid's <path> "d" data: a gently wavy top edge between
// the glass's left and right edges at the current fill height, then
// straight down each edge to the apex (each of those two segments lies
// exactly on the glass's own edge line, per LeftEdgeX/RightEdgeX above).
QString BuildLiquidPathData(int percent) {
    const double top_y = kApexY - (kApexY - kRimY) * percent / 100.0;
    const double left_x = LeftEdgeX(top_y);
    const double right_x = RightEdgeX(top_y);
    const double width = right_x - left_x;

    QString d = QStringLiteral("M%1,%2 ").arg(left_x).arg(top_y);
    if (width > 8.0) {
        // A handful of small quadratic waves across the top edge,
        // amplitude scaled down as the surface narrows near the apex so
        // it never overshoots past the (also-narrowing) edges.
        constexpr int kSegments = 6;
        const double amplitude = std::min(8.0, width / (kSegments * 2.0));
        const double seg_w = width / kSegments;
        for (int i = 0; i < kSegments; i++) {
            const double x0 = left_x + seg_w * i;
            const double mid_x = x0 + seg_w / 2.0;
            const double end_x = x0 + seg_w;
            const double mid_y = top_y + ((i % 2 == 0) ? -amplitude : amplitude);
            d += QStringLiteral("Q%1,%2 %3,%4 ").arg(mid_x).arg(mid_y).arg(end_x).arg(top_y);
        }
    } else {
        d += QStringLiteral("L%1,%2 ").arg(right_x).arg(top_y);
    }
    d += QStringLiteral("L%1,%2 Z").arg(kApexX).arg(kApexY);
    return d;
}

const char kSvgTemplate[] = R"SVG(<svg width="400" height="380" viewBox="0 0 800 760" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="glassGrad" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#bfe3fb"/>
      <stop offset="45%" stop-color="#cdf3e6"/>
      <stop offset="100%" stop-color="#fdf0c2"/>
    </linearGradient>
    <linearGradient id="liquidGrad" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#fff2b8"/>
      <stop offset="50%" stop-color="#ffce6b"/>
      <stop offset="100%" stop-color="#ff9142"/>
    </linearGradient>
    <radialGradient id="orangeGrad" cx="35%" cy="28%" r="75%">
      <stop offset="0%" stop-color="#fff0d2"/>
      <stop offset="35%" stop-color="#ffb04a"/>
      <stop offset="100%" stop-color="#d9640c"/>
    </radialGradient>
    <radialGradient id="melonGrad" cx="35%" cy="28%" r="75%">
      <stop offset="0%" stop-color="#f2ffc9"/>
      <stop offset="40%" stop-color="#9ede4a"/>
      <stop offset="100%" stop-color="#458c1c"/>
    </radialGradient>
    <linearGradient id="pickGrad" x1="0" y1="0" x2="1" y2="0.2">
      <stop offset="0%" stop-color="#f5e6bd"/>
      <stop offset="50%" stop-color="#e7cd8f"/>
      <stop offset="100%" stop-color="#d9b96f"/>
    </linearGradient>
    <path id="sparkle" d="M0,-26 C4,-4 4,-4 26,0 C4,4 4,4 0,26 C-4,4 -4,4 -26,0 C-4,-4 -4,-4 0,-26 Z"/>
  </defs>

  <use href="#sparkle" transform="translate(70,90) scale(0.7)" fill="#eef2f6">
    <animate attributeName="opacity" values="0.35;0.9;0.35" dur="2.4s" repeatCount="indefinite"/>
  </use>
  <use href="#sparkle" transform="translate(742,540) scale(0.45)" fill="#eef2f6">
    <animate attributeName="opacity" values="0.9;0.35;0.9" dur="2.4s" repeatCount="indefinite"/>
  </use>
  <circle cx="46" cy="470" r="7" fill="none" stroke="#dfe6ec" stroke-width="3" opacity="0.8"/>
  <circle cx="758" cy="150" r="6" fill="none" stroke="#dfe6ec" stroke-width="3" opacity="0.8"/>
  <circle cx="400" cy="30" r="5" fill="none" stroke="#dfe6ec" stroke-width="3" opacity="0.8"/>

  <g stroke="#ffffff" stroke-opacity="0.9" stroke-linejoin="round" stroke-linecap="round">
    <path d="M140,300 L660,300 L403,552 Z" fill="url(#glassGrad)" fill-opacity="0.35" stroke-width="7"/>
    <rect x="391" y="552" width="22" height="118" rx="10" fill="url(#glassGrad)" fill-opacity="0.35" stroke-width="5"/>
    <ellipse cx="402" cy="676" rx="115" ry="19" fill="url(#glassGrad)" fill-opacity="0.35" stroke-width="5"/>
  </g>

  <path fill="url(#liquidGrad)" opacity="0.92" d="%1"/>

  <g fill="none" stroke="#ffffff" stroke-opacity="0.9" stroke-linejoin="round" stroke-linecap="round">
    <path d="M140,300 L660,300 L403,552 Z" stroke-width="7"/>
    <rect x="391" y="552" width="22" height="118" rx="10" stroke-width="5"/>
    <ellipse cx="402" cy="676" rx="115" ry="19" stroke-width="5"/>
  </g>

  <g id="garnish">
    <line x1="148" y1="278" x2="656" y2="278" stroke="#c9a86a" stroke-width="14" stroke-linecap="round"/>
    <line x1="148" y1="278" x2="656" y2="278" stroke="url(#pickGrad)" stroke-width="8" stroke-linecap="round"/>
    <g>
      <circle cx="258" cy="288" r="96" fill="url(#orangeGrad)" stroke="#ffffff" stroke-width="5" stroke-opacity="0.9"/>
      <ellipse cx="223" cy="247" rx="33" ry="21" fill="#fff6e6" opacity="0.85" transform="rotate(-18 223 247)"/>
      <ellipse cx="300" cy="330" rx="27" ry="15" fill="#c4540b" opacity="0.35"/>
    </g>
    <g>
      <circle cx="544" cy="288" r="96" fill="url(#melonGrad)" stroke="#ffffff" stroke-width="5" stroke-opacity="0.9"/>
      <ellipse cx="509" cy="247" rx="31" ry="19" fill="#f6ffe2" opacity="0.85" transform="rotate(-18 509 247)"/>
      <circle cx="566" cy="266" r="7" fill="#2e4d12" opacity="0.9"/>
      <circle cx="592" cy="292" r="6" fill="#2e4d12" opacity="0.9"/>
      <circle cx="560" cy="316" r="7" fill="#2e4d12" opacity="0.9"/>
      <circle cx="588" cy="336" r="5" fill="#2e4d12" opacity="0.9"/>
      <circle cx="536" cy="336" r="5" fill="#2e4d12" opacity="0.85"/>
    </g>
  </g>

  <text x="402" y="730" text-anchor="middle" font-family="Arial,sans-serif" font-weight="700" font-size="30" fill="#8b98a6">%2%</text>
</svg>
)SVG";

} // namespace
#endif // NDSBREWER_HAS_SVG

MartiniProgressWidget::MartiniProgressWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignHCenter);

#ifdef NDSBREWER_HAS_SVG
    svg_ = new QSvgWidget(this);
    svg_->setFixedSize(240, 228); // matches the SVG's 800:760 aspect ratio
    layout->addWidget(svg_, 0, Qt::AlignHCenter);
#else
    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setTextVisible(true);
    layout->addWidget(progress_bar_);
#endif

    label_ = new QLabel(this);
    label_->setAlignment(Qt::AlignHCenter);
    layout->addWidget(label_);

    SetProgress(0);
}

void MartiniProgressWidget::SetProgress(int percent) {
    percent_ = std::clamp(percent, 0, 100);
#ifdef NDSBREWER_HAS_SVG
    const QByteArray svg = QString::fromLatin1(kSvgTemplate)
                                .arg(BuildLiquidPathData(percent_))
                                .arg(percent_)
                                .toUtf8();
    svg_->load(svg);
#else
    progress_bar_->setValue(percent_);
#endif
}

void MartiniProgressWidget::SetLabelText(const QString& text) {
    label_->setText(text);
}
