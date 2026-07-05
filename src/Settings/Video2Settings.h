#pragma once

#include <QtQmlIntegration/QtQmlIntegration>

#include "SettingsGroup.h"

class Video2Settings : public SettingsGroup
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")
public:
    Video2Settings(QObject* parent = nullptr);
    DEFINE_SETTING_NAME_GROUP()

    DEFINE_SETTINGFACT(videoSource)
    DEFINE_SETTINGFACT(udpUrl)
    DEFINE_SETTINGFACT(tcpUrl)
    DEFINE_SETTINGFACT(rtspUrl)
    DEFINE_SETTINGFACT(whepUrl)

    Q_PROPERTY(bool     streamConfigured        READ streamConfigured       NOTIFY streamConfiguredChanged)
    Q_PROPERTY(QString  rtspVideoSource         READ rtspVideoSource        CONSTANT)
    Q_PROPERTY(QString  whepVideoSource         READ whepVideoSource        CONSTANT)
    Q_PROPERTY(QString  udp264VideoSource       READ udp264VideoSource      CONSTANT)
    Q_PROPERTY(QString  udp265VideoSource       READ udp265VideoSource      CONSTANT)
    Q_PROPERTY(QString  tcpVideoSource          READ tcpVideoSource         CONSTANT)
    Q_PROPERTY(QString  mpegtsVideoSource       READ mpegtsVideoSource      CONSTANT)
    Q_PROPERTY(QString  disabledVideoSource     READ disabledVideoSource    CONSTANT)

    bool     streamConfigured       ();
    QString  rtspVideoSource        () { return videoSourceRTSP; }
    QString  whepVideoSource        () { return videoSourceWHEP; }
    QString  udp264VideoSource      () { return videoSourceUDPH264; }
    QString  udp265VideoSource      () { return videoSourceUDPH265; }
    QString  tcpVideoSource         () { return videoSourceTCP; }
    QString  mpegtsVideoSource      () { return videoSourceMPEGTS; }
    QString  disabledVideoSource    () { return videoDisabled; }

    static constexpr const char* videoSourceNoVideo         = QT_TRANSLATE_NOOP("Video2Settings", "No Video Available");
    static constexpr const char* videoDisabled              = QT_TRANSLATE_NOOP("Video2Settings", "Video Stream Disabled");
    static constexpr const char* videoSourceRTSP            = QT_TRANSLATE_NOOP("Video2Settings", "RTSP Video Stream");
    static constexpr const char* videoSourceWHEP            = QT_TRANSLATE_NOOP("Video2Settings", "WHEP Video Stream");
    static constexpr const char* videoSourceUDPH264         = QT_TRANSLATE_NOOP("Video2Settings", "UDP h.264 Video Stream");
    static constexpr const char* videoSourceUDPH265         = QT_TRANSLATE_NOOP("Video2Settings", "UDP h.265 Video Stream");
    static constexpr const char* videoSourceTCP             = QT_TRANSLATE_NOOP("Video2Settings", "TCP-MPEG2 Video Stream");
    static constexpr const char* videoSourceMPEGTS          = QT_TRANSLATE_NOOP("Video2Settings", "MPEG-TS Video Stream");
    static constexpr const char* videoSource3DRSolo         = QT_TRANSLATE_NOOP("Video2Settings", "3DR Solo (requires restart)");
    static constexpr const char* videoSourceParrotDiscovery = QT_TRANSLATE_NOOP("Video2Settings", "Parrot Discovery");
    static constexpr const char* videoSourceYuneecMantisG   = QT_TRANSLATE_NOOP("Video2Settings", "Yuneec Mantis G");
    static constexpr const char* videoSourceHerelinkAirUnit = QT_TRANSLATE_NOOP("Video2Settings", "Herelink AirUnit");
    static constexpr const char* videoSourceHerelinkHotspot = QT_TRANSLATE_NOOP("Video2Settings", "Herelink Hotspot");

signals:
    void streamConfiguredChanged    (bool configured);

private slots:
    void _configChanged             (QVariant value);

private:
    void _setDefaults               ();

private:
    bool _noVideo = false;
};
