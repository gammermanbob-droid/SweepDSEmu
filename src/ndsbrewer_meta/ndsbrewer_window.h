// src/ndsbrewer_meta/ndsbrewer_window.h
//
// SweepDSEmuNDSBrewer's one and only window: pick which .nds/.dsi ROMs
// (from the current profile's sdmc/roms/nds and sdmc/roms/dsi) should get
// a DS forwarder CIA, optionally install each one immediately (deleting
// the built .cia once installed), and manage/delete forwarders that
// already exist. Replaces the old manual workflow of running
// tools/make_ds_forwarder.py by hand per ROM with no way to see or remove
// what's already registered.

#pragma once

#include <QWidget>

class QCheckBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;

class NDSBrewerWindow : public QWidget {
    Q_OBJECT

public:
    explicit NDSBrewerWindow(QWidget* parent = nullptr);
    ~NDSBrewerWindow() override;

private:
    void RefreshRomList();
    void RefreshForwarderList();

    void OnSelectAllToggled(bool checked);
    void OnBuildClicked();
    void OnDeleteForwarderClicked();

    // Builds one forwarder CIA end to end (icon decode -> stub -> SMDH ->
    // banner -> CIA -> registry) and returns its output path, or an empty
    // string on failure with *out_error set to why.
    QString BuildForwarderCia(const QString& rom_path, QString* out_error);

    QListWidget* rom_list_ = nullptr;
    QCheckBox* select_all_ = nullptr;
    QCheckBox* install_after_build_ = nullptr;
    QPushButton* build_button_ = nullptr;

    QListWidget* forwarder_list_ = nullptr;
    QPushButton* delete_forwarder_button_ = nullptr;
};
