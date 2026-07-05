pragma ComponentBehavior: Bound
import QtQuick

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlyView

// Owns all 4 camera video items. Each receiver is permanently bound to camera[N]
// so setCurrentCamera() causes zero stream restarts — only x/y/width/height change.
Item {
    id: _root

    property var  pipView:  null          // PipView (map/video swap widget)
    property Item pipState: _pipState     // Exposed so PipView can manage full/pip state

    property var  _camMgr:   QGroundControl.multiVehicleManager.activeVehicle?.cameraManager ?? null
    property int  _curCam:   _camMgr ? _camMgr.currentCamera : 0
    property int  _camCount: _camMgr ? _camMgr.cameras.count : 0
    property bool _manualVideo2Enabled: QGroundControl.settingsManager.video2Settings.streamConfigured
    property bool _manualVideo2PrimaryIsPip1: false
    property var  _curCamObj: (_camMgr && _camCount > 0) ? _camMgr.cameras.get(_curCam) : null

    // Match the map/video PipView widget size so the pip column looks uniform.
    property real _pipSize:  pipView ? pipView._pipSize : parent.width * 0.2
    property real _pipH:     _pipSize * (9 / 16)
    property real _margin:   ScreenTools.defaultFontPixelWidth * 0.75
    property real _spacing:  ScreenTools.defaultFontPixelHeight * 0.5

    // Vertical clearance from the bottom of this item to clear the PipView widget.
    property real _pipBase:  (pipView && pipView.visible)
                              ? (2 * _margin + pipView.height)
                              : _margin

    // True when this item is inside the PipView small box (map is the full view).
    property bool _inPip:    _pipState.state === _pipState.pipState

    // PipState: drives this item between fullscreen and the PipView small box.
    PipState {
        id:      _pipState
        pipView: _root.pipView
        isDark:  true

        onWindowAboutToOpen:  { QGroundControl.videoManager.stopVideo(); _restartTimer.start() }
        onWindowAboutToClose: { QGroundControl.videoManager.stopVideo(); _restartTimer.start() }
        onStateChanged: {
            if (_pipState.state !== _pipState.fullState) {
                QGroundControl.videoManager.fullScreen = false
            }
        }
    }

    Timer {
        id:         _restartTimer
        interval:   2000
        onTriggered: QGroundControl.videoManager.startVideo()
    }

    function promoteManualCamera(camIdx) {
        if (!_manualVideo2Enabled || _camCount > 1) {
            return
        }
        if (camIdx === 0 || camIdx === 1) {
            _manualVideo2PrimaryIsPip1 = (camIdx === 1)
            QGroundControl.videoManager.setPrimaryAudioReceiver(camIdx === 1 ? "pipCamera1Video" : "videoContent")
        }
    }

    on_ManualVideo2EnabledChanged: {
        if (!_manualVideo2Enabled) {
            QGroundControl.videoManager.clearPrimaryAudioReceiver()
        }
    }

    // Returns the pip-column slot index (0 = bottommost) for a non-current camera.
    function _slot(camIdx) {
        if (_camCount <= 1 && _manualVideo2Enabled) {
            if (camIdx === (_manualVideo2PrimaryIsPip1 ? 0 : 1)) {
                return 0
            }
            return -1
        }
        var s = 0
        for (var i = 0; i < _camCount; i++) {
            if (i === _curCam) continue
            if (i === camIdx) return s
            s++
        }
        return -1
    }

    Component { id: _videoOutputComp; FlightDisplayViewVideoOutput { } }

    // ── One item per camera ───────────────────────────────────────────────────
    component CameraItem: Item {
        id: _ci

        required property int    camIdx
        required property string vidName

        property bool _manualMode: _root._camCount <= 1 && _root._manualVideo2Enabled
        property bool _cur:      _manualMode
                                 ? (_root._manualVideo2PrimaryIsPip1 ? (_ci.camIdx === 1) : (_ci.camIdx === 0))
                                 : (_root._curCam === _ci.camIdx)
        property bool _exists:   _ci.camIdx === 0 || _ci.camIdx < _root._camCount || (_ci.camIdx === 1 && _root._manualVideo2Enabled)
        property int  _slt:      _root._slot(_ci.camIdx)
        property var  _cam:      (_ci._exists && _root._camMgr)
                                  ? _root._camMgr.cameras.get(_ci.camIdx) : null
        property bool _decoding: _ci.camIdx === 0 ? QGroundControl.videoManager.decoding
                                 : _ci.camIdx === 1 ? QGroundControl.videoManager.pipCamera1Decoding
                                 : _ci.camIdx === 2 ? QGroundControl.videoManager.pipCamera2Decoding
                                 : _ci.camIdx === 3 ? QGroundControl.videoManager.pipCamera3Decoding
                                 : false

        // Full-screen when current; pip-slot position otherwise.
        // When in pip, position non-current cameras ABOVE using negative y, x=0 (PipView has margins)
        x:      _ci._cur ? 0 : (_root._inPip ? 0 : _root._margin)
        y:      _ci._cur ? 0
                         : _root._inPip
                           ? (0 - (_ci._slt + 1) * (_root._pipH + _root._spacing))
                           : (_root.height - _root._pipBase
                              - (_ci._slt + 1) * _root._pipH
                              - _ci._slt * _root._spacing)
        width:  _ci._cur ? _root.width  : _root._pipSize
        height: _ci._cur ? _root.height : _root._pipH

        // Always visible so video renders (FlyViewPipCameras provides clickable overlay)
        visible: _ci._exists && (_ci._cur || _ci._slt >= 0)

        // Pip items sit above the fullscreen item within this parent.
        z: _ci._cur ? 0 : 1

        clip: true

        // Black background - only when video is decoding
        Rectangle {
            anchors.fill: parent
            color:        "black"
            visible:      _ci._decoding
            z:            0
        }

        // Overlay background - only when NOT decoding
        Rectangle {
            anchors.fill: parent
            color:        "#1a1a1a"
            visible:      !_ci._decoding && !QGroundControl.videoManager.isUvc
            z:            0

            // Background image
            Image {
                anchors.fill: parent
                source:       "/res/NoVideoBackground.jpg"
                fillMode:     Image.PreserveAspectCrop
                z:            1
            }

            // "WAITING FOR VIDEO" label background
            Rectangle {
                anchors.centerIn: parent
                width:            _noVidLabel.contentWidth + ScreenTools.defaultFontPixelHeight
                height:           _noVidLabel.contentHeight + ScreenTools.defaultFontPixelHeight
                radius:           ScreenTools.defaultFontPixelWidth / 2
                color:            "black"
                opacity:          0.5
                z:                2
            }

            // "WAITING FOR VIDEO" label
            QGCLabel {
                id:               _noVidLabel
                anchors.centerIn: parent
                text:             QGroundControl.settingsManager.videoSettings.streamEnabled.rawValue
                                   ? qsTr("WAITING FOR VIDEO") : qsTr("VIDEO DISABLED")
                font.bold:        true
                color:            "white"
                font.pointSize:   _ci._cur ? ScreenTools.largeFontPointSize
                                            : ScreenTools.smallFontPointSize
                z:                3
            }
        }

        // Video surface - only visible when decoding
        Loader {
            anchors.fill:    parent
            sourceComponent: _videoOutputComp
            onLoaded:        { if (item) item.objectName = _ci.vidName }
            visible:         _ci._decoding  // Hide video when not decoding
            z:               10
        }

        // Click a pip item to promote it to current camera (no stream restarts).
        MouseArea {
            anchors.fill: parent
            enabled:      !_ci._cur
            z:            100  // On top of everything to ensure clicks work
            onClicked: {
                if (_ci._manualMode) {
                    _root.promoteManualCamera(_ci.camIdx)
                } else if (_root._camMgr) {
                    QGroundControl.videoManager.clearPrimaryAudioReceiver()
                    _root._camMgr.currentCamera = _ci.camIdx
                }
                // Swap to video fullscreen if map is currently fullscreen
                if (_root.pipView && _root.pipView.item1 &&
                        _root.pipView.item1.pipState.state === _root.pipView.item1.pipState.fullState) {
                    _root.pipView._swapPip()
                }
            }
        }
    }

    // Camera[0] → "videoContent", Camera[1] → "pipCamera1Video", etc.
    // Receiver names are fixed; VideoManager never reroutes them on camera switch.
    CameraItem { camIdx: 0; vidName: "videoContent"    }
    CameraItem { camIdx: 1; vidName: "pipCamera1Video" }
    CameraItem { camIdx: 2; vidName: "pipCamera2Video" }
    CameraItem { camIdx: 3; vidName: "pipCamera3Video" }

    // ── Overlays (full-view mode) ─────────────────────────────────────────────
    OnScreenGimbalController {
        id:                    _gimbal
        anchors.fill:          parent
        cameraTrackingEnabled: !!(_root._curCamObj && _root._curCamObj.trackingEnabled)
    }

    OnScreenCameraTrackingController {
        id:          _tracking
        anchors.fill: parent
        camera:      _root._curCamObj
        videoWidth:  _root.width
        videoHeight: _root.height
    }

    MouseArea {
        anchors.fill: parent
        enabled:      _pipState.state === _pipState.fullState

        property real _px: 0
        property real _py: 0
        property bool _drag: false
        readonly property real _thresh: 10

        onDoubleClicked: QGroundControl.videoManager.fullScreen = !QGroundControl.videoManager.fullScreen

        onPressed: (mouse) => { _px = mouse.x; _py = mouse.y; _drag = false }

        onPositionChanged: (mouse) => {
            if (!_drag && (Math.abs(mouse.x - _px) >= _thresh || Math.abs(mouse.y - _py) >= _thresh)) {
                _drag = true
                _gimbal.mouseDragStart(_px, _py)
                _tracking.mouseDragStart(_px, _py)
            }
            if (_drag) {
                _gimbal.mouseDragPositionChanged(mouse.x, mouse.y)
                _tracking.mouseDragPositionChanged(mouse.x, mouse.y)
            }
        }

        onReleased: (mouse) => {
            if (_drag) {
                _gimbal.mouseDragEnd()
                _tracking.mouseDragEnd(mouse.x, mouse.y)
            } else {
                _gimbal.mouseClicked(mouse.x, mouse.y)
                _tracking.mouseClicked(mouse.x, mouse.y)
            }
            _drag = false
        }
    }

    ProximityRadarVideoView {
        anchors.fill: parent
        vehicle:      QGroundControl.multiVehicleManager.activeVehicle
    }

    ObstacleDistanceOverlayVideo {
        showText: _pipState.state === _pipState.fullState
    }

    QGCLabel {
        text:             qsTr("Double-click to exit full screen")
        font.pointSize:   ScreenTools.largeFontPointSize
        visible:          QGroundControl.videoManager.fullScreen
        anchors.centerIn: parent

        onVisibleChanged: { if (visible) _fsAnim.start() }

        PropertyAnimation on opacity {
            id:          _fsAnim
            duration:    10000
            from:        1.0
            to:          0.0
            easing.type: Easing.InExpo
        }
    }
}
