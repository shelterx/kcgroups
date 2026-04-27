// SPDX-FileCopyrightText: 2020 Henri Chain <henri.chain@enioka.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "foregroundbooster.h"
#include <QtCore>
#include <abstracttasksmodel.h>
#include <fstream>

using namespace TaskManager;

ForegroundBooster::ForegroundBooster(QObject *parent)
    : QObject(parent)
    , m_tasksModel(new TasksModel(this))
    , m_settings(new BoosterSettings(this))
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
    connect(m_tasksModel, &TasksModel::virtualDesktopChanged, this, &ForegroundBooster::onActiveWindowChanged);
    connect(m_tasksModel, &TasksModel::countChanged, this, &ForegroundBooster::onActiveWindowChanged);

    CGroupDeviceMemoryLimit limit;

    std::ifstream capacityStream("/sys/fs/cgroup/dmem.capacity");
    if (capacityStream.is_open()) {
        for (std::string line; std::getline(capacityStream, line); ) {
            auto spacePos = line.find(' ');
            if (spacePos == std::string::npos)
                continue;

            bool ok = false;
            const unsigned long value = QString::fromStdString(line.substr(spacePos + 1)).toULong(&ok);
            if (!ok) {
                qWarning() << "Failed to parse dmem.capacity line:" << QString::fromStdString(line);
                continue;
            }

            limit.path = QString::fromStdString(line.substr(0, spacePos));
            limit.limit = value;
            m_boostedGPUMemoryLimit.push_back(limit);
            limit.limit = 0;
            m_nonBoostedGPUMemoryLimit.push_back(limit);
        }
    }
}

ForegroundBooster::~ForegroundBooster()
{
    delete m_currentApp;
}

// Helper to find the active window index, checking grouped tasks too
static QModelIndex findActiveWindowIndex(TaskManager::TasksModel *model)
{
    auto activeTaskIndex = model->activeTask();
    if (model->data(activeTaskIndex, AbstractTasksModel::IsWindow).toBool())
        return activeTaskIndex;

    activeTaskIndex = {};
    for (int i = 0; i < model->rowCount(); ++i) {
        const QModelIndex &idx = model->makeModelIndex(i);
        if (idx.data(AbstractTasksModel::IsActive).toBool()
            && idx.data(AbstractTasksModel::IsWindow).toBool()) {
            return idx;
        }
        if (model->groupMode() != TasksModel::GroupDisabled
            && model->rowCount(idx)) {
            for (int j = 0; j < model->rowCount(idx); ++j) {
                const QModelIndex &child = model->makeModelIndex(i, j);
                if (child.data(AbstractTasksModel::IsWindow).toBool()) {
                    return child;
                }
            }
        }
    }
    return {};
}

void ForegroundBooster::onActiveWindowChanged()
{
    const auto activeTaskIndex = findActiveWindowIndex(m_tasksModel);
    if (activeTaskIndex == QModelIndex{}) return;

    const auto pid = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::AppPid).toUInt();
    const auto appid = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::AppId).toString();
    const auto isWindow = m_tasksModel->data(activeTaskIndex, AbstractTasksModel::IsWindow).toBool();

    if (!isWindow) return;
    if (pid == m_currentPid) return;

    qDebug() << "Window switch pending: " << m_currentAppid << " -> " << appid << " (waiting 200 ms)";
    m_debounceTimer.start(200);
}

void ForegroundBooster::onSwitchTimeout()
{
    const auto activeTaskIndex = findActiveWindowIndex(m_tasksModel);
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

    qDebug() << "Switch confirmed: " << m_currentAppid << " -> " << appid;

    KApplicationScope *newApp = KApplicationScope::fromPid(pid, nullptr);
    const auto prevApp = m_currentApp;

    if (newApp == nullptr) {
        // Can't resolve new app — still un-boost the previous one
        if (prevApp != nullptr) {
            qDebug() << "[RESET] Clearing weight for" << prevApp->id() << "(switching to unmanaged PID)";
            prevApp->setCpuWeight(OptionalQULongLong());
            prevApp->setDeviceMemoryLow(m_nonBoostedGPUMemoryLimit);
            delete prevApp;
        }
        m_currentPid = pid;
        m_currentAppid = appid;
        m_currentApp = nullptr;
        return;
    }

    if (prevApp != nullptr) {
        // Guard - Only reset if switching to a DIFFERENT cgroup.
        // Games and launchers (faugus/heroic/steam) may share the same cgroup scope depending on configuration.
        if (newApp->cgroup() != prevApp->cgroup()) {
            qDebug() << "[RESET] Clearing weight for" << prevApp->id();
            prevApp->setCpuWeight(OptionalQULongLong());
            prevApp->setDeviceMemoryLow(m_nonBoostedGPUMemoryLimit);
            delete prevApp;

            qDebug() << "[BOOST] Setting weight to" << m_settings->boostedCpuWeight()
                     << "for" << newApp->id();
            newApp->setCpuWeight(m_settings->boostedCpuWeight());
            newApp->setDeviceMemoryLow(m_boostedGPUMemoryLimit);
        } else {
            qDebug() << "[SKIP] Same cgroup scope, keeping boost as-is:" << newApp->id();
            delete prevApp;
        }
    } else {
        qDebug() << "[BOOST] Setting weight to" << m_settings->boostedCpuWeight()
                 << "for" << newApp->id();
        newApp->setCpuWeight(m_settings->boostedCpuWeight());
        newApp->setDeviceMemoryLow(m_boostedGPUMemoryLimit);
    }

    m_currentPid = pid;
    m_currentAppid = appid;
    m_currentApp = newApp;
}