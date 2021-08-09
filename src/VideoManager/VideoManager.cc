/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QUrl>
#include <QDir>

#ifndef QGC_DISABLE_UVC
#include <QCameraInfo>
#endif

#include "ScreenToolsController.h"
#include "VideoManager.h"
#include "QGCToolbox.h"
#include "QGCCorePlugin.h"
#include "QGCOptions.h"
#include "MultiVehicleManager.h"
#include "Settings/SettingsManager.h"
#include "Vehicle.h"
#include "QGCCameraManager.h"

#if defined(QGC_GST_STREAMING)
#include "GStreamer.h"
#include "VideoSettings.h"
#else
#include "GLVideoItemStub.h"
#endif

#ifdef QGC_GST_TAISYNC_ENABLED
#include "TaisyncHandler.h"
#endif

QGC_LOGGING_CATEGORY(VideoManagerLog, "VideoManagerLog")

#if defined(QGC_GST_STREAMING)
static const char* kFileExtension[VideoReceiver::FILE_FORMAT_MAX - VideoReceiver::FILE_FORMAT_MIN] = {
    "mkv",
    "mov",
    "mp4"
};
#endif

//-----------------------------------------------------------------------------
VideoManager::VideoManager(QGCApplication* app, QGCToolbox* toolbox)
    : QGCTool(app, toolbox)
{
#if !defined(QGC_GST_STREAMING)
    static bool once = false;
    if (!once) {
        qmlRegisterType<GLVideoItemStub>("org.freedesktop.gstreamer.GLVideoItem", 1, 0, "GstGLVideoItem");
        once = true;
    }
#endif
}

//-----------------------------------------------------------------------------
VideoManager::~VideoManager()
{
    /* 2 -> 5 for loop */
    for (int i = 0; i < 5; i++) {
        if (_videoReceiver[i] != nullptr) {
            delete _videoReceiver[i];
            _videoReceiver[i] = nullptr;
        }
#if defined(QGC_GST_STREAMING)
        if (_videoSink[i] != nullptr) {
            // FIXME: AV: we need some interaface for video sink with .release() call
            // Currently VideoManager is destroyed after corePlugin() and we are crashing on app exit
            // calling qgcApp()->toolbox()->corePlugin()->releaseVideoSink(_videoSink[i]);
            // As for now let's call GStreamer::releaseVideoSink() directly
            GStreamer::releaseVideoSink(_videoSink[i]);
            _videoSink[i] = nullptr;
        }
#endif
    }
}

//-----------------------------------------------------------------------------
void
VideoManager::setToolbox(QGCToolbox *toolbox)
{
   QGCTool::setToolbox(toolbox);
   QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
   qmlRegisterUncreatableType<VideoManager> ("QGroundControl.VideoManager", 1, 0, "VideoManager", "Reference only");
   qmlRegisterUncreatableType<VideoReceiver>("QGroundControl",              1, 0, "VideoReceiver","Reference only");

   // TODO: Those connections should be Per Video, not per VideoManager.
   _videoSettings = toolbox->settingsManager()->videoSettings();
   QString videoSource = _videoSettings->videoSource()->rawValue().toString();
   connect(_videoSettings->videoSource(),   &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
   connect(_videoSettings->udpPort(),       &Fact::rawValueChanged, this, &VideoManager::_udpPortChanged);
   connect(_videoSettings->rtspUrl(),       &Fact::rawValueChanged, this, &VideoManager::_rtspUrlChanged);
   connect(_videoSettings->tcpUrl(),        &Fact::rawValueChanged, this, &VideoManager::_tcpUrlChanged);
   connect(_videoSettings->aspectRatio(),   &Fact::rawValueChanged, this, &VideoManager::_aspectRatioChanged);
   connect(_videoSettings->lowLatencyMode(),&Fact::rawValueChanged, this, &VideoManager::_lowLatencyModeChanged);
   MultiVehicleManager *pVehicleMgr = qgcApp()->toolbox()->multiVehicleManager();
   connect(pVehicleMgr, &MultiVehicleManager::activeVehicleChanged, this, &VideoManager::_setActiveVehicle);

#if defined(QGC_GST_STREAMING)
    GStreamer::blacklist(static_cast<VideoSettings::VideoDecoderOptions>(_videoSettings->forceVideoDecoder()->rawValue().toInt()));
#ifndef QGC_DISABLE_UVC
   // If we are using a UVC camera setup the device name
   _updateUVC();
#endif

    emit isGStreamerChanged();
    qCDebug(VideoManagerLog) << "New Video Source:" << videoSource;
#if defined(QGC_GST_STREAMING)

    /* included a for loop */
    for (int i = 0; i < 5; i++) {
        _videoReceiver[i] = toolbox->corePlugin()->createVideoReceiver(this);
    }

    for (int i = 0; i < 4; i++) {

        connect(_videoReceiver[i], &VideoReceiver::streamingChanged, this, [this](bool active){
            _streaming[i] = active;
            emit streamingChanged();
        });

        connect(_videoReceiver[i], &VideoReceiver::onStartComplete, this, [this](VideoReceiver::STATUS status) {
            if (status == VideoReceiver::STATUS_OK) {
                _videoStarted[i] = true;
                if (_videoSink[i] != nullptr) {
                    // It is absolytely ok to have video receiver active (streaming) and decoding not active
                    // It should be handy for cases when you have many streams and want to show only some of them
                    // NOTE that even if decoder did not start it is still possible to record video
                    _videoReceiver[i]->startDecoding(_videoSink[i]);
                }
            } else if (status == VideoReceiver::STATUS_INVALID_URL) {
                // Invalid URL - don't restart
            } else if (status == VideoReceiver::STATUS_INVALID_STATE) {
                // Already running
            } else {
                _restartVideo(i);
            }
        });

        connect(_videoReceiver[i], &VideoReceiver::onStopComplete, this, [this](VideoReceiver::STATUS) {
            _videoStarted[i] = false;
            _startReceiver(i);
        });

        connect(_videoReceiver[i], &VideoReceiver::decodingChanged, this, [this](bool active){
            _decoding[i] = active;
            emit decodingChanged();
        });

        connect(_videoReceiver[i], &VideoReceiver::recordingChanged, this, [this](bool active){
            _recording[i] = active;
            if (!active) {
                _subtitleWriter.stopCapturingTelemetry();
            }
            emit recordingChanged();
        });

        connect(_videoReceiver[i], &VideoReceiver::recordingStarted, this, [this](){
            _subtitleWriter.startCapturingTelemetry(_videoFile);
        });

        connect(_videoReceiver[i], &VideoReceiver::videoSizeChanged, this, [this](QSize size){
            _videoSize[i] = ((quint32)size.width() << 16) | (quint32)size.height();
            emit videoSizeChanged();
        });
    }

   

    //connect(_videoReceiver, &VideoReceiver::onTakeScreenshotComplete, this, [this](VideoReceiver::STATUS status){
    //    if (status == VideoReceiver::STATUS_OK) {
    //    }
    //});

    // FIXME: AV: I believe _thermalVideoReceiver should be handled just like _videoReceiver in terms of event
    // and I expect that it will be changed during multiple video stream activity
    if (_videoReceiver[4] != nullptr) {
        connect(_videoReceiver[4], &VideoReceiver::onStartComplete, this, [this](VideoReceiver::STATUS status) {
            if (status == VideoReceiver::STATUS_OK) {
                _videoStarted[4] = true;
                if (_videoSink[4] != nullptr) {
                    _videoReceiver[4]->startDecoding(_videoSink[4]);
                }
            } else if (status == VideoReceiver::STATUS_INVALID_URL) {
                // Invalid URL - don't restart
            } else if (status == VideoReceiver::STATUS_INVALID_STATE) {
                // Already running
            } else {
                _restartVideo(4);
            }
        });

        connect(_videoReceiver[4], &VideoReceiver::onStopComplete, this, [this](VideoReceiver::STATUS) {
            _videoStarted[4] = false;
            _startReceiver(4);
        });
    }
#endif
    for (int i = 0; i < 5; i++) {
        _updateSettings(i);
    }
    
    if(isGStreamer()) {
        startVideo();
    } else {
        stopVideo();
    }

#endif
}

void VideoManager::_cleanupOldVideos()
{
#if defined(QGC_GST_STREAMING)
    //-- Only perform cleanup if storage limit is enabled
    if(!_videoSettings->enableStorageLimit()->rawValue().toBool()) {
        return;
    }
    QString savePath = qgcApp()->toolbox()->settingsManager()->appSettings()->videoSavePath();
    QDir videoDir = QDir(savePath);
    videoDir.setFilter(QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::Writable);
    videoDir.setSorting(QDir::Time);

    QStringList nameFilters;

    for(size_t i = 0; i < sizeof(kFileExtension) / sizeof(kFileExtension[0]); i += 1) {
        nameFilters << QString("*.") + kFileExtension[i];
    }

    videoDir.setNameFilters(nameFilters);
    //-- get the list of videos stored
    QFileInfoList vidList = videoDir.entryInfoList();
    if(!vidList.isEmpty()) {
        uint64_t total   = 0;
        //-- Settings are stored using MB
        uint64_t maxSize = _videoSettings->maxVideoSize()->rawValue().toUInt() * 1024 * 1024;
        //-- Compute total used storage
        for(int i = 0; i < vidList.size(); i++) {
            total += vidList[i].size();
        }
        //-- Remove old movies until max size is satisfied.
        while(total >= maxSize && !vidList.isEmpty()) {
            total -= vidList.last().size();
            qCDebug(VideoManagerLog) << "Removing old video file:" << vidList.last().filePath();
            QFile file (vidList.last().filePath());
            file.remove();
            vidList.removeLast();
        }
    }
#endif
}

//-----------------------------------------------------------------------------
void
VideoManager::startVideo()
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }

    if(!_videoSettings->streamEnabled()->rawValue().toBool() || !_videoSettings->streamConfigured()) {
        qCDebug(VideoManagerLog) << "Stream not enabled/configured";
        return;
    }

    for (int i = 0; i < 5; i++) {
        _startReceiver(i);
    }

}

//-----------------------------------------------------------------------------
void
VideoManager::stopVideo()
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }

    for (int i = 0; i < 5; i++) {
        _stopReceiver(i);
    }
}

void
VideoManager::startRecording(const QString& videoFile)
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }
#if defined(QGC_GST_STREAMING)
    for (int i = 0; i < 4; i++) {
        if (!_videoReceiver[i]) {
            qgcApp()->showAppMessage(tr("Video receiver" + i + " is not ready."));
            return;
        }
    }
    

    const VideoReceiver::FILE_FORMAT fileFormat = static_cast<VideoReceiver::FILE_FORMAT>(_videoSettings->recordingFormat()->rawValue().toInt());

    if(fileFormat < VideoReceiver::FILE_FORMAT_MIN || fileFormat >= VideoReceiver::FILE_FORMAT_MAX) {
        qgcApp()->showAppMessage(tr("Invalid video format defined."));
        return;
    }
    QString ext = kFileExtension[fileFormat - VideoReceiver::FILE_FORMAT_MIN];

    //-- Disk usage maintenance
    _cleanupOldVideos();

    QString savePath = qgcApp()->toolbox()->settingsManager()->appSettings()->videoSavePath();

    if (savePath.isEmpty()) {
        qgcApp()->showAppMessage(tr("Unabled to record video. Video save path must be specified in Settings."));
        return;
    }

    _videoFile = savePath + "/"
            + (videoFile.isEmpty() ? QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss") : videoFile)
            + ".";
    QString videoFile2 = _videoFile + "2." + ext;
    
    QString videoFile3 = _videoFile + "3." + ext;
    
    QString videoFile4 = _videoFile + "4." + ext;
    
    QString videoFile5 = _videoFile + "5." + ext;

    _videoFile += ext;


    if (_videoReceiver[0] && _videoStarted[0]) {
        _videoReceiver[0]->startRecording(_videoFile, fileFormat);
    }
    if (_videoReceiver[1] && _videoStarted[1]) {
        _videoReceiver[1]->startRecording(videoFile2, fileFormat);
    }
     if (_videoReceiver[2] && _videoStarted[2]) {
        _videoReceiver[2]->startRecording(videoFile3, fileFormat);
    }
     if (_videoReceiver[3] && _videoStarted[3]) {
        _videoReceiver[3]->startRecording(videoFile4, fileFormat);
    }
     if (_videoReceiver[4] && _videoStarted[4]) {
        _videoReceiver[4]->startRecording(videoFile5, fileFormat);
    }


#else
    Q_UNUSED(videoFile)
#endif
}

void
VideoManager::stopRecording()
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }
#if defined(QGC_GST_STREAMING)
    /* 2 -> 5*/
    for (int i = 0; i < 5; i++) {
        if (_videoReceiver[i]) {
            _videoReceiver[i]->stopRecording();
        }
    }
#endif
}
//change grabImage to take screenshot of all images
void
VideoManager::grabImage(const QString& imageFile)
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }


#if defined(QGC_GST_STREAMING)

    for (int i = 0; i < 4; i++) {
        if (!_videoReceiver[i]) {
        return;
    }

    if (imageFile.isEmpty()) {
        _imageFile[i] = qgcApp()->toolbox()->settingsManager()->appSettings()->photoSavePath();
        _imageFile[i] += + "/" + QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss.zzz") + i + ".jpg";
    } else {
        _imageFile[i] = imageFile;
    }

    emit imageFileChanged();

    _videoReceiver[i]->takeScreenshot(_imageFile[i]);
    }

#else
    Q_UNUSED(imageFile)
#endif
}

//-----------------------------------------------------------------------------
double VideoManager::aspectRatio()
{
    if(_activeVehicle && _activeVehicle->cameraManager()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->cameraManager()->currentStreamInstance();
        if(pInfo) {
            qCDebug(VideoManagerLog) << "Primary AR: " << pInfo->aspectRatio();
            return pInfo->aspectRatio();
        }
    }
    // FIXME: AV: use _videoReceiver->videoSize() to calculate AR (if AR is not specified in the settings?)
    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

//-----------------------------------------------------------------------------
double VideoManager::thermalAspectRatio()
{
    if(_activeVehicle && _activeVehicle->cameraManager()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->cameraManager()->thermalStreamInstance();
        if(pInfo) {
            qCDebug(VideoManagerLog) << "Thermal AR: " << pInfo->aspectRatio();
            return pInfo->aspectRatio();
        }
    }
    return 1.0;
}

//-----------------------------------------------------------------------------
double VideoManager::hfov()
{
    if(_activeVehicle && _activeVehicle->cameraManager()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->cameraManager()->currentStreamInstance();
        if(pInfo) {
            return pInfo->hfov();
        }
    }
    return 1.0;
}

//-----------------------------------------------------------------------------
double VideoManager::thermalHfov()
{
    if(_activeVehicle && _activeVehicle->cameraManager()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->cameraManager()->thermalStreamInstance();
        if(pInfo) {
            return pInfo->aspectRatio();
        }
    }
    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

//-----------------------------------------------------------------------------
bool
VideoManager::hasThermal()
{
    if(_activeVehicle && _activeVehicle->cameraManager()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->cameraManager()->thermalStreamInstance();
        if(pInfo) {
            return true;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
QString
VideoManager::imageFile()
{
    return _imageFile;
}

//added functions to access each separate image file
QString
VideoManager::imageFile1()
{
    return _imageFile[0];
}

QString
VideoManager::imageFile2()
{
    return _imageFile[1];
}

QString
VideoManager::imageFile3()
{
    return _imageFile[2];
}

QString
VideoManager::imageFile4()
{
    return _imageFile[3];
}

//-----------------------------------------------------------------------------
bool
VideoManager::autoStreamConfigured()
{
#if defined(QGC_GST_STREAMING)
    if(_activeVehicle && _activeVehicle->cameraManager()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->cameraManager()->currentStreamInstance();
        if(pInfo) {
            return !pInfo->uri().isEmpty();
        }
    }
#endif
    return false;
}

//-----------------------------------------------------------------------------
void
VideoManager::_updateUVC()
{
#ifndef QGC_DISABLE_UVC
    QString videoSource = _videoSettings->videoSource()->rawValue().toString();
    QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    for (const QCameraInfo &cameraInfo: cameras) {
        if(cameraInfo.description() == videoSource) {
            _videoSourceID = cameraInfo.deviceName();
            emit videoSourceIDChanged();
            qCDebug(VideoManagerLog) << "Found USB source:" << _videoSourceID << " Name:" << videoSource;
            break;
        }
    }
#endif
}

//-----------------------------------------------------------------------------
void
VideoManager::_videoSourceChanged()
{
    _updateUVC();
    emit hasVideoChanged();
    emit isGStreamerChanged();
    emit isAutoStreamChanged();

    for (int i =0; i < 4; i++) {
     _restartVideo(i);       
    }

}

//-----------------------------------------------------------------------------
void
VideoManager::_udpPortChanged()
{
    for (int i =0; i < 4; i++) {
     _restartVideo(i);       
    }
}

//-----------------------------------------------------------------------------
void
VideoManager::_rtspUrlChanged()
{
    for (int i =0; i < 4; i++) {
     _restartVideo(i);       
    }
}

//-----------------------------------------------------------------------------
void
VideoManager::_tcpUrlChanged()
{
    for (int i =0; i < 4; i++) {
     _restartVideo(i);       
    }
}

//-----------------------------------------------------------------------------
void
VideoManager::_lowLatencyModeChanged()
{
    _restartAllVideos();
}

//-----------------------------------------------------------------------------
bool
VideoManager::hasVideo()
{
    if(autoStreamConfigured()) {
        return true;
    }
    QString videoSource = _videoSettings->videoSource()->rawValue().toString();
    return !videoSource.isEmpty() && videoSource != VideoSettings::videoSourceNoVideo && videoSource != VideoSettings::videoDisabled;
}

//-----------------------------------------------------------------------------
bool
VideoManager::isGStreamer()
{
#if defined(QGC_GST_STREAMING)
    QString videoSource = _videoSettings->videoSource()->rawValue().toString();
    return videoSource == VideoSettings::videoSourceUDPH264 ||
            videoSource == VideoSettings::videoSourceUDPH265 ||
            videoSource == VideoSettings::videoSourceRTSP ||
            videoSource == VideoSettings::videoSourceTCP ||
            videoSource == VideoSettings::videoSourceMPEGTS ||
            videoSource == VideoSettings::videoSource3DRSolo ||
            videoSource == VideoSettings::videoSourceParrotDiscovery ||
            autoStreamConfigured();
#else
    return false;
#endif
}

//-----------------------------------------------------------------------------
#ifndef QGC_DISABLE_UVC
bool
VideoManager::uvcEnabled()
{
    return QCameraInfo::availableCameras().count() > 0;
}
#endif

//-----------------------------------------------------------------------------
void
VideoManager::setfullScreen(bool f)
{
    if(f) {
        //-- No can do if no vehicle or connection lost
        if(!_activeVehicle || _activeVehicle->vehicleLinkManager()->communicationLost()) {
            f = false;
        }
    }
    _fullScreen = f;
    emit fullScreenChanged();
}

//-----------------------------------------------------------------------------
void
VideoManager::_initVideo()
{
#if defined(QGC_GST_STREAMING)
    QQuickItem* root = qgcApp()->mainRootWindow();

    if (root == nullptr) {
        qCDebug(VideoManagerLog) << "mainRootWindow() failed. No root window";
        return;
    }

    QQuickItem* widget = root->findChild<QQuickItem*>("videoContent");

    for (int i = 0; i < 4; i++){

        if (widget != nullptr && _videoReceiver[i] != nullptr) {
        _videoSink[i] = qgcApp()->toolbox()->corePlugin()->createVideoSink(this, widget);
        if (_videoSink[i] != nullptr) {
            if (_videoStarted[i]) {
                _videoReceiver[i]->startDecoding(_videoSink[i]);
            }
        } else {
            qCDebug(VideoManagerLog) << "createVideoSink() failed";
        }
    } else {
        qCDebug(VideoManagerLog) << "video receiver disabled for camera";
        qCDebug(VideoManagerLog) << i;
    }
        
    }



    widget = root->findChild<QQuickItem*>("thermalVideo");

    if (widget != nullptr && _videoReceiver[4] != nullptr) {
        _videoSink[4] = qgcApp()->toolbox()->corePlugin()->createVideoSink(this, widget);
        if (_videoSink[4] != nullptr) {
            if (_videoStarted[4]) {
                _videoReceiver[4]->startDecoding(_videoSink[4]);
            }
        } else {
            qCDebug(VideoManagerLog) << "createVideoSink() failed";
        }
    } else {
        qCDebug(VideoManagerLog) << "thermal video receiver disabled";
    }
#endif
}

//-----------------------------------------------------------------------------
bool
VideoManager::_updateSettings(unsigned id)
{
    if(!_videoSettings)
        return false;

    const bool lowLatencyStreaming  =_videoSettings->lowLatencyMode()->rawValue().toBool();

    bool settingsChanged = _lowLatencyStreaming[id] != lowLatencyStreaming;

    _lowLatencyStreaming[id] = lowLatencyStreaming;

    //-- Auto discovery

    if(_activeVehicle && _activeVehicle->cameraManager()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->cameraManager()->currentStreamInstance();
        if(pInfo) {
            if ( 0 <= id <=3 ) {
                qCDebug(VideoManagerLog) << "Configure primary stream:" << pInfo->uri();
                switch(pInfo->type()) {
                    case VIDEO_STREAM_TYPE_RTSP:
                        if ((settingsChanged |= _updateVideoUri(id, pInfo->uri()))) {
                            _toolbox->settingsManager()->videoSettings()->videoSource()->setRawValue(VideoSettings::videoSourceRTSP);
                        }
                        break;
                    case VIDEO_STREAM_TYPE_TCP_MPEG:
                        if ((settingsChanged |= _updateVideoUri(id, pInfo->uri()))) {
                            _toolbox->settingsManager()->videoSettings()->videoSource()->setRawValue(VideoSettings::videoSourceTCP);
                        }
                        break;
                    case VIDEO_STREAM_TYPE_RTPUDP:
                        if ((settingsChanged |= _updateVideoUri(id, QStringLiteral("udp://0.0.0.0:%1").arg(pInfo->uri())))) {
                            _toolbox->settingsManager()->videoSettings()->videoSource()->setRawValue(VideoSettings::videoSourceUDPH264);
                        }
                        break;
                    case VIDEO_STREAM_TYPE_MPEG_TS_H264:
                        if ((settingsChanged |= _updateVideoUri(id, QStringLiteral("mpegts://0.0.0.0:%1").arg(pInfo->uri())))) {
                            _toolbox->settingsManager()->videoSettings()->videoSource()->setRawValue(VideoSettings::videoSourceMPEGTS);
                        }
                        break;
                    default:
                        settingsChanged |= _updateVideoUri(id, pInfo->uri());
                        break;
                }
            }
            else if (id == 4) { //-- Thermal stream (if any)
                QGCVideoStreamInfo* pTinfo = _activeVehicle->cameraManager()->thermalStreamInstance();
                if (pTinfo) {
                    qCDebug(VideoManagerLog) << "Configure secondary stream:" << pTinfo->uri();
                    switch(pTinfo->type()) {
                        case VIDEO_STREAM_TYPE_RTSP:
                        case VIDEO_STREAM_TYPE_TCP_MPEG:
                            settingsChanged |= _updateVideoUri(id, pTinfo->uri());
                            break;
                        case VIDEO_STREAM_TYPE_RTPUDP:
                            settingsChanged |= _updateVideoUri(id, QStringLiteral("udp://0.0.0.0:%1").arg(pTinfo->uri()));
                            break;
                        case VIDEO_STREAM_TYPE_MPEG_TS_H264:
                            settingsChanged |= _updateVideoUri(id, QStringLiteral("mpegts://0.0.0.0:%1").arg(pTinfo->uri()));
                            break;
                        default:
                            settingsChanged |= _updateVideoUri(id, pTinfo->uri());
                            break;
                    }
                }
            }
            return settingsChanged;
        }
    }
    QString source = _videoSettings->videoSource()->rawValue().toString();

    for (int i = 0; i < 4; i++){

    if (source == VideoSettings::videoSourceUDPH264)
        settingsChanged |= _updateVideoUri(i, QStringLiteral("udp://0.0.0.0:%1").arg(_videoSettings->udpPort()->rawValue().toInt()));
    else if (source == VideoSettings::videoSourceUDPH265)
        settingsChanged |= _updateVideoUri(i, QStringLiteral("udp265://0.0.0.0:%1").arg(_videoSettings->udpPort()->rawValue().toInt()));
    else if (source == VideoSettings::videoSourceMPEGTS)
        settingsChanged |= _updateVideoUri(i, QStringLiteral("mpegts://0.0.0.0:%1").arg(_videoSettings->udpPort()->rawValue().toInt()));
    else if (source == VideoSettings::videoSourceRTSP)
        settingsChanged |= _updateVideoUri(i, _videoSettings->rtspUrl()->rawValue().toString());
    else if (source == VideoSettings::videoSourceTCP)
        settingsChanged |= _updateVideoUri(i, QStringLiteral("tcp://%1").arg(_videoSettings->tcpUrl()->rawValue().toString()));
    else if (source == VideoSettings::videoSource3DRSolo)
        settingsChanged |= _updateVideoUri(i, QStringLiteral("udp://0.0.0.0:5600"));
    else if (source == VideoSettings::videoSourceParrotDiscovery)
        settingsChanged |= _updateVideoUri(i, QStringLiteral("udp://0.0.0.0:8888"));

    }


    return settingsChanged;
}

//-----------------------------------------------------------------------------
bool
VideoManager::_updateVideoUri(unsigned id, const QString& uri)
{
#if defined(QGC_GST_TAISYNC_ENABLED) && (defined(__android__) || defined(__ios__))
    //-- Taisync on iOS or Android sends a raw h.264 stream
    if (isTaisync()) {
        if ( 0 <= id <=3) {
            return _updateVideoUri(id, QString("tsusb://0.0.0.0:%1").arg(TAISYNC_VIDEO_UDP_PORT));
        } if (id == 4) {
            // FIXME: AV: TAISYNC_VIDEO_UDP_PORT is used by video stream, thermal stream should go via its own proxy
            if (!_videoUri[4].isEmpty()) {
                _videoUri[4].clear();
                return true;
            } else {
                return false;
            }
        }
    }
#endif
    if (uri == _videoUri[id]) {
        return false;
    }

    _videoUri[id] = uri;

    return true;
}

//-----------------------------------------------------------------------------
void
VideoManager::_restartVideo(unsigned id)
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }

#if defined(QGC_GST_STREAMING)
    bool oldLowLatencyStreaming = _lowLatencyStreaming[id];
    QString oldUri = _videoUri[id];
    _updateSettings(id);
    bool newLowLatencyStreaming = _lowLatencyStreaming[id];
    QString newUri = _videoUri[id];

    // FIXME: AV: use _updateSettings() result to check if settings were changed
    if (oldUri == newUri && oldLowLatencyStreaming == newLowLatencyStreaming && _videoStarted[id]) {
        qCDebug(VideoManagerLog) << "No sense to restart video streaming, skipped"  << id;
        return;
    }

    qCDebug(VideoManagerLog) << "Restart video streaming"  << id;

    if (_videoStarted[id]) {
        _stopReceiver(id);
    } else {
        _startReceiver(id);
    }
#endif
}

//-----------------------------------------------------------------------------
void
VideoManager::_restartAllVideos()
{
    for (int i = 0; i < 5; i++){
        _restartVideo(i);
    }


}

//----------------------------------------------------------------------------------------
void
VideoManager::_startReceiver(unsigned id)
{
#if defined(QGC_GST_STREAMING)
    const unsigned timeout = _videoSettings->rtspTimeout()->rawValue().toUInt();

    if (id > 4) {
        qCDebug(VideoManagerLog) << "Unsupported receiver id" << id;
    } else if (_videoReceiver[id] != nullptr/* && _videoSink[id] != nullptr*/) {
        if (!_videoUri[id].isEmpty()) {
            _videoReceiver[id]->start(_videoUri[id], timeout, _lowLatencyStreaming[id] ? -1 : 0);
        }
    }
#endif
}

//----------------------------------------------------------------------------------------
void
VideoManager::_stopReceiver(unsigned id)
{
#if defined(QGC_GST_STREAMING)
    if (id > 4) {
        qCDebug(VideoManagerLog) << "Unsupported receiver id" << id;
    } else if (_videoReceiver[id] != nullptr) {
        _videoReceiver[id]->stop();
    }
#endif
}

//----------------------------------------------------------------------------------------
void
VideoManager::_setActiveVehicle(Vehicle* vehicle)
{
    if(_activeVehicle) {
        disconnect(_activeVehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this, &VideoManager::_communicationLostChanged);
        if(_activeVehicle->cameraManager()) {
            QGCCameraControl* pCamera = _activeVehicle->cameraManager()->currentCameraInstance();
            if(pCamera) {
                pCamera->stopStream();
            }
            disconnect(_activeVehicle->cameraManager(), &QGCCameraManager::streamChanged, this, &VideoManager::_restartAllVideos);
        }
    }
    _activeVehicle = vehicle;
    if(_activeVehicle) {
        connect(_activeVehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this, &VideoManager::_communicationLostChanged);
        if(_activeVehicle->cameraManager()) {
            connect(_activeVehicle->cameraManager(), &QGCCameraManager::streamChanged, this, &VideoManager::_restartAllVideos);
            QGCCameraControl* pCamera = _activeVehicle->cameraManager()->currentCameraInstance();
            if(pCamera) {
                pCamera->resumeStream();
            }
        }
    } else {
        //-- Disable full screen video if vehicle is gone
        setfullScreen(false);
    }
    emit autoStreamConfiguredChanged();
    _restartAllVideos();
}

//----------------------------------------------------------------------------------------
void
VideoManager::_communicationLostChanged(bool connectionLost)
{
    if(connectionLost) {
        //-- Disable full screen video if connection is lost
        setfullScreen(false);
    }
}

//----------------------------------------------------------------------------------------
void
VideoManager::_aspectRatioChanged()
{
    emit aspectRatioChanged();
}
