// SPDX-FileCopyrightText: 2020 Henri Chain <henri.chain@enioka.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef FOREGROUNDBOOSTER_H
#define FOREGROUNDBOOSTER_H
#include "boostersettings.h"
#include <KApplicationScope>
#include <QObject>
#include <QTimer>
#include <tasksmodel.h>

class KApplicationScope;

class ForegroundBooster : public QObject {
    Q_OBJECT
public:
    ForegroundBooster(QObject *parent = nullptr);
    ~ForegroundBooster();

public Q_SLOTS:
    void onActiveWindowChanged();
    void onSwitchTimeout();

private:
    TaskManager::TasksModel *m_tasksModel;
    BoosterSettings *m_settings;
    uint m_currentPid = 0;
    QString m_currentAppid;
    KApplicationScope *m_currentApp = nullptr;
    QTimer m_debounceTimer;

    CGroupDeviceMemoryLimitList m_boostedGPUMemoryLimit;
    CGroupDeviceMemoryLimitList m_nonBoostedGPUMemoryLimit;
};

#endif // FOREGROUNDBOOSTER_H