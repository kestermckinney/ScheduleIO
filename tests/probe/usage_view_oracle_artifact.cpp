// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Creates and verifies the deterministic MPP used by the Microsoft Project
// usage-view interoperability oracle.
#include "mppio.h"

#include <cstdio>

namespace {
constexpr quint32 kTaskName = 188743694u;
constexpr quint32 kTaskWork = 188743680u;
constexpr quint32 kResourceName = 205520897u;
constexpr quint32 kResourceWork = 205520909u;

bool matches(const schedule::Project &project)
{
    return project.resourceUsageView.columnWidth(kResourceName) == 31
        && project.resourceUsageView.columnWidth(kResourceWork) == 17
        && project.resourceUsageView.timescaleSize == 135
        && project.taskUsageView.columnWidth(kTaskName) == 29
        && project.taskUsageView.columnWidth(kTaskWork) == 15
        && project.taskUsageView.timescaleSize == 145;
}
}

int main(int argc, char **argv)
{
    if (argc != 3 || (QString::fromLocal8Bit(argv[1]) != QStringLiteral("create")
                      && QString::fromLocal8Bit(argv[1]) != QStringLiteral("verify"))) {
        std::fprintf(stderr, "usage: usage_view_oracle_artifact <create|verify> <mpp-path>\n");
        return 2;
    }

    const QString action = QString::fromLocal8Bit(argv[1]);
    const QString path = QString::fromLocal8Bit(argv[2]);
    MppIO io;

    if (action == QStringLiteral("create")) {
        const QString fixture = QStringLiteral(SCHEDULEIO_FIXTURE_DIR)
            + QStringLiteral("/Average Project.mpp");
        if (!io.open(fixture)) {
            std::fprintf(stderr, "fixture open failed: %s\n", qPrintable(io.errorString()));
            return 1;
        }
        schedule::Project project = io.project();
        project.resourceUsageView.setColumnWidth(kResourceName, 31);
        project.resourceUsageView.setColumnWidth(kResourceWork, 17);
        project.resourceUsageView.setTimescaleSize(135);
        project.taskUsageView.setColumnWidth(kTaskName, 29);
        project.taskUsageView.setColumnWidth(kTaskWork, 15);
        project.taskUsageView.setTimescaleSize(145);
        io.setProject(project);
        if (!io.save(path)) {
            std::fprintf(stderr, "save failed: %s\n", qPrintable(io.errorString()));
            return 1;
        }
    } else if (!io.open(path)) {
        std::fprintf(stderr, "open failed: %s\n", qPrintable(io.errorString()));
        return 1;
    }

    if (!matches(io.project())) {
        const schedule::Project &project = io.project();
        std::fprintf(stderr,
            "usage settings mismatch: resource=(%d,%d,%d) task=(%d,%d,%d)\n",
            project.resourceUsageView.columnWidth(kResourceName),
            project.resourceUsageView.columnWidth(kResourceWork),
            project.resourceUsageView.timescaleSize,
            project.taskUsageView.columnWidth(kTaskName),
            project.taskUsageView.columnWidth(kTaskWork),
            project.taskUsageView.timescaleSize);
        return 1;
    }

    std::printf("usage settings verified: resource=(31,17,135) task=(29,15,145)\n");
    return 0;
}
