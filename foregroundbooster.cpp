// SPDX-FileCopyrightText: 2020 Henri Chain <henri.chain@enioka.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "foregroundbooster.h"
#include <QtCore>
#include <abstracttasksmodel.h>
#include <algorithm>
#include <fstream>

using namespace TaskManager;

ForegroundBooster::ForegroundBooster(QObject *parent)
    : QObject(parent)
    , m_tasksModel(new TasksModel(this))
    , m_settings(new BoosterSettings(this))
    , m_currentApp(nullptr)
    , m_debounceTimer(this)
{
    m_debounceTimer.setSingleShot(true);
    connect(&m_debounceTimer, &QTimer::timeout, this, &ForegroundBooster::onSwitchTimeout);

   connect(m_tasksModel, &TasksModel::activeTaskChanged, this, &ForegroundBooster::onActiveWindowChanged);
   connect(m_tasksModel, &TasksModel::activityChanged, this, &ForegroundBooster::onActiveWindowChanged);
   connect(m_tasksModel,
           &TasksModel::dataChanged,
           [this](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles) {
               Q_UNUSED(topLeft)
               Q_UNUSED(bottomRight)
               if (roles.contains(AbstractTasksModel::IsWindow)
                   || roles.contains(AbstractTasksModel::AppPid)
                   || roles.isEmpty()) {
                   m_debounceTimer.start(200);
               }
           });
   connect(m_tasksModel, &TasksModel::activityChanged, this, &ForegroundBooster::onActiveWindowChanged);
   connect(m_tasksModel, &TasksModel::virtualDesktopChanged, this, &ForegroundBooster::onActiveWindowChanged);
   connect(m_tasksModel, &TasksModel::countChanged, this, &ForegroundBooster::onActiveWindowChanged);
   connect(m_tasksModel, &TasksModel::rowsAboutToBeRemoved, this, &ForegroundBooster::onWindowRemoved);

    CGroupDeviceMemoryLimit limit;

    std::ifstream capacityStream = std::ifstream("/sys/fs/cgroup/dmem.capacity");
    if (capacityStream.is_open()) {
       for (std::string line; std::getline(capacityStream, line); ) {
          auto spacePos = line.find(' ');
          if (spacePos == std::string::npos)
             continue;
          const auto device = line.substr(0, spacePos);
          limit.path = QString::fromStdString(device);

          const unsigned long value = std::stoul(line.substr(spacePos + 1, line.size()));

          limit.limit = value;
          m_boostedGPUMemoryLimit.push_back(limit);
          limit.limit = 0;
          m_nonBoostedGPUMemoryLimit.push_back(limit);
       }
    }
}

ForegroundBooster::~ForegroundBooster()
{
}

void ForegroundBooster::onWindowRemoved(const QModelIndex &parent, int first, int last)
{
    for (int row = first; row <= last; row++) {
        const auto index = m_tasksModel->index(row, 0, parent);
        const auto pid = m_tasksModel->data(index, AbstractTasksModel::AppPid).toUInt();

        if (m_appsByPid.contains(pid)) {
            KApplicationScope *app = m_appsByPid.value(pid);
            // CRITICAL: If this is the currently boosted scope, do NOT delete it.
            // We need it to reset the weight later.
            if (app == m_currentApp) {
                qDebug() << "Keeping active scope in cache:" << app->id();
                m_appsByPid.remove(pid);
                m_currentAppOrphaned = true;
            } else {
                if (app) {
                    qDebug() << "Removing" << app->id() << "from cache";
                }
                delete app;
                m_appsByPid.remove(pid);
            }
        }
    }
}

void ForegroundBooster::onActiveWindowChanged()
{
    // Just detect the target and restart debounce timer — actual boost happens in timeout
    auto activeTaskIndex = m_tasksModel->activeTask();
    if (!m_tasksModel->data(activeTaskIndex, AbstractTasksModel::IsWindow)
             .toBool()) {
       activeTaskIndex = {};
       for (int i = 0; i < m_tasksModel->rowCount(); ++i) {
          const QModelIndex &idx = m_tasksModel->makeModelIndex(i);
          if (idx.data(AbstractTasksModel::IsActive).toBool()
              && idx.data(AbstractTasksModel::IsWindow).toBool()) {
             activeTaskIndex = idx;
             break;
          }
          if (m_tasksModel->groupMode() != TasksModel::GroupDisabled
              && m_tasksModel->rowCount(idx)) {
             for (int j = 0; j < m_tasksModel->rowCount(idx); ++j) {
                const QModelIndex &child = m_tasksModel->makeModelIndex(i, j);
                if (child.data(AbstractTasksModel::IsWindow).toBool()) {
                   activeTaskIndex = child;
                   break;
                }
             }
             if (activeTaskIndex != QModelIndex{}) break;
          }
       }
    }

    if (activeTaskIndex == QModelIndex{}) return;

    const auto appid = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::AppId).toString();
    const auto pid = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::AppPid).toUInt();
    const auto isWindow = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::IsWindow).toBool();

    if (!isWindow) return;
    if (pid == m_currentPid) return;

    qDebug() << "Window switch pending: " << m_currentAppid << " → " << appid << " (waiting 200 ms)";
    m_debounceTimer.start(200);
}

void ForegroundBooster::onSwitchTimeout()
{
    // Re-detect to confirm focus hasn't changed
    auto activeTaskIndex = m_tasksModel->activeTask();
    if (!m_tasksModel->data(activeTaskIndex, AbstractTasksModel::IsWindow)
             .toBool()) {
       activeTaskIndex = {};
       for (int i = 0; i < m_tasksModel->rowCount(); ++i) {
          const QModelIndex &idx = m_tasksModel->makeModelIndex(i);
          if (idx.data(AbstractTasksModel::IsActive).toBool()
              && idx.data(AbstractTasksModel::IsWindow).toBool()) {
             activeTaskIndex = idx;
             break;
          }
          if (m_tasksModel->groupMode() != TasksModel::GroupDisabled
              && m_tasksModel->rowCount(idx)) {
             for (int j = 0; j < m_tasksModel->rowCount(idx); ++j) {
                const QModelIndex &child = m_tasksModel->makeModelIndex(i, j);
                if (child.data(AbstractTasksModel::IsWindow).toBool()) {
                   activeTaskIndex = child;
                   break;
                }
             }
             if (activeTaskIndex != QModelIndex{}) break;
          }
       }
    }

    if (activeTaskIndex == QModelIndex{}) {
       qDebug() << "Switch cancelled: no active task";
       return;
    }

    const auto appid = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::AppId).toString();
    const auto pid = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::AppPid).toUInt();

    if (pid == m_currentPid) {
        qDebug() << "Switch cancelled: same PID on timeout" << appid;
        return;
    }

    // If switching between two game windows with the same appid, skip.
    // Wine/Proton often changes PIDs without changing the actual window.
    const bool prevWasGame = m_currentAppid.startsWith(QLatin1String("steam_app"));
    const bool nowIsGame = appid.startsWith(QLatin1String("steam_app"));
    if (appid == m_currentAppid && prevWasGame && nowIsGame) {
        qDebug() << "Switch cancelled: same game window (PID flicker)" << appid;
        return;
    }

    qDebug() << "Switch confirmed: " << m_currentAppid << " → " << appid;

    KApplicationScope *currentApp = m_appsByPid.value(pid);
    const auto prevApp = m_currentApp;

    if (currentApp == nullptr) {
        currentApp = KApplicationScope::fromPid(pid, this);
        m_appsByPid[pid] = currentApp;
        if (currentApp == nullptr) {
            m_currentPid = pid;
            m_currentAppid = appid;
            m_currentApp = currentApp;
            return;
        }
    }

    if (prevApp != currentApp) {
        if (prevApp != nullptr) {
            // Only reset if switching to a DIFFERENT cgroup.
            // Games and launchers (faugus/heroic/steam) often share the same cgroup scope.
            if (currentApp == nullptr || currentApp->cgroup() != prevApp->cgroup()) {
                qDebug() << "[RESET] Clearing weight for" << prevApp->id();
                prevApp->setCpuWeight(OptionalQULongLong());
                prevApp->setDeviceMemoryLow(m_nonBoostedGPUMemoryLimit);
            } else {
                qDebug() << "[SKIP RESET] Same cgroup scope" << prevApp->id();
            }
        }
        if (currentApp != nullptr) {
            qDebug() << "[BOOST] Setting weight to" << m_settings->boostedCpuWeight()
                     << "for" << currentApp->id();
            currentApp->setCpuWeight(m_settings->boostedCpuWeight());
            currentApp->setDeviceMemoryLow(m_boostedGPUMemoryLimit);
        }
    }

    m_currentPid = pid;
    m_currentAppid = appid;
    m_currentApp = currentApp;

    // If the old scope was evicted from the hash by onWindowRemoved,
    // it is safe to delete it now that we've finished using it.
    if (prevApp != nullptr && prevApp != currentApp && m_currentAppOrphaned) {
        qDebug() << "Cleaning up evicted scope:" << prevApp->id();
        delete prevApp;
    }
    m_currentAppOrphaned = false;
}
