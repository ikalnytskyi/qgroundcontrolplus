#include "VideoManager.h"
#include "AppSettings.h"
#include "MavlinkCameraControlInterface.h"
#include "MultiVehicleManager.h"
#include "AppMessages.h"
#include "QGCApplication.h"
#include "QGCCameraManager.h"
#include "QGCCorePlugin.h"
#include "QGCLoggingCategory.h"
#include "QGCVideoStreamInfo.h"
#include "SettingsManager.h"
#include "SubtitleWriter.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"
#include "VideoReceiver.h"
#include "VideoSettings.h"
#include "Video2Settings.h"
#include "QtMultimediaReceiver.h"
#include "UVCReceiver.h"
#ifdef QGC_GST_STREAMING
#include "GStreamerHelpers.h"
#include "GStreamer.h"
#if defined(QGC_HAS_ANY_GPU_PATH)
#include "VideoReceiver/GStreamer/HwBuffers/QGCRhiCapture.h"
#endif
#include <QtMultimedia/QVideoSink>
#include <QtMultimediaQuick/private/qquickvideooutput_p.h>
#endif

#include <QtConcurrent/QtConcurrent>
#include <QtCore/QApplicationStatic>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFutureWatcher>
#include <QtCore/QPointer>
#include <QtCore/QRunnable>
#include <QtCore/QTimer>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

QGC_LOGGING_CATEGORY(VideoManagerLog, "Video.VideoManager")

// Fixed mapping: each receiver is permanently tied to one camera by index.
// "videoContent" → camera[0], "pipCamera1Video" → camera[1], etc.
// This means setCurrentCamera() triggers no stream restarts — only visual layout changes.
static int _cameraIndexForReceiver(const VideoReceiver *receiver)
{
    const QString &n = receiver->name();
    if (n == QLatin1String("videoContent"))    { return 0; }
    if (n == QLatin1String("pipCamera1Video")) { return 1; }
    if (n == QLatin1String("pipCamera2Video")) { return 2; }
    if (n == QLatin1String("pipCamera3Video")) { return 3; }
    return -1;
}

static constexpr const char *kFileExtension[VideoReceiver::FILE_FORMAT_MAX + 1] = {
    "mkv",
    "mov",
    "mp4"
};

Q_APPLICATION_STATIC(VideoManager, _videoManagerInstance);

bool VideoManager::_shouldSkipGStreamerForUnitTests()
{
    return qgcApp() && QGC::runningUnitTests() && !qEnvironmentVariableIsSet("QGC_TEST_ENABLE_GSTREAMER");
}

VideoManager::VideoManager(QObject *parent)
    : QObject(parent)
    , _subtitleWriter(new SubtitleWriter(this))
    , _videoSettings(SettingsManager::instance()->videoSettings())
    , _video2Settings(SettingsManager::instance()->video2Settings())
{
    qCDebug(VideoManagerLog) << this;

    (void) qRegisterMetaType<VideoReceiver::STATUS>("STATUS");

#ifdef QGC_GST_STREAMING
    _gstreamerDisabledForUnitTests = _shouldSkipGStreamerForUnitTests();
    if (_gstreamerDisabledForUnitTests) {
        qCInfo(VideoManagerLog) << "Skipping GStreamer initialization for unit tests";
    }
#endif
}

VideoManager::~VideoManager()
{
    qCDebug(VideoManagerLog) << this;
}

VideoManager *VideoManager::instance()
{
    return _videoManagerInstance();
}

void VideoManager::startGStreamerInit()
{
#ifdef QGC_GST_STREAMING
    if (_gstreamerDisabledForUnitTests) {
        _initState = InitState::GstReady;
        qCInfo(VideoManagerLog) << "GStreamer initialization disabled for unit tests";
        return;
    }

    if (_initState != InitState::NotStarted) {
        qCWarning(VideoManagerLog) << "GStreamer init already started";
        return;
    }

    _initState = InitState::Pending;

    GStreamer::prepareEnvironment();
    _gstInitFuture = QtConcurrent::run(&GStreamer::initialize);

    _gstInitFuture.then(this, [this](bool success) {
        _onGstInitComplete(success);
    }).onCanceled(this, [this] {
        _onGstInitComplete(false);
    });
#endif
}

bool VideoManager::waitForGStreamerInit(int timeoutMs)
{
#ifdef QGC_GST_STREAMING
    if (_gstreamerDisabledForUnitTests) {
        return true;
    }

    if (_initState == InitState::NotStarted) {
        startGStreamerInit();
    }

    switch (_initState) {
    case InitState::Failed:
        return false;
    case InitState::GstReady:
    case InitState::Running:
        return true;
    default:
        break;
    }

    if (!_gstInitFuture.isValid()) {
        qCCritical(VideoManagerLog) << "waitForGStreamerInit: no valid future";
        return false;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QFutureWatcher<bool> watcher;
    (void) connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);
    (void) connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    watcher.setFuture(_gstInitFuture);
    if (!watcher.isFinished()) {
        timer.start(timeoutMs);
        loop.exec();
    }

    if (!watcher.isFinished()) {
        qCCritical(VideoManagerLog) << "Timed out waiting for GStreamer init";
        return false;
    }

    const bool success = watcher.result();
    if (_initState == InitState::Pending || _initState == InitState::QmlReady) {
        _onGstInitComplete(success);
    }
    return _initState != InitState::Failed;
#else
    Q_UNUSED(timeoutMs);
    return true;
#endif
}

void VideoManager::init(QQuickWindow *mainWindow)
{
    if (_initialized) {
        qCDebug(VideoManagerLog) << "Video Manager already initialized";
        return;
    }

    if (!mainWindow) {
        qCCritical(VideoManagerLog) << "Failed To Init Video Manager - mainWindow is NULL";
        return;
    }
    _mainWindow = mainWindow;

#if defined(QGC_HAS_ANY_GPU_PATH)
    QGCRhiCapture::connectWindow(mainWindow);  // populate cached QRhi for GPU bridge handlers
#endif

    (void) connect(_videoSettings->videoSource(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->udpUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->rtspUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->whepUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->tcpUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_video2Settings->videoSource(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_video2Settings->udpUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_video2Settings->rtspUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_video2Settings->whepUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_video2Settings->tcpUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->aspectRatio(), &Fact::rawValueChanged, this, &VideoManager::aspectRatioChanged);
    (void) connect(_videoSettings->lowLatencyMode(), &Fact::rawValueChanged, this, [this](const QVariant &value) { Q_UNUSED(value); _restartAllVideos(); });
    (void) connect(SettingsManager::instance()->appSettings()->gstDebugLevel(), &Fact::rawValueChanged, this, [](const QVariant &value) {
#ifdef QGC_GST_STREAMING
        GStreamer::setDebugLevel(value.toInt());
#else
        Q_UNUSED(value);
#endif
    });
    (void) connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged, this, &VideoManager::_setActiveVehicle);

    (void) connect(this, &VideoManager::autoStreamConfiguredChanged, this, &VideoManager::_videoSourceChanged);

#ifdef QGC_GST_STREAMING
    if (_initState == InitState::NotStarted) {
        startGStreamerInit();
    }
#endif

    _mainWindow->scheduleRenderJob(
        QRunnable::create([this] {
            QMetaObject::invokeMethod(this, &VideoManager::_initAfterQmlIsReady, Qt::QueuedConnection);
        }),
        QQuickWindow::AfterSynchronizingStage);

    _initialized = true;
}

void VideoManager::_initAfterQmlIsReady()
{
    if (!_mainWindow) {
        qCCritical(VideoManagerLog) << "_initAfterQmlIsReady called with NULL mainWindow";
        return;
    }

    qCDebug(VideoManagerLog) << "_initAfterQmlIsReady";

#ifdef QGC_GST_STREAMING
    switch (_initState) {
    case InitState::Pending:
        _initState = InitState::QmlReady;
        qCDebug(VideoManagerLog) << "QML ready, waiting for GStreamer";
        return;
    case InitState::GstReady:
        _initState = InitState::Running;
        qCDebug(VideoManagerLog) << "QML ready, GStreamer already done — creating receivers";
        break;
    case InitState::Failed:
        qCWarning(VideoManagerLog) << "QML ready but GStreamer init failed";
        return;
    default:
        qCWarning(VideoManagerLog) << "_initAfterQmlIsReady: unexpected state" << static_cast<int>(_initState);
        return;
    }
#endif
    _createVideoReceivers();
}

void VideoManager::_onGstInitComplete(bool success)
{
    if (!success) {
        _initState = InitState::Failed;
        qCCritical(VideoManagerLog) << "GStreamer initialization failed";
        return;
    }

#ifdef QGC_GST_STREAMING
    if (_videoSettings) {
        const auto decoderOption = static_cast<GStreamer::VideoDecoderOptions>(
            _videoSettings->forceVideoDecoder()->rawValue().toInt());
        GStreamer::setCodecPriorities(decoderOption);
    }
#endif

    switch (_initState) {
    case InitState::Pending:
        _initState = InitState::GstReady;
        qCDebug(VideoManagerLog) << "GStreamer ready, waiting for QML";
        return;
    case InitState::QmlReady:
        _initState = InitState::Running;
        qCDebug(VideoManagerLog) << "GStreamer ready, QML already done — creating receivers";
        _createVideoReceivers();
        return;
    default:
        qCWarning(VideoManagerLog) << "_onGstInitComplete: unexpected state" << static_cast<int>(_initState);
        return;
    }
}

void VideoManager::_createVideoReceivers()
{
#ifdef QGC_UNITTEST_BUILD
    if (_createVideoReceiversForTest) {
        _createVideoReceiversForTest();
        return;
    }
#endif
    static const QStringList videoStreamList = {
        "videoContent",
        "thermalVideo",
        "pipCamera1Video",
        "pipCamera2Video",
        "pipCamera3Video",
    };
    for (const QString &streamName : videoStreamList) {
        VideoReceiver *receiver = QGCCorePlugin::instance()->createVideoReceiver(this);
        if (!receiver) {
            continue;
        }
        receiver->setName(streamName);

        _initVideoReceiver(receiver, _mainWindow);
    }
}

void VideoManager::cleanup()
{
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        QGCCorePlugin::instance()->releaseVideoSink(receiver->sink());
    }
}

void VideoManager::_cleanupOldVideos()
{
    if (!SettingsManager::instance()->videoSettings()->enableStorageLimit()->rawValue().toBool()) {
        return;
    }

    const QString savePath = SettingsManager::instance()->appSettings()->videoSavePath();
    QDir videoDir = QDir(savePath);
    videoDir.setFilter(QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::Writable);
    videoDir.setSorting(QDir::Time);

    QStringList nameFilters;
    for (size_t i = 0; i < std::size(kFileExtension); i++) {
        nameFilters << QStringLiteral("*.") + kFileExtension[i];
    }

    videoDir.setNameFilters(nameFilters);
    QFileInfoList vidList = videoDir.entryInfoList();
    if (vidList.isEmpty()) {
        return;
    }

    uint64_t total = 0;
    for (const QFileInfo &video : std::as_const(vidList)) {
        total += video.size();
    }

    const uint64_t maxSize = SettingsManager::instance()->videoSettings()->maxVideoSize()->rawValue().toUInt() * qPow(1024, 2);
    while ((total >= maxSize) && !vidList.isEmpty()) {
        const QFileInfo info = vidList.takeLast();
        total -= info.size();
        const QString path = info.filePath();
        qCDebug(VideoManagerLog) << "Removing old video file:" << path;
        (void) QFile::remove(path);
    }
}

void VideoManager::startRecording(const QString &videoFile)
{
    const VideoReceiver::FILE_FORMAT fileFormat = static_cast<VideoReceiver::FILE_FORMAT>(_videoSettings->recordingFormat()->rawValue().toInt());
    if (!VideoReceiver::isValidFileFormat(fileFormat)) {
        QGC::showAppMessage(tr("Invalid video format defined."));
        return;
    }

    _cleanupOldVideos();

    const QString savePath = SettingsManager::instance()->appSettings()->videoSavePath();
    if (savePath.isEmpty()) {
        QGC::showAppMessage(tr("Unabled to record video. Video save path must be specified in Settings."));
        return;
    }

    const QString videoFileUrl = videoFile.isEmpty() ? QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss") : videoFile;
    const QString ext = kFileExtension[fileFormat];

    const QString videoFileNameTemplate = savePath + "/" + videoFileUrl + ".%1" + ext;

    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        if (!receiver->started()) {
            qCDebug(VideoManagerLog) << "Video receiver is not ready.";
            continue;
        }
        const QString streamName = (receiver->name() == QStringLiteral("videoContent")) ? "" : (receiver->name() + ".");
        const QString videoFileName = videoFileNameTemplate.arg(streamName);
        receiver->startRecording(videoFileName, fileFormat);
    }
}

void VideoManager::stopRecording()
{
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        receiver->stopRecording();
    }
}

void VideoManager::grabImage(const QString &imageFile)
{
    if (imageFile.isEmpty()) {
        _imageFile = SettingsManager::instance()->appSettings()->photoSavePath();
        _imageFile += QStringLiteral("/") + QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss.zzz") + QStringLiteral(".jpg");
    } else {
        _imageFile = imageFile;
    }

    emit imageFileChanged(_imageFile);

    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        receiver->takeScreenshot(_imageFile);
        // QSharedPointer<QQuickItemGrabResult> result = receiver->widget()->grabToImage(const QSize &targetSize = QSize())
    }
}

double VideoManager::aspectRatio() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (!receiver->isThermal() && !receiver->isPipCamera() && pInfo && !pInfo->isThermal()) {
            return pInfo->aspectRatio();
        }
    }

    // FIXME: use _videoReceiver->videoSize() to calculate AR (if AR is not specified in the settings?)
    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

double VideoManager::thermalAspectRatio() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (receiver->isThermal() && pInfo && pInfo->isThermal()) {
            return pInfo->aspectRatio();
        }
    }

    return 1.0;
}

double VideoManager::hfov() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (!receiver->isThermal() && !receiver->isPipCamera() && pInfo && !pInfo->isThermal()) {
            return pInfo->hfov();
        }
    }

    return 1.0;
}

double VideoManager::thermalHfov() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (receiver->isThermal() && pInfo && pInfo->isThermal()) {
            return pInfo->hfov();
        }
    }

    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

bool VideoManager::hasThermal() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (receiver->isThermal() && pInfo && pInfo->isThermal()) {
            return true;
        }
    }

    return false;
}

bool VideoManager::hasVideo() const
{
    const bool primaryConfigured = (_videoSettings->streamEnabled()->rawValue().toBool() && _videoSettings->streamConfigured());
    const bool secondaryConfigured = (_video2Settings->streamEnabled()->rawValue().toBool() && _video2Settings->streamConfigured());
    return primaryConfigured || secondaryConfigured;
}

bool VideoManager::isUvc() const
{
    return (!_uvcVideoSourceID.isEmpty() && uvcEnabled() && hasVideo());
}

bool VideoManager::pipCamera1Decoding() const
{
    return _pipCamera1Decoding;
}

bool VideoManager::pipCamera2Decoding() const
{
    return _pipCamera2Decoding;
}

bool VideoManager::pipCamera3Decoding() const
{
    return _pipCamera3Decoding;
}

bool VideoManager::gstreamerEnabled()
{
#ifdef QGC_GST_STREAMING
    return true;
#else
    return false;
#endif
}

bool VideoManager::uvcEnabled()
{
    return UVCReceiver::enabled();
}

bool VideoManager::qtmultimediaEnabled()
{
    return QtMultimediaReceiver::enabled();
}

void VideoManager::setfullScreen(bool on)
{
    if (on) {
        if (!_activeVehicle || _activeVehicle->vehicleLinkManager()->communicationLost()) {
            on = false;
        }
    }

    if (on != _fullScreen) {
        _fullScreen = on;
        emit fullScreenChanged();
    }
}

bool VideoManager::isStreamSource() const
{
    static const QStringList videoSourceList = {
        VideoSettings::videoSourceUDPH264,
        VideoSettings::videoSourceUDPH265,
        VideoSettings::videoSourceRTSP,
        VideoSettings::videoSourceWHEP,
        VideoSettings::videoSourceTCP,
        VideoSettings::videoSourceMPEGTS,
        VideoSettings::videoSource3DRSolo,
        VideoSettings::videoSourceParrotDiscovery,
        VideoSettings::videoSourceYuneecMantisG,
        VideoSettings::videoSourceHerelinkAirUnit,
        VideoSettings::videoSourceHerelinkHotspot,
    };
    const QString videoSource = _videoSettings->videoSource()->rawValue().toString();
    return (videoSourceList.contains(videoSource) || autoStreamConfigured());
}

void VideoManager::_videoSourceChanged()
{
    QHash<VideoReceiver*, QString> oldUris;
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        oldUris[receiver] = receiver->uri();
    }

    bool changed = false;
    if (_activeVehicle) {
        QGCCameraManager* camMgr = _activeVehicle->cameraManager();
        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            QGCVideoStreamInfo* info = nullptr;
            if (receiver->isThermal()) {
                info = camMgr ? camMgr->thermalStreamInstance() : nullptr;
            } else {
                const int camIdx = _cameraIndexForReceiver(receiver);
                if (camIdx >= 0) {
                    info = camMgr ? camMgr->streamInstanceForCamera(camIdx) : nullptr;
                }
            }
            receiver->setVideoStreamInfo(info);
            changed |= _updateSettings(receiver);
        }
    } else {
        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            receiver->setVideoStreamInfo(nullptr);
            changed |= _updateSettings(receiver);
        }
    }

    if (changed) {
        emit hasVideoChanged();
        emit isStreamSourceChanged();
        emit isAutoStreamChanged();

        if (hasVideo()) {
            for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
                if (receiver->uri() != oldUris.value(receiver)) {
                    _restartVideo(receiver);
                }
            }
        } else {
            stopVideo();
        }

        qCDebug(VideoManagerLog) << "New Video Source:" << _videoSettings->videoSource()->rawValue().toString();
    }
}

bool VideoManager::_updateUVC(VideoReceiver * /*receiver*/)
{
    bool result = false;

    const QString oldUvcVideoSrcID = _uvcVideoSourceID;

    if (!uvcEnabled() || !hasVideo() || isStreamSource()) {
        _uvcVideoSourceID = QString();
    } else {
        _uvcVideoSourceID = UVCReceiver::getSourceId();
    }

    if (oldUvcVideoSrcID != _uvcVideoSourceID) {
        qCDebug(VideoManagerLog) << "UVC changed from [" << oldUvcVideoSrcID << "] to [" << _uvcVideoSourceID << "]";
        if (!_uvcVideoSourceID.isEmpty()) {
            UVCReceiver::checkPermission();
        }
        result = true;
        emit uvcVideoSourceIDChanged();
        emit isUvcChanged();
    }

    return result;
}

bool VideoManager::autoStreamConfigured() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (!receiver->isThermal() && !receiver->isPipCamera() && pInfo && !pInfo->isThermal()) {
            return !pInfo->uri().isEmpty();
        }
    }

    return false;
}

bool VideoManager::_updateAutoStream(VideoReceiver *receiver)
{
    const QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
    if (!pInfo) {
        return false;
    }

    qCDebug(VideoManagerLog) << QString("Configure stream (%1):").arg(receiver->name()) << pInfo->uri();

    QString source, url;
    quint8 streamType = pInfo->type();

    switch (streamType) {
    case VIDEO_STREAM_TYPE_RTSP:
        source = VideoSettings::videoSourceRTSP;
        url = pInfo->uri();
        _videoSettings->rtspUrl()->setRawValue(url);
        break;
    case VIDEO_STREAM_TYPE_TCP_MPEG:
        source = VideoSettings::videoSourceTCP;
        url = pInfo->uri();
        break;
    case VIDEO_STREAM_TYPE_RTPUDP:
        if (pInfo->encoding() == VIDEO_STREAM_ENCODING_H265) {
            source = VideoSettings::videoSourceUDPH265;
            url = pInfo->uri().contains("udp265://") ? pInfo->uri() : QStringLiteral("udp265://0.0.0.0:%1").arg(pInfo->uri());
        } else {
            source = VideoSettings::videoSourceUDPH264;
            url = pInfo->uri().contains("udp://") ? pInfo->uri() : QStringLiteral("udp://0.0.0.0:%1").arg(pInfo->uri());
        }
        break;
    case VIDEO_STREAM_TYPE_MPEG_TS:
        source = VideoSettings::videoSourceMPEGTS;
        url = pInfo->uri().contains("mpegts://") ? pInfo->uri() : QStringLiteral("mpegts://0.0.0.0:%1").arg(pInfo->uri());
        break;
    case VIDEO_STREAM_TYPE_WHEP:
        source = VideoSettings::videoSourceWHEP;
        url = pInfo->uri();
        _videoSettings->whepUrl()->setRawValue(url);
        break;
    default:
        qCWarning(VideoManagerLog) << "Unknown VIDEO_STREAM_TYPE";
        source = VideoSettings::videoSourceNoVideo;
        url = pInfo->uri();
        break;
    }

    const bool settingsChanged = _updateVideoUri(receiver, url);
    if (settingsChanged && !receiver->isPipCamera()) {
        if (!receiver->isThermal()) {
            _videoSettings->videoSource()->setRawValue(source);
        }
        emit autoStreamConfiguredChanged();
    }

    return settingsChanged;
}

bool VideoManager::_updateVideoUri(VideoReceiver *receiver, const QString &uri)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return false;
    }

    if ((uri == receiver->uri()) && !receiver->uri().isNull()) {
        return false;
    }

    qCDebug(VideoManagerLog) << "New Video URI" << uri;

    receiver->setUri(uri);

    return true;
}

bool VideoManager::_updateSettings(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return false;
    }

    bool settingsChanged = false;

    const bool useVideo2Settings = receiver->isPipCamera() && (receiver->name() == QStringLiteral("pipCamera1Video")) && !receiver->videoStreamInfo();
    const bool lowLatency = useVideo2Settings
        ? _video2Settings->lowLatencyMode()->rawValue().toBool()
        : _videoSettings->lowLatencyMode()->rawValue().toBool();
    if (lowLatency != receiver->lowLatency()) {
        receiver->setLowLatency(lowLatency);
        settingsChanged = true;
    }

    if (receiver->isThermal()) {
        return settingsChanged;
    }

    if (receiver->isPipCamera()) {
        // PiP cameras auto-follow MAVLink camera streams when available.
        settingsChanged |= _updateAutoStream(receiver);
        // If the first PiP stream has no MAVLink stream info, use Video2 settings.
        if ((receiver->name() == QStringLiteral("pipCamera1Video")) && !receiver->videoStreamInfo()) {
            settingsChanged |= _updateSettingsFromSource(
                receiver,
                _video2Settings->videoSource()->rawValue().toString(),
                _video2Settings->udpUrl()->rawValue().toString(),
                _video2Settings->rtspUrl()->rawValue().toString(),
                _video2Settings->whepUrl()->rawValue().toString(),
                _video2Settings->tcpUrl()->rawValue().toString());
        }
        return settingsChanged;
    }

    settingsChanged |= _updateUVC(receiver);
    settingsChanged |= _updateAutoStream(receiver);

    settingsChanged |= _updateSettingsFromSource(
        receiver,
        _videoSettings->videoSource()->rawValue().toString(),
        _videoSettings->udpUrl()->rawValue().toString(),
        _videoSettings->rtspUrl()->rawValue().toString(),
        _videoSettings->whepUrl()->rawValue().toString(),
        _videoSettings->tcpUrl()->rawValue().toString());

    return settingsChanged;
}

bool VideoManager::_updateSettingsFromSource(VideoReceiver *receiver, const QString &source, const QString &udpUrl, const QString &rtspUrl, const QString &whepUrl, const QString &tcpUrl)
{
    bool settingsChanged = false;

    if (source == VideoSettings::videoSourceUDPH264) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp://%1").arg(udpUrl));
    } else if (source == VideoSettings::videoSourceUDPH265) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp265://%1").arg(udpUrl));
    } else if (source == VideoSettings::videoSourceMPEGTS) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("mpegts://%1").arg(udpUrl));
    } else if (source == VideoSettings::videoSourceRTSP) {
        settingsChanged |= _updateVideoUri(receiver, rtspUrl);
    } else if (source == VideoSettings::videoSourceWHEP) {
        settingsChanged |= _updateVideoUri(receiver, whepUrl);
    } else if (source == VideoSettings::videoSourceTCP) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("tcp://%1").arg(tcpUrl));
    } else if (source == VideoSettings::videoSource3DRSolo) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp://0.0.0.0:5600"));
    } else if (source == VideoSettings::videoSourceParrotDiscovery) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp://0.0.0.0:8888"));
    } else if (source == VideoSettings::videoSourceYuneecMantisG) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("rtsp://192.168.42.1:554/live"));
    } else if (source == VideoSettings::videoSourceHerelinkAirUnit) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("rtsp://192.168.0.10:8554/H264Video"));
    } else if (source == VideoSettings::videoSourceHerelinkHotspot) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("rtsp://192.168.43.1:8554/fpv_stream"));
    } else if ((source == VideoSettings::videoDisabled) || (source == VideoSettings::videoSourceNoVideo)) {
        settingsChanged |= _updateVideoUri(receiver, QString());
    } else {
        settingsChanged |= _updateVideoUri(receiver, QString());
        if (!receiver->isPipCamera() && !isUvc()) {
            qCCritical(VideoManagerLog) << "Video source URI \"" << source << "\" is not supported. Please add support!";
        }
    }

    return settingsChanged;
}

void VideoManager::_setActiveVehicle(Vehicle *vehicle)
{
    qCDebug(VideoManagerLog) << Q_FUNC_INFO << "new vehicle" << vehicle << "old active vehicle" << _activeVehicle;

    if (_activeVehicle) {
        (void) disconnect(_activeVehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this, &VideoManager::_communicationLostChanged);
        auto cameraManager = _activeVehicle->cameraManager();
        if (cameraManager) {
            MavlinkCameraControlInterface *pCamera = cameraManager->currentCameraInstance();
            if (pCamera) {
                pCamera->stopStream();
            }
            (void) disconnect(cameraManager, &QGCCameraManager::streamChanged, this, &VideoManager::_videoSourceChanged);
        }

        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            // disconnect(receiver->videoStreamInfo(), &QGCVideoStreamInfo::infoChanged, ))
            receiver->setVideoStreamInfo(nullptr);
        }
    }

    _activeVehicle = vehicle;
    if (_activeVehicle) {
        (void) connect(_activeVehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this, &VideoManager::_communicationLostChanged);
        if (_activeVehicle->cameraManager()) {
            (void) connect(_activeVehicle->cameraManager(), &QGCCameraManager::streamChanged, this, &VideoManager::_videoSourceChanged);
            MavlinkCameraControlInterface *pCamera = _activeVehicle->cameraManager()->currentCameraInstance();
            if (pCamera) {
                pCamera->resumeStream();
            }
        }

        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            QGCCameraManager *camMgr = _activeVehicle->cameraManager();
            if (camMgr) {
                if (receiver->isThermal()) {
                    receiver->setVideoStreamInfo(camMgr->thermalStreamInstance());
                } else {
                    const int camIdx = _cameraIndexForReceiver(receiver);
                    receiver->setVideoStreamInfo(camIdx >= 0 ? camMgr->streamInstanceForCamera(camIdx) : nullptr);
                }
            } else {
                receiver->setVideoStreamInfo(nullptr);
            }
        }
    } else {
        setfullScreen(false);
    }
}

void VideoManager::_communicationLostChanged(bool connectionLost)
{
    if (connectionLost) {
        setfullScreen(false);
    }
}

void VideoManager::_restartAllVideos()
{
    for (VideoReceiver *videoReceiver : std::as_const(_videoReceivers)) {
        _restartVideo(videoReceiver);
    }
}

void VideoManager::_restartVideo(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return;
    }

    qCDebug(VideoManagerLog) << "Restart video receiver" << receiver->name();

    if (receiver->started()) {
        _stopReceiver(receiver);
        // onStopComplete Signal Will Restart It
    } else {
        _startReceiver(receiver);
    }
}

void VideoManager::_stopReceiver(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return;
    }

    if (receiver->started()) {
        receiver->stop();
    }
}

void VideoManager::stopVideo()
{
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        _stopReceiver(receiver);
    }
}

void VideoManager::_startReceiver(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return;
    }

    if (receiver->started()) {
        qCDebug(VideoManagerLog) << "VideoReceiver is already started" << receiver->name();
        return;
    }

    if (receiver->uri().isEmpty()) {
        qCDebug(VideoManagerLog) << "VideoUri is NULL" << receiver->name();
        return;
    }

    const QString source = _videoSettings->videoSource()->rawValue().toString();
    /* The gstreamer rtsp source will switch to tcp if udp is not available after 5 seconds.
       So we should allow for some negotiation time for rtsp */

    const uint32_t timeout = ((source == VideoSettings::videoSourceRTSP) ? _videoSettings->rtspTimeout()->rawValue().toUInt() : 3);

    receiver->start(timeout);
}

void VideoManager::_initVideoReceiver(VideoReceiver *receiver, QQuickWindow *window)
{
    if (_videoReceivers.contains(receiver)) {
        qCWarning(VideoManagerLog) << "Receiver already initialized";
    }

    QQuickItem *widget = window->findChild<QQuickItem*>(receiver->name());
    if (!widget) {
        qCCritical(VideoManagerLog) << "stream widget not found" << receiver->name();
    }
    receiver->setWidget(widget);

    void *sink = QGCCorePlugin::instance()->createVideoSink(receiver->widget(), receiver);
    if (!sink) {
        qCCritical(VideoManagerLog) << "createVideoSink() failed" << receiver->name();
    }
    receiver->setSink(sink);

#ifdef QGC_GST_STREAMING
    if (sink && widget) {
        auto *videoOutput = qobject_cast<QQuickVideoOutput *>(widget);
        if (videoOutput) {
            QVideoSink *videoSink = videoOutput->videoSink();
            if (!GStreamer::setupAppSinkAdapter(sink, videoSink, receiver)) {
                qCWarning(VideoManagerLog) << "setupAppSinkAdapter failed" << receiver->name();
            }
            // Visibility gate: drop frames at the appsink while the host window is hidden
            // or minimized. The decoder still runs (cheap with HW accel) but render-thread
            // and copy work disappears. Connector handles late window attachment via
            // QQuickItem::windowChanged.
            auto applyVisibility = [receiver](QWindow *win) {
                if (!win) return;
                const QWindow::Visibility v = win->visibility();
                const bool active = (v != QWindow::Hidden && v != QWindow::Minimized);
                GStreamer::setAppSinkAdaptersActive(receiver, active);
            };
            // Track the previous connection so windowChanged can drop it before wiring the
            // new window. Without this, an old hidden/minimized window keeps gating the
            // live receiver after the video output reparents to a new window.
            auto prevConn = std::make_shared<QMetaObject::Connection>();
            auto wireWindow = [receiver, applyVisibility, prevConn](QQuickWindow *qw) {
                if (*prevConn) {
                    QObject::disconnect(*prevConn);
                    *prevConn = QMetaObject::Connection{};
                }
                if (!qw) return;
                applyVisibility(qw);
                *prevConn = QObject::connect(qw, &QWindow::visibilityChanged, receiver,
                    [applyVisibility, qw](QWindow::Visibility) { applyVisibility(qw); });
            };
            if (QQuickWindow *qw = videoOutput->window()) wireWindow(qw);
            QObject::connect(videoOutput, &QQuickVideoOutput::windowChanged, receiver, wireWindow);
        } else {
            qCWarning(VideoManagerLog) << "Widget is not a VideoOutput, cannot connect appsink" << receiver->name();
        }
    }
#endif

    (void) connect(receiver, &VideoReceiver::onStartComplete, this, [this, receiver](VideoReceiver::STATUS status) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "Start complete, status:" << status;
        switch (status) {
        case VideoReceiver::STATUS_OK:
            receiver->setStarted(true);
            if (receiver->sink()) {
                receiver->startDecoding(receiver->sink());
            }
            break;
        case VideoReceiver::STATUS_INVALID_URL:
        case VideoReceiver::STATUS_INVALID_STATE:
            break;
        default:
            _restartVideo(receiver);
            break;
        }
    });

    (void) connect(receiver, &VideoReceiver::onStopComplete, this, [this, receiver](VideoReceiver::STATUS status) {
        qCDebug(VideoManagerLog) << "Stop complete" << receiver->name() << receiver->uri()  << ", status:" << status;
        receiver->setStarted(false);
        if (status == VideoReceiver::STATUS_INVALID_URL) {
            qCDebug(VideoManagerLog) << "Invalid video URL. Not restarting";
        } else {
            QTimer::singleShot(1000, receiver, [this, receiver]() {
                qCDebug(VideoManagerLog) << "Restarting video receiver" << receiver->name() << receiver->uri();
                _startReceiver(receiver);
            });
        }
    });

    (void) connect(receiver, &VideoReceiver::streamingChanged, this, [this, receiver](bool active) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "streaming changed, active:" << (active ? "yes" : "no");
        if (!receiver->isThermal() && !receiver->isPipCamera()) {
            _streaming = active;
            emit streamingChanged();
        }
    });

    (void) connect(receiver, &VideoReceiver::decodingChanged, this, [this, receiver](bool active) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "decoding changed, active:" << (active ? "yes" : "no");
        if (!receiver->isThermal() && !receiver->isPipCamera()) {
            _decoding = active;
            emit decodingChanged();
        } else if (receiver->name() == QStringLiteral("pipCamera1Video")) {
            _pipCamera1Decoding = active;
            emit pipCamera1DecodingChanged();
        } else if (receiver->name() == QStringLiteral("pipCamera2Video")) {
            _pipCamera2Decoding = active;
            emit pipCamera2DecodingChanged();
        } else if (receiver->name() == QStringLiteral("pipCamera3Video")) {
            _pipCamera3Decoding = active;
            emit pipCamera3DecodingChanged();
        }
    });

    (void) connect(receiver, &VideoReceiver::recordingChanged, this, [this, receiver](bool active) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "recording changed, active:" << (active ? "yes" : "no");
        if (!receiver->isThermal() && !receiver->isPipCamera()) {
            _recording = active;
            if (!active) {
                _subtitleWriter->stopCapturingTelemetry();
            }
            emit recordingChanged(_recording);
        }
    });

    (void) connect(receiver, &VideoReceiver::recordingStarted, this, [this, receiver](const QString &filename) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "recording started";
        if (!receiver->isThermal() && !receiver->isPipCamera()) {
            _subtitleWriter->startCapturingTelemetry(filename, videoSize());
        }
    });

    (void) connect(receiver, &VideoReceiver::videoSizeChanged, this, [this, receiver](QSize size) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "resized. New resolution:" << size.width() << "x" << size.height();
        if (!receiver->isThermal() && !receiver->isPipCamera()) {
            _videoSize = size;
            emit videoSizeChanged();
        }
    });

    (void) connect(receiver, &VideoReceiver::onTakeScreenshotComplete, this, [receiver](VideoReceiver::STATUS status) {
        if (status == VideoReceiver::STATUS_OK) {
            qCDebug(VideoManagerLog) << "Video" << receiver->name() << "screenshot taken";
        } else {
            qCWarning(VideoManagerLog) << "Video" << receiver->name() << "screenshot failed";
        }
    });

    (void) connect(receiver, &VideoReceiver::videoStreamInfoChanged, this, [this, receiver]() {
        const QGCVideoStreamInfo *videoStreamInfo = receiver->videoStreamInfo();
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "stream info:" << (videoStreamInfo ? "received" : "lost");

        // Pip cameras have their URI updated via _updateSettings so the change
        // is captured in the `changed` flag and triggers _restartAllVideos.
        if (!receiver->isPipCamera()) {
            (void) _updateAutoStream(receiver);
        }
    });

    (void) _updateSettings(receiver);

    _videoReceivers.append(receiver);

    if (hasVideo()) {
        _startReceiver(receiver);
    }
}

void VideoManager::startVideo()
{
    qCDebug(VideoManagerLog) << "startVideo";

    if (!hasVideo()) {
        qCDebug(VideoManagerLog) << "Stream not enabled/configured";
        return;
    }

    _restartAllVideos();
}
