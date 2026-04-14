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

{
   connect(m_tasksModel, &TasksModel::activeTaskChanged, this, &ForegroundBooster::onActiveWindowChanged);
   connect(m_tasksModel, &TasksModel::activeTaskChanged, this, &ForegroundBooster::onActiveWindowChanged);
   connect(m_tasksModel,
           &TasksModel::dataChanged,
           [this](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles) {
               Q_UNUSED(topLeft)
               Q_UNUSED(bottomRight)
               if (roles.contains(AbstractTasksModel::IsWindow) || roles.isEmpty()) {
                   onActiveWindowChanged();
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
            } else {
                if (app) {
                    qDebug() << "Removing" << app->id() << "from cache";
                }
                delete app;
            }
            m_appsByPid.remove(pid);
        }
    }
}

void ForegroundBooster::onActiveWindowChanged()
{
    qDebug() << "Checking active tasks";
    auto activeTaskIndex = m_tasksModel->activeTask();
    if (!m_tasksModel->data(activeTaskIndex, AbstractTasksModel::IsWindow)
             .toBool()) {
       activeTaskIndex = {};
       for (int i = 0; i < m_tasksModel->rowCount(); ++i) {
          const QModelIndex &idx = m_tasksModel->makeModelIndex(i);

          if (idx.data(AbstractTasksModel::IsActive).toBool()) {
             if (idx.data(AbstractTasksModel::IsWindow).toBool()) {
                activeTaskIndex = idx;
                break;
             } else {
                const auto pid = m_tasksModel->data(idx, AbstractTasksModel::AppPid).toUInt();
                qDebug() << "No window detected, checking group children: " << pid;
             }
             if (m_tasksModel->groupMode() != TasksModel::GroupDisabled
                 && m_tasksModel->rowCount(idx)) {
                for (int j = 0; j < m_tasksModel->rowCount(idx); ++j) {
                   const QModelIndex &child
                       = m_tasksModel->makeModelIndex(i, j);

                   if (child.data(AbstractTasksModel::IsWindow).toBool()) {
                      activeTaskIndex = child;
                      break;
                   }
                }
                if (activeTaskIndex != QModelIndex{}) {
                   break;
                }
             }
          }
       }
    }

    if (activeTaskIndex == QModelIndex{}) {
       qDebug() << "No active task found";
       return;
    }

    const auto appid = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::AppId).toString();
    const auto pid = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::AppPid).toUInt();
    const auto isWindow = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::IsWindow).toBool();

    if (!isWindow) {
        qDebug() << "NOT WINDOW" << pid;

        return;
    }

    if (pid == m_currentPid) {
        qDebug() << "SAME PID" << appid;
        return;
    }

    const auto prevApp = m_currentApp;
    qDebug() << "Switching from" << m_currentAppid << "to" << appid;

    KApplicationScope *currentApp;

    if (m_appsByPid.contains(pid)) {
        currentApp = m_appsByPid[pid];
        if (currentApp == nullptr) {
            qDebug() << "Previous unmanaged app focused: appid =" << appid << ", pid =" << pid;
        } else {
            qDebug() << "Previous  systemd  app focused:" << currentApp->id() << ", appid =" << appid << ", pid =" << pid;
        }
    } else {
        qDebug() << "New window focused: appid =" << appid << ", pid =" << pid;
        currentApp = KApplicationScope::fromPid(pid, this);
        m_appsByPid[pid] = currentApp;
        if (currentApp == nullptr) {
            qDebug() << "This new window is not managed by systemd";
        }
    }

    if (prevApp != currentApp) {
        if (prevApp != nullptr) {
            qDebug() << "resetting" << prevApp->id() << "weight to default";
            prevApp->setCpuWeight(OptionalQULongLong());
            prevApp->setDeviceMemoryLow(m_nonBoostedGPUMemoryLimit);
        }
        if (currentApp != nullptr) {
            qDebug() << "setting" << currentApp->id() << "weight to" << (float)m_settings->boostedCpuWeight() / 100.
                    << "times normal weight";
            currentApp->setCpuWeight(m_settings->boostedCpuWeight());
            currentApp->setDeviceMemoryLow(m_boostedGPUMemoryLimit);
        }
    } else {
        qDebug() << "Changed to different window of same app";
    }
    m_currentPid = pid;
    m_currentAppid = appid;
    m_currentApp = currentApp;
}
