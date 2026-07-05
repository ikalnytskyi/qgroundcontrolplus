#include "Video2Settings.h"
#include "VideoManager.h"

#include "QGCLoggingCategory.h"
#include <QtCore/QVariantList>

QGC_LOGGING_CATEGORY(Video2SettingsLog, "Settings.Video2Settings")

DECLARE_SETTINGGROUP(Video2, "Video2")
{
    // Setup enum values for videoSource settings into meta data
    QVariantList videoSourceList;
#if defined(QGC_GST_STREAMING) || defined(QGC_QT_STREAMING)
    videoSourceList.append(videoSourceRTSP);
#ifdef QGC_GST_WHEP
    videoSourceList.append(videoSourceWHEP);
#endif
    videoSourceList.append(videoSourceUDPH264);
    videoSourceList.append(videoSourceUDPH265);
    videoSourceList.append(videoSourceTCP);
    videoSourceList.append(videoSourceMPEGTS);
    videoSourceList.append(videoSource3DRSolo);
    videoSourceList.append(videoSourceParrotDiscovery);
    videoSourceList.append(videoSourceYuneecMantisG);

#ifdef QGC_HERELINK_AIRUNIT_VIDEO
    videoSourceList.append(videoSourceHerelinkAirUnit);
#else
    videoSourceList.append(videoSourceHerelinkHotspot);
#endif
#endif
    if (videoSourceList.count() == 0) {
        _noVideo = true;
        videoSourceList.append(videoSourceNoVideo);
        setUserVisible(false);
    } else {
        videoSourceList.insert(0, videoDisabled);
    }

    // make translated strings
    QStringList videoSourceCookedList;
    for (const QVariant& videoSource: videoSourceList) {
        videoSourceCookedList.append(Video2Settings::tr(videoSource.toString().toStdString().c_str()));
    }

    _nameToMetaDataMap[videoSourceName]->setEnumInfo(videoSourceCookedList, videoSourceList);

    // Set default value for videoSource
    _setDefaults();
}

void Video2Settings::_setDefaults()
{
    if (_noVideo) {
        _nameToMetaDataMap[videoSourceName]->setRawDefaultValue(videoSourceNoVideo);
    } else {
        _nameToMetaDataMap[videoSourceName]->setRawDefaultValue(videoDisabled);
    }
}

DECLARE_SETTINGSFACT_NO_FUNC(Video2Settings, videoSource)
{
    if (!_videoSourceFact) {
        _videoSourceFact = _createSettingsFact(videoSourceName);
        if (!_videoSourceFact->enumValues().contains(_videoSourceFact->rawValue().toString())) {
            if (_noVideo) {
                _videoSourceFact->setRawValue(videoSourceNoVideo);
            } else {
                _videoSourceFact->setRawValue(videoDisabled);
            }
        }
        connect(_videoSourceFact, &Fact::valueChanged, this, &Video2Settings::_configChanged);
    }
    return _videoSourceFact;
}

DECLARE_SETTINGSFACT_NO_FUNC(Video2Settings, udpUrl)
{
    if (!_udpUrlFact) {
        _udpUrlFact = _createSettingsFact(udpUrlName);
        connect(_udpUrlFact, &Fact::valueChanged, this, &Video2Settings::_configChanged);
    }
    return _udpUrlFact;
}

DECLARE_SETTINGSFACT_NO_FUNC(Video2Settings, rtspUrl)
{
    if (!_rtspUrlFact) {
        _rtspUrlFact = _createSettingsFact(rtspUrlName);
        connect(_rtspUrlFact, &Fact::valueChanged, this, &Video2Settings::_configChanged);
    }
    return _rtspUrlFact;
}

DECLARE_SETTINGSFACT_NO_FUNC(Video2Settings, whepUrl)
{
    if (!_whepUrlFact) {
        _whepUrlFact = _createSettingsFact(whepUrlName);
        connect(_whepUrlFact, &Fact::valueChanged, this, &Video2Settings::_configChanged);
    }
    return _whepUrlFact;
}

DECLARE_SETTINGSFACT_NO_FUNC(Video2Settings, tcpUrl)
{
    if (!_tcpUrlFact) {
        _tcpUrlFact = _createSettingsFact(tcpUrlName);
        connect(_tcpUrlFact, &Fact::valueChanged, this, &Video2Settings::_configChanged);
    }
    return _tcpUrlFact;
}

bool Video2Settings::streamConfigured(void)
{
    const QString vSource = videoSource()->rawValue().toString();
    if (vSource == videoSourceNoVideo || vSource == videoDisabled) {
        return false;
    }
    if (vSource == videoSourceUDPH264 || vSource == videoSourceUDPH265) {
        qCDebug(Video2SettingsLog) << "Testing configuration for UDP Stream:" << udpUrl()->rawValue().toString();
        return !udpUrl()->rawValue().toString().isEmpty();
    }
    if (vSource == videoSourceRTSP) {
        qCDebug(Video2SettingsLog) << "Testing configuration for RTSP Stream:" << rtspUrl()->rawValue().toString();
        return !rtspUrl()->rawValue().toString().isEmpty();
    }
    if (vSource == videoSourceWHEP) {
        qCDebug(Video2SettingsLog) << "Testing configuration for WHEP Stream:" << whepUrl()->rawValue().toString();
        return !whepUrl()->rawValue().toString().isEmpty();
    }
    if (vSource == videoSourceTCP) {
        qCDebug(Video2SettingsLog) << "Testing configuration for TCP Stream:" << tcpUrl()->rawValue().toString();
        return !tcpUrl()->rawValue().toString().isEmpty();
    }
    if (vSource == videoSourceMPEGTS) {
        qCDebug(Video2SettingsLog) << "Testing configuration for MPEG-TS Stream:" << udpUrl()->rawValue().toString();
        return !udpUrl()->rawValue().toString().isEmpty();
    }
    if (vSource == videoSourceHerelinkAirUnit) {
        qCDebug(Video2SettingsLog) << "Stream configured for Herelink Air Unit";
        return true;
    }
    if (vSource == videoSourceHerelinkHotspot) {
        qCDebug(Video2SettingsLog) << "Stream configured for Herelink Hotspot";
        return true;
    }
    return false;
}

void Video2Settings::_configChanged(QVariant)
{
    emit streamConfiguredChanged(streamConfigured());
}
