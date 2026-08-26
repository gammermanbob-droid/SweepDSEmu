// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <thread>

#include <QApplication>
#include <QElapsedTimer>

#include "common/logging/backend.h"
#include "ndsbrewer_meta/martini_progress_widget.h"
#include "ndsbrewer_meta/ndsbrewer_window.h"

int main(int argc, char* argv[]) {
    // FileUtil::UserPath resolution (and therefore which profile's
    // roms/nds folder and ds_forwarders.txt this tool operates on) is
    // shared with the main app and lazily self-initializes on first
    // FileUtil::GetUserPath() call -- this doesn't need its own CLI-arg
    // parsing (unlike citra_meta/CitraCLI) since it has no ROM/movie/
    // compression concepts of its own, just the one profile-relative
    // job of building and managing DS forwarders.
    Common::Log::Initialize();
    Common::Log::Start();

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SweepDSEmuNDSBrewer"));

    // Startup splash: the actual work behind it (scanning roms/nds +
    // roms/dsi and parsing ds_forwarders.txt in NDSBrewerWindow's
    // constructor) is normally fast enough to be imperceptible, but the
    // fill animation is still worth a brief, deliberate hold rather than
    // a one-frame flash -- same reasoning most apps' splash screens use
    // a minimum display time regardless of how fast the real load is.
    auto* splash = new MartiniProgressWidget();
    splash->setWindowFlag(Qt::SplashScreen);
    splash->SetLabelText(QObject::tr("Loading..."));
    splash->show();
    app.processEvents();

    NDSBrewerWindow window;

    constexpr int kMinSplashMs = 500;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < kMinSplashMs) {
        splash->SetProgress(static_cast<int>(timer.elapsed() * 100 / kMinSplashMs));
        app.processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    splash->SetProgress(100);
    app.processEvents();

    splash->close();
    splash->deleteLater();
    window.show();

    return app.exec();
}
