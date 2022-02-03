/*
    SPDX-FileCopyrightText: Lieven Hey <lieven.hey@kdab.com>
    SPDX-FileCopyrightText: 2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "perfrecordssh.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <csignal>

#include "hotspot-config.h"

QString sshOutput(const QString& hostname, const QStringList& command)
{
    QProcess ssh;
    ssh.setProgram(QStandardPaths::findExecutable(QLatin1String("ssh")));
    const auto arguments = QStringList({hostname}) + command;
    ssh.setArguments(arguments);
    ssh.start();
    ssh.waitForFinished();
    return QString::fromUtf8(ssh.readAll());
}

int sshExitCode(const QString& hostname, const QStringList& command)
{
    QProcess ssh;
    ssh.setProgram(QStandardPaths::findExecutable(QLatin1String("ssh")));
    const auto arguments = QStringList({hostname}) + command;
    ssh.setArguments(arguments);
    ssh.start();
    ssh.waitForFinished();
    return ssh.exitCode();
}

PerfRecordSSH::PerfRecordSSH(QObject* parent)
    : PerfRecord(parent)
{
    m_hostname = QStringLiteral("user@localhost");
}

PerfRecordSSH::~PerfRecordSSH() = default;

void PerfRecordSSH::record(const QStringList& perfOptions, const QString& outputPath, bool /*elevatePrivileges*/,
                           const QString& exePath, const QStringList& exeOptions, const QString& workingDirectory)
{
    int exitCode = sshExitCode(m_hostname, {QLatin1String("test"), QLatin1String("-e"), exePath});
    if (exitCode) {
        emit recordingFailed(tr("File '%1' does not exist.").arg(exePath));
    }

    exitCode = sshExitCode(m_hostname, {QLatin1String("test"), QLatin1String("-f"), exePath});
    if (exitCode) {
        emit recordingFailed(tr("'%1' is not a file.").arg(exePath));
    }

    exitCode = sshExitCode(m_hostname, {QLatin1String("test"), QLatin1String("-x"), exePath});
    if (exitCode) {
        emit recordingFailed(tr("File '%1' is not executable.").arg(exePath));
    }

    QStringList recordOptions = {exePath};
    recordOptions += exeOptions;

    startRecording(perfOptions, outputPath, recordOptions, workingDirectory);
}

void PerfRecordSSH::record(const QStringList& perfOptions, const QString& outputPath, bool /*elevatePrivileges*/,
                           const QStringList& pids)
{
    if (pids.empty()) {
        emit recordingFailed(tr("Process does not exist."));
        return;
    }

    QStringList options = perfOptions;
    options += {QStringLiteral("--pid"), pids.join(QLatin1Char(','))};
    startRecording(options, outputPath, {}, {});
}

void PerfRecordSSH::recordSystem(const QStringList& perfOptions, const QString& outputPath)
{
    auto options = perfOptions;
    options.append(QStringLiteral("--all-cpus"));
    startRecording(options, outputPath, {}, {});
}

void PerfRecordSSH::stopRecording()
{
    if (m_recordProcess) {
        m_userTerminated = true;
        m_outputFile->close();
        m_recordProcess->terminate();
        m_recordProcess->waitForFinished();
        m_recordProcess = nullptr;
    }
}

void PerfRecordSSH::sendInput(const QByteArray& input)
{
    if (m_recordProcess)
        m_recordProcess->write(input);
}

QString PerfRecordSSH::currentUsername()
{
    if (m_hostname.isEmpty())
        return {};
    return sshOutput(m_hostname, {QLatin1String("echo"), QLatin1String("$USERNAME")}).simplified();
}

bool PerfRecordSSH::canTrace(const QString& path)
{
    if (m_hostname.isEmpty())
        return false;

    // exit code == 0 -> true
    bool isDir = sshExitCode(m_hostname, {QLatin1String("test"), QLatin1String("-d"), path}) == 0;
    bool isReadable = sshExitCode(m_hostname, {QLatin1String("test"), QLatin1String("-r"), path}) == 0;

    if (!isDir || !isReadable) {
        return false;
    }

    QString paranoid =
        sshOutput(m_hostname, {QLatin1String("cat"), QLatin1String("/proc/sys/kernel/perf_event_paranoid")});
    return paranoid.trimmed() == QLatin1String("-1");
}

bool PerfRecordSSH::canProfileOffCpu()
{
    if (m_hostname.isEmpty())
        return false;
    return canTrace(QStringLiteral("events/sched/sched_switch"));
}

static QString perfRecordHelp(const QString& hostname)
{
    static const QString recordHelp = [hostname]() {
        static QString help =
            sshOutput(hostname, {QLatin1String("perf"), QLatin1String("record"), QLatin1String("--help")});
        if (help.isEmpty()) {
            // no man page installed, assume the best
            help = QLatin1String("--sample-cpu --switch-events");
        }
        return help;
    }();
    return recordHelp;
}

static QString perfBuildOptions(const QString& hostname)
{
    static const QString buildOptionsHelper = [hostname]() {
        static QString buildOptions =
            sshOutput(hostname, {QLatin1String("perf"), QLatin1String("version"), QLatin1String("--build-options")});
        return buildOptions;
    }();
    return buildOptionsHelper;
}

bool PerfRecordSSH::canSampleCpu()
{
    if (m_hostname.isEmpty())
        return false;
    return perfRecordHelp(m_hostname).contains(QLatin1String("--sample-cpu"));
}

bool PerfRecordSSH::canSwitchEvents()
{
    if (m_hostname.isEmpty())
        return false;
    return perfRecordHelp(m_hostname).contains(QLatin1String("--switch-events"));
}

bool PerfRecordSSH::canUseAio()
{
    // somehow this doesn't work with ssh
    return false;
}

bool PerfRecordSSH::canCompress()
{
    if (m_hostname.isEmpty())
        return false;
    return Zstd_FOUND && perfBuildOptions(m_hostname).contains(QLatin1String("zstd: [ on  ]"));
}

bool PerfRecordSSH::isPerfInstalled()
{
    if (m_hostname.isEmpty())
        return false;
    return sshExitCode(m_hostname, {QLatin1String("perf")}) != 127;
}

void PerfRecordSSH::startRecording(const QStringList& perfOptions, const QString& outputPath,
                                   const QStringList& recordOptions, const QString& workingDirectory)
{
    if (m_recordProcess) {
        stopRecording();
    }

    QFileInfo outputFileInfo(outputPath);
    QString folderPath = outputFileInfo.dir().path();
    QFileInfo folderInfo(folderPath);
    if (!folderInfo.exists()) {
        emit recordingFailed(tr("Folder '%1' does not exist.").arg(folderPath));
        return;
    }
    if (!folderInfo.isDir()) {
        emit recordingFailed(tr("'%1' is not a folder.").arg(folderPath));
        return;
    }
    if (!folderInfo.isWritable()) {
        emit recordingFailed(tr("Folder '%1' is not writable.").arg(folderPath));
        return;
    }

    QStringList perfCommand = {QStringLiteral("record"), QStringLiteral("-o"), QStringLiteral("-")};
    perfCommand += perfOptions;
    perfCommand += recordOptions;

    m_outputFile = new QFile(outputPath);
    m_outputFile->open(QIODevice::WriteOnly);

    m_recordProcess = new QProcess(this);
    m_recordProcess->setProgram(QStandardPaths::findExecutable(QLatin1String("ssh")));
    m_recordProcess->setArguments({m_hostname, QLatin1String("perf ") + perfCommand.join(QLatin1Char(' '))});
    m_recordProcess->start();
    m_recordProcess->waitForStarted();

    qDebug() << m_recordProcess->arguments().join(QLatin1Char(' '));

    emit recordingStarted(QLatin1String("perf"), perfCommand);

    connect(m_recordProcess, &QProcess::readyReadStandardOutput, this,
            [this] { m_outputFile->write(m_recordProcess->readAllStandardOutput()); });

    connect(m_recordProcess, &QProcess::readyReadStandardError, this,
            [this] { emit recordingOutput(QString::fromUtf8(m_recordProcess->readAllStandardError())); });

    connect(m_recordProcess, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                Q_UNUSED(exitStatus)

                m_outputFile->close();

                if ((exitCode == EXIT_SUCCESS || (exitCode == SIGTERM && m_userTerminated) || m_outputFile->size() > 0)
                    && m_outputFile->exists()) {
                    if (exitCode != EXIT_SUCCESS && !m_userTerminated) {
                        emit debuggeeCrashed();
                    }
                    emit recordingFinished(m_outputFile->fileName());
                } else {
                    emit recordingFailed(tr("Failed to record perf data, error code %1.").arg(exitCode));
                }
                m_userTerminated = false;

                emit recordingFinished(m_outputFile->fileName());
            });
}
