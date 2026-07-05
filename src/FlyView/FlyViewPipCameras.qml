pragma ComponentBehavior: Bound
import QtQuick

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlyView

// Shows up to 3 additional camera pip widgets stacked above the existing PipView.
// Each live stream is always decoding; clicking promotes that camera to fullscreen.
Column {
    id: _root

    spacing: ScreenTools.defaultFontPixelHeight * 0.5

    // Set by FlyView.qml so we can share size and trigger swap.
    property var  pipView:   null
    property var  videoControl: null

    property var  _camMgr:   QGroundControl.multiVehicleManager.activeVehicle?.cameraManager ?? null
    property int  _curCam:   _camMgr ? _camMgr.currentCamera : 0
    property int  _camCount: _camMgr ? _camMgr.cameras.count : 0
    property bool _manualMode: (_camCount <= 1) && videoControl && videoControl._manualVideo2Enabled
    // Match PipView's size so the resize handle there controls everything.
    property real _pipSize:  pipView ? pipView._pipSize : parent.width * 0.2
    property real _pipHeight: _pipSize * (9 / 16)

    function _pipCamIdx(slot) {
        if (_manualMode) {
            if (slot !== 0) {
                return -1
            }
            return videoControl._manualVideo2PrimaryIsPip1 ? 0 : 1
        }
        var found = 0
        for (var i = 0; i < _camCount; i++) {
            if (i === _curCam) continue
            if (found === slot) return i
            found++
        }
        return -1
    }

    component PipCameraItem: Item {
        id: _item

        required property int    slot
        required property string videoName

        property int _camIdx: _root._pipCamIdx(_item.slot)
        property var _cam:    (_item._camIdx >= 0 && _root._camMgr) ? _root._camMgr.cameras.get(_item._camIdx) : null

        width:   _root._pipSize
        height:  _root._pipHeight
        clip:    true
        visible: _item._camIdx >= 0 && (!_root._manualMode || _item.slot === 0)

        MouseArea {
            anchors.fill: _item
            onClicked: {
                if (_item._camIdx < 0) return
                if (_root._manualMode) {
                    if (_root.videoControl) {
                        _root.videoControl.promoteManualCamera(_item._camIdx)
                    }
                } else {
                    if (!_root._camMgr) return
                    _root._camMgr.currentCamera = _item._camIdx
                }
                // Promote video to fullscreen if the map is currently the full item.
                if (_root.pipView && _root.pipView.item1 &&
                        _root.pipView.item1.pipState.state === _root.pipView.item1.pipState.fullState) {
                    _root.pipView._swapPip()
                }
            }
        }
    }

    PipCameraItem { slot: 2; videoName: "pipCamera3Video" }
    PipCameraItem { slot: 1; videoName: "pipCamera2Video" }
    PipCameraItem { slot: 0; videoName: "pipCamera1Video" }
}
