// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "vaultcredentials.h"

#include <QByteArray>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_MACOS)
#include <Security/Security.h>
#else
#include <QProcess>
#include <QStandardPaths>
#endif

namespace schedule::vaultCredentials {
namespace {

// "Schedule Vault Manager" -> "ScheduleVaultManager", the form used where the
// store wants an identifier rather than a display name.
QString compact(const QString &application)
{
    QString name = application;
    name.remove(QLatin1Char(' '));
    return name;
}

#if defined(Q_OS_WIN)
std::wstring targetName(const QString &application, const QString &key)
{
    return (compact(application) + QLatin1Char('/') + key).toStdWString();
}
#elif !defined(Q_OS_MACOS)
bool secretTool(const QStringList &args, const QByteArray &input, QByteArray *output = nullptr)
{
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
    if (tool.isEmpty())
        return false;
    QProcess process;
    process.start(tool, args);
    if (!process.waitForStarted(1500))
        return false;
    if (!input.isEmpty())
        process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(2500)) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return false;
    if (output)
        *output = process.readAllStandardOutput();
    return true;
}

QStringList attributes(const QString &application, const QString &key)
{
    return {QStringLiteral("application"), compact(application), QStringLiteral("profile"), key};
}
#endif

} // namespace

bool store(const QString &application, const QString &key, const QString &password)
{
    const QByteArray secret = password.toUtf8();
#if defined(Q_OS_WIN)
    auto target = targetName(application, key);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.CredentialBlob = reinterpret_cast<BYTE *>(const_cast<char *>(secret.constData()));
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0);
#elif defined(Q_OS_MACOS)
    const QByteArray service = application.toUtf8();
    const QByteArray account = key.toUtf8();
    SecKeychainItemRef item = nullptr;
    const OSStatus found = SecKeychainFindGenericPassword(nullptr, service.size(), service.constData(),
        account.size(), account.constData(), nullptr, nullptr, &item);
    if (found == errSecItemNotFound) {
        return SecKeychainAddGenericPassword(nullptr, service.size(), service.constData(),
            account.size(), account.constData(), secret.size(), secret.constData(), nullptr) == errSecSuccess;
    }
    if (found != errSecSuccess)
        return false;
    const auto result = SecKeychainItemModifyAttributesAndData(item, nullptr, secret.size(), secret.constData());
    CFRelease(item);
    return result == errSecSuccess;
#else
    return secretTool(QStringList{QStringLiteral("store"), QStringLiteral("--label=") + application}
                      + attributes(application, key), secret.toBase64());
#endif
}

bool read(const QString &application, const QString &key, QString &password)
{
#if defined(Q_OS_WIN)
    const auto target = targetName(application, key);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return false;
    password = QString::fromUtf8(reinterpret_cast<const char *>(credential->CredentialBlob),
                                 credential->CredentialBlobSize);
    CredFree(credential);
#elif defined(Q_OS_MACOS)
    const QByteArray service = application.toUtf8();
    const QByteArray account = key.toUtf8();
    UInt32 size = 0;
    void *secret = nullptr;
    if (SecKeychainFindGenericPassword(nullptr, service.size(), service.constData(), account.size(),
            account.constData(), &size, &secret, nullptr) != errSecSuccess)
        return false;
    password = QString::fromUtf8(static_cast<const char *>(secret), size);
    SecKeychainItemFreeContent(nullptr, secret);
#else
    QByteArray secret;
    if (!secretTool(QStringList{QStringLiteral("lookup")} + attributes(application, key), {}, &secret))
        return false;
    if (secret.endsWith('\n'))
        secret.chop(1);
    const auto decoded = QByteArray::fromBase64(secret);
    if (decoded.isEmpty() || decoded.toBase64() != secret)
        return false;
    password = QString::fromUtf8(decoded);
#endif
    return true;
}

bool remove(const QString &application, const QString &key)
{
#if defined(Q_OS_WIN)
    const auto target = targetName(application, key);
    return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
#elif defined(Q_OS_MACOS)
    const QByteArray service = application.toUtf8();
    const QByteArray account = key.toUtf8();
    SecKeychainItemRef item = nullptr;
    const OSStatus found = SecKeychainFindGenericPassword(nullptr, service.size(), service.constData(),
        account.size(), account.constData(), nullptr, nullptr, &item);
    if (found != errSecSuccess)
        return found == errSecItemNotFound;
    const auto result = SecKeychainItemDelete(item);
    CFRelease(item);
    return result == errSecSuccess;
#else
    return secretTool(QStringList{QStringLiteral("clear")} + attributes(application, key), {});
#endif
}

} // namespace schedule::vaultCredentials
