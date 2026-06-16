#include "AndroidInterface.h"

#include <QAndroidScreen.h>
#include <QtAndroidHelpers/QAndroidPartialWakeLocker.h>
#include <QtAndroidHelpers/QAndroidWiFiLocker.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QJniEnvironment>
#include <QtCore/QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#include <QtCore/QMetaObject>
#include <QtCore/QSharedPointer>
#include <QtCore/QStandardPaths>

#include "AppSettings.h"
#include "QGCApplication.h"
#include "QGCLoggingCategory.h"
#include "SettingsFact.h"
#include "SettingsManager.h"

QGC_LOGGING_CATEGORY(AndroidInterfaceLog, "Android.AndroidInterface")

namespace AndroidInterface {

static constexpr const char* kBundledSettingsAsset = "qgroundcontrol.ini";

static std::function<void(const QString&)> s_importCallback;

static void jniLogDebug(JNIEnv*, jobject, jstring message)
{
    qCDebug(AndroidInterfaceLog) << QJniObject(message).toString();
}

static void jniLogWarning(JNIEnv*, jobject, jstring message)
{
    qCWarning(AndroidInterfaceLog) << QJniObject(message).toString();
}

static void jniStoragePermissionsResult(JNIEnv*, jobject, jboolean granted)
{
    if (!qgcApp()) {
        return;
    }

    if (!granted) {
        qCWarning(AndroidInterfaceLog) << "Storage permission request denied; disabling save to SD card";

        (void)QMetaObject::invokeMethod(
            qgcApp(),
            []() {
                SettingsManager* const settingsManager = SettingsManager::instance();
                if (!settingsManager) {
                    return;
                }

                AppSettings* const appSettings = settingsManager->appSettings();
                if (!appSettings) {
                    return;
                }

                if (!appSettings->androidDontSaveToSDCard()->rawValue().toBool()) {
                    appSettings->androidDontSaveToSDCard()->setRawValue(true);
                }
            },
            Qt::QueuedConnection);
        return;
    }

    (void)QMetaObject::invokeMethod(
        qgcApp(),
        []() {
            SettingsManager* const settingsManager = SettingsManager::instance();
            if (!settingsManager) {
                return;
            }

            AppSettings* const appSettings = settingsManager->appSettings();
            if (!appSettings || appSettings->androidDontSaveToSDCard()->rawValue().toBool()) {
                return;
            }

            SettingsFact* const savePathFact = qobject_cast<SettingsFact*>(appSettings->savePath());
            if (!savePathFact) {
                return;
            }

            const QString appName = QCoreApplication::applicationName();
            const QString currentSavePath = savePathFact->rawValue().toString();
            const QString internalBasePath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
            const QString internalSavePath = QDir(internalBasePath).filePath(appName);

            if (!currentSavePath.isEmpty() && (currentSavePath != internalSavePath)) {
                return;
            }

            const QString sdCardRootPath = getSDCardPath();
            if (sdCardRootPath.isEmpty() || !QDir(sdCardRootPath).exists() || !QFileInfo(sdCardRootPath).isWritable()) {
                return;
            }

            const QString sdSavePath = QDir(sdCardRootPath).filePath(appName);
            if (currentSavePath != sdSavePath) {
                qCDebug(AndroidInterfaceLog) << "Applying SD card save path after permission grant:" << sdSavePath;
                savePathFact->setRawValue(sdSavePath);
            }
        },
        Qt::QueuedConnection);
}

static void jniOnImportResult(JNIEnv* env, jobject, jstring filePathA)
{
    const char* const filePathCStr = env->GetStringUTFChars(filePathA, nullptr);
    const QString filePath = QString::fromUtf8(filePathCStr);
    env->ReleaseStringUTFChars(filePathA, filePathCStr);
    (void)QJniEnvironment::checkAndClearExceptions(env);
    auto callback = std::move(s_importCallback);
    if (!callback) {
        return;
    }
    callback(filePath);
}

void setNativeMethods()
{
    qCDebug(AndroidInterfaceLog) << "Registering Native Functions";

    const JNINativeMethod javaMethods[]{
        {"qgcLogDebug", "(Ljava/lang/String;)V", reinterpret_cast<void*>(jniLogDebug)},
        {"qgcLogWarning", "(Ljava/lang/String;)V", reinterpret_cast<void*>(jniLogWarning)},
        {"nativeStoragePermissionsResult", "(Z)V", reinterpret_cast<void*>(jniStoragePermissionsResult)},
        {"onImportResult", "(Ljava/lang/String;)V", reinterpret_cast<void*>(jniOnImportResult)}};

    QJniEnvironment env;
    if (!env.registerNativeMethods(kJniQGCActivityClassName, javaMethods, std::size(javaMethods))) {
        qCWarning(AndroidInterfaceLog) << "Failed to register native methods for" << kJniQGCActivityClassName;
    } else {
        qCDebug(AndroidInterfaceLog) << "Native Functions Registered";
    }
}

void installBundledDefaultSettings()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QCoreApplication::organizationName(), QCoreApplication::applicationName());
    const QString settingsFile = settings.fileName();
    if (QFileInfo::exists(settingsFile)) {
        return;
    }

    const QFileInfo settingsFileInfo(settingsFile);
    if (!QDir().mkpath(settingsFileInfo.absolutePath())) {
        qCWarning(AndroidInterfaceLog) << "Failed to create settings directory for bundled defaults:" << settingsFileInfo.absolutePath();
        return;
    }

    const QJniObject assetPath = QJniObject::fromString(QString::fromUtf8(kBundledSettingsAsset));
    const QJniObject destPath = QJniObject::fromString(settingsFile);
    const auto context = QNativeInterface::QAndroidApplication::context();
    const jboolean copied = QJniObject::callStaticMethod<jboolean>(
        kJniQGCActivityClassName,
        "copyAssetToFile",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z",
        context.object<jobject>(),
        assetPath.object<jstring>(),
        destPath.object<jstring>());

    QJniEnvironment env;
    if (env.checkAndClearExceptions()) {
        qCWarning(AndroidInterfaceLog) << "Exception while installing bundled default settings";
        return;
    }

    if (copied) {
        qCDebug(AndroidInterfaceLog) << "Installed bundled default settings:" << settingsFile;
    } else {
        qCWarning(AndroidInterfaceLog) << "Failed to install bundled default settings from asset:" << kBundledSettingsAsset;
    }
}

bool checkStoragePermissions()
{
    const bool hasPermission =
        QJniObject::callStaticMethod<jboolean>(kJniQGCActivityClassName, "checkStoragePermissions", "()Z");
    QJniEnvironment env;
    if (env.checkAndClearExceptions()) {
        qCWarning(AndroidInterfaceLog) << "Exception in checkStoragePermissions";
        return false;
    }

    if (hasPermission) {
        qCDebug(AndroidInterfaceLog) << "Storage permissions granted";
    } else {
        qCWarning(AndroidInterfaceLog) << "Storage permissions not granted";
    }

    return hasPermission;
}

QString getSDCardPath()
{
    if (!checkStoragePermissions()) {
        qCWarning(AndroidInterfaceLog) << "Storage Permission Denied";
        return QString();
    }

    const QJniObject result =
        QJniObject::callStaticObjectMethod(kJniQGCActivityClassName, "getSDCardPath", "()Ljava/lang/String;");
    QJniEnvironment env;
    if (env.checkAndClearExceptions()) {
        qCWarning(AndroidInterfaceLog) << "Exception in getSDCardPath";
        return QString();
    }
    if (!result.isValid()) {
        qCWarning(AndroidInterfaceLog) << "Call to java getSDCardPath failed: Invalid Result";
        return QString();
    }

    return result.toString();
}

void openFileImportDialog(const QString& destPath, std::function<void(const QString&)> callback)
{
    s_importCallback = std::move(callback);

    const QJniObject jDestPath = QJniObject::fromString(destPath);
    QJniObject::callStaticMethod<void>(
        kJniQGCActivityClassName,
        "openFileImportDialog",
        "(Ljava/lang/String;)V",
        jDestPath.object<jstring>());

    QJniEnvironment env;
    if (env.checkAndClearExceptions()) {
        qCWarning(AndroidInterfaceLog) << "Exception in openFileImportDialog";
        if (s_importCallback) {
            auto cb = std::move(s_importCallback);
            cb(QString());
        }
    }
}

static QSharedPointer<QLocks::QLockBase> s_partialWakeLock;
static QSharedPointer<QLocks::QLockBase> s_wifiLock;

void setKeepScreenOn(bool on)
{
    if (!QAndroidScreen::instance()) {
        new QAndroidScreen(QCoreApplication::instance());
    }
    QAndroidScreen::instance()->keepScreenOn(on);

    if (on) {
        s_partialWakeLock = QAndroidPartialWakeLocker::instance().getLock();
        s_wifiLock = QAndroidWiFiLocker::instance().getLock();
    } else {
        s_partialWakeLock.reset();
        s_wifiLock.reset();
    }
}

}  // namespace AndroidInterface
