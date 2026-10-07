/*
    SPDX-FileCopyrightText: 2026 Brent

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QTest>

#include "abstracttasksmodel.h"
#include "taskgroupingproxymodel.h"

using namespace TaskManager;

class MutableTasksModel : public AbstractTasksModel
{
public:
    struct TaskIdentity {
        QString appId;
        QUrl launcherUrl;
        bool isWindow = true;
    };

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_tasks.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_tasks.size())) {
            return {};
        }

        const TaskIdentity &task = m_tasks.at(index.row());
        switch (role) {
        case AppId:
            return task.appId;
        case LauncherUrl:
        case LauncherUrlWithoutIcon:
            return task.launcherUrl;
        case IsWindow:
            return task.isWindow;
        case IsLauncher:
            return !task.isWindow;
        case IsDemandingAttention:
        case SkipTaskbar:
            return false;
        default:
            return AbstractTasksModel::data(index, role);
        }
    }

    void appendWindow(const QString &appId, const QUrl &launcherUrl)
    {
        const int row = m_tasks.size();
        beginInsertRows({}, row, row);
        m_tasks.append({appId, launcherUrl, true});
        endInsertRows();
    }

    void appendLauncher(const QString &appId, const QUrl &launcherUrl)
    {
        const int row = m_tasks.size();
        beginInsertRows({}, row, row);
        m_tasks.append({appId, launcherUrl, false});
        endInsertRows();
    }

    void setIdentity(int row, const QString &appId, const QUrl &launcherUrl)
    {
        setIdentityWithoutNotification(row, appId, launcherUrl);
        notifyDataChanged(row, {AppId, LauncherUrl, LauncherUrlWithoutIcon, SkipTaskbar});
    }

    void setIdentityWithoutNotification(int row, const QString &appId, const QUrl &launcherUrl)
    {
        m_tasks[row].appId = appId;
        m_tasks[row].launcherUrl = launcherUrl;
    }

    void notifyDataChanged(int row, const QList<int> &roles)
    {
        const QModelIndex taskIndex = index(row, 0);
        Q_EMIT dataChanged(taskIndex, taskIndex, roles);
    }

private:
    QList<TaskIdentity> m_tasks;
};

class TaskGroupingProxyModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void identityChangeRegroupsImmediately();
    void identityChangeUngroupsImmediately();
    void launcherUrlChangeRegroupsAndUngroupsImmediately();
    void emptyRoleLauncherRefreshDoesNotReenterGrouping();
};

void TaskGroupingProxyModelTest::identityChangeRegroupsImmediately()
{
    MutableTasksModel source;
    source.appendWindow(QStringLiteral("ungoogled-chromium"),
                        QUrl(QStringLiteral("applications:ungoogled-chromium.desktop")));
    source.appendWindow(QStringLiteral("pwa-app-id"), QUrl(QStringLiteral("applications:chrome-pwa.desktop")));

    TaskGroupingProxyModel grouped;
    grouped.setSourceModel(&source);
    QCOMPARE(grouped.rowCount(), 2);

    // Wayland can update the window's application identity after task insertion.
    // The PWA keeps its own launcher URL but now maps to the browser's application.
    source.setIdentity(1,
                       QStringLiteral("ungoogled-chromium"),
                       QUrl(QStringLiteral("applications:chrome-pwa.desktop")));

    QCOMPARE(grouped.rowCount(), 1);
    const QModelIndex group = grouped.index(0, 0);
    QVERIFY(group.data(AbstractTasksModel::IsGroupParent).toBool());
    QCOMPARE(grouped.rowCount(group), 2);
}

void TaskGroupingProxyModelTest::identityChangeUngroupsImmediately()
{
    MutableTasksModel source;
    source.appendWindow(QStringLiteral("ungoogled-chromium"),
                        QUrl(QStringLiteral("applications:ungoogled-chromium.desktop")));
    source.appendWindow(QStringLiteral("ungoogled-chromium"), QUrl(QStringLiteral("applications:chrome-pwa.desktop")));

    TaskGroupingProxyModel grouped;
    grouped.setSourceModel(&source);
    QCOMPARE(grouped.rowCount(), 1);
    QCOMPARE(grouped.rowCount(grouped.index(0, 0)), 2);

    source.setIdentity(1, QStringLiteral("pwa-app-id"), QUrl(QStringLiteral("applications:chrome-pwa.desktop")));

    QCOMPARE(grouped.rowCount(), 2);
    QVERIFY(!grouped.index(0, 0).data(AbstractTasksModel::IsGroupParent).toBool());
    QVERIFY(!grouped.index(1, 0).data(AbstractTasksModel::IsGroupParent).toBool());
}

void TaskGroupingProxyModelTest::emptyRoleLauncherRefreshDoesNotReenterGrouping()
{
    constexpr int windowCount = 8;
    MutableTasksModel source;
    for (int i = 0; i < windowCount; ++i) {
        source.appendWindow(QStringLiteral("initial-app-%1").arg(i), QUrl(QStringLiteral("applications:initial-%1.desktop").arg(i)));
    }
    const int launcherRow = source.rowCount();
    source.appendLauncher(QStringLiteral("browser"), QUrl(QStringLiteral("applications:browser.desktop")));

    TaskGroupingProxyModel grouped;
    grouped.setSourceModel(&source);

    // Model TasksModel's empty-role launcher re-filter notification. This is
    // emitted synchronously when tryToGroup() reports a changed group parent.
    int feedbackDepth = 0;
    int maximumFeedbackDepth = 0;
    QObject::connect(&grouped,
                     &QAbstractItemModel::dataChanged,
                     &grouped,
                     [&](const QModelIndex &topLeft, const QModelIndex &, const QList<int> &roles) {
                         if (!roles.isEmpty() || !topLeft.data(AbstractTasksModel::IsGroupParent).toBool()) {
                             return;
                         }
                         ++feedbackDepth;
                         maximumFeedbackDepth = qMax(maximumFeedbackDepth, feedbackDepth);
                         source.notifyDataChanged(launcherRow, {});
                         --feedbackDepth;
                     });

    for (int row = 0; row < windowCount; ++row) {
        source.setIdentityWithoutNotification(row, QStringLiteral("browser"), QUrl(QStringLiteral("applications:browser.desktop")));
    }
    source.notifyDataChanged(0, {AbstractTasksModel::AppId, AbstractTasksModel::LauncherUrlWithoutIcon});

    QCOMPARE(grouped.rowCount(), 2); // One browser window group and the launcher.
    QCOMPARE(grouped.rowCount(grouped.index(0, 0)), windowCount);
    QCOMPARE(maximumFeedbackDepth, 1);
}

void TaskGroupingProxyModelTest::launcherUrlChangeRegroupsAndUngroupsImmediately()
{
    MutableTasksModel source;
    source.appendWindow(QStringLiteral("browser-id"), QUrl(QStringLiteral("applications:browser.desktop")));
    source.appendWindow(QStringLiteral("pwa-id"), QUrl(QStringLiteral("applications:pwa.desktop")));

    TaskGroupingProxyModel grouped;
    grouped.setSourceModel(&source);
    QCOMPARE(grouped.rowCount(), 2);

    // Launcher URLs are the fallback grouping key when the application IDs differ.
    source.setIdentity(1, QStringLiteral("pwa-id"), QUrl(QStringLiteral("applications:browser.desktop")));
    QCOMPARE(grouped.rowCount(), 1);
    QCOMPARE(grouped.rowCount(grouped.index(0, 0)), 2);

    source.setIdentity(1, QStringLiteral("pwa-id"), QUrl(QStringLiteral("applications:pwa.desktop")));
    QCOMPARE(grouped.rowCount(), 2);
}

QTEST_GUILESS_MAIN(TaskGroupingProxyModelTest)

#include "taskgroupingproxymodeltest.moc"
