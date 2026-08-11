import QtQuick

import QGroundControl

// Driving guide lines drawn over a video stream, showing the vehicle's projected width
// and near/far distance markers on the ground ahead of the camera, to help an operator
// judge clearance while remotely driving or flying the vehicle in close quarters.
// Geometry is derived from a simple pinhole projection of the camera's configured
// height, downward tilt, and horizontal/vertical field of view (see videoSettings parameter).
Canvas {
    id: root

    required property var videoSettings // videoSettings or video2Settings
    required property var videoItem     // the VideoOutput item actually rendering the frame

    // VideoOutput.contentRect is the on-screen sub-rectangle the video frame actually occupies
    // (item-local coordinates), already accounting for letterbox/pillarbox bars added whenever
    // videoFit isn't "Fill". Falls back to the full parent rect if the video item isn't ready yet.
    readonly property rect _contentRect: videoItem ? videoItem.contentRect : Qt.rect(0, 0, parent.width, parent.height)

    x:      _contentRect.x
    y:      _contentRect.y
    width:  _contentRect.width
    height: _contentRect.height

    readonly property bool   guideEnabled:  videoSettings ? videoSettings.drivingGuideEnabled.rawValue        : false
    readonly property real   cameraHeight:  videoSettings ? videoSettings.drivingGuideCameraHeight.rawValue   : 0
    readonly property real   cameraTilt:    videoSettings ? videoSettings.drivingGuideCameraTilt.rawValue     : 0
    readonly property real   cameraFov:     videoSettings ? videoSettings.drivingGuideCameraFov.rawValue      : 0
    readonly property real   cameraVfov:    videoSettings ? videoSettings.drivingGuideCameraVfov.rawValue     : 0
    readonly property real   vehicleWidth:  videoSettings ? videoSettings.drivingGuideVehicleWidth.rawValue   : 0
    readonly property real   nearDistance:  videoSettings ? videoSettings.drivingGuideNearDistance.rawValue   : 0
    readonly property real   farDistance:   videoSettings ? videoSettings.drivingGuideFarDistance.rawValue    : 0
    readonly property color  lineColor:     videoSettings ? videoSettings.drivingGuideColor.rawValue          : "#2196f3"
    readonly property real   lineWidth:     videoSettings ? videoSettings.drivingGuideLineWidth.rawValue      : 2
    readonly property real   cameraSideOffset:  videoSettings ? videoSettings.drivingGuideCameraSideOffset.rawValue  : 0
    readonly property real   cameraFrontSetback: videoSettings ? videoSettings.drivingGuideCameraFrontSetback.rawValue : 0

    onGuideEnabledChanged: requestPaint()
    onCameraHeightChanged: requestPaint()
    onCameraTiltChanged:   requestPaint()
    onCameraFovChanged:    requestPaint()
    onCameraVfovChanged:   requestPaint()
    onVehicleWidthChanged: requestPaint()
    onNearDistanceChanged: requestPaint()
    onFarDistanceChanged:  requestPaint()
    onLineColorChanged:    requestPaint()
    onLineWidthChanged:    requestPaint()
    onCameraSideOffsetChanged:   requestPaint()
    onCameraFrontSetbackChanged: requestPaint()
    onWidthChanged:        requestPaint()
    onHeightChanged:       requestPaint()

    // Projects a ground point (forward distance d, lateral offset x, meters, both relative to the
    // vehicle's own reference frame — d from its front edge, x from its centerline) to canvas pixel
    // coordinates using a pinhole model. Accounts for the camera's own position relative to that
    // reference frame: cameraSideOffset (positive = camera mounted right of centerline) and
    // cameraFrontSetback (positive = camera mounted behind/set back from the reference edge;
    // negative = camera mounted ahead of it, e.g. on a boom).
    // Returns null if the point is behind the camera and cannot be projected.
    function _project(d, x) {
        const xCam = x - cameraSideOffset
        const dCam = d + cameraFrontSetback
        const tiltRad = cameraTilt * Math.PI / 180
        const zCam = dCam * Math.cos(tiltRad) + cameraHeight * Math.sin(tiltRad)
        if (zCam <= 0.01) {
            return null
        }
        const yCam = cameraHeight * Math.cos(tiltRad) - dCam * Math.sin(tiltRad)
        // Horizontal and vertical FOV are independent settings, not derived from one another via
        // the image aspect ratio — real lenses (especially wide-angle ones) don't relate H/V FOV
        // that simply, as confirmed by camera datasheets listing them as separate specs.
        const tanHalfHFov = Math.tan(cameraFov * Math.PI / 360)
        const tanHalfVFov = Math.tan(cameraVfov * Math.PI / 360)
        const xNdc = (xCam / zCam) / tanHalfHFov
        const yNdc = (yCam / zCam) / tanHalfVFov
        return Qt.point((0.5 + 0.5 * xNdc) * root.width, (0.5 + 0.5 * yNdc) * root.height)
    }

    function _line(ctx, p1, p2) {
        ctx.beginPath()
        ctx.moveTo(p1.x, p1.y)
        ctx.lineTo(p2.x, p2.y)
        ctx.stroke()
    }

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()

        if (!guideEnabled || cameraFov <= 0 || cameraVfov <= 0 || root.width <= 0 || root.height <= 0) {
            return
        }

        const halfWidth = vehicleWidth / 2

        // zCam depends only on d (height/tilt/frontSetback), never on lateral position, so both side
        // edges share the same "closest distance the camera can actually see." When the configured
        // nearDistance falls behind that boundary, clip the edges to start there instead of dropping
        // them entirely — render as much of the guide as the camera geometry actually allows.
        const tiltRad = cameraTilt * Math.PI / 180
        const dClip = -cameraFrontSetback - cameraHeight * Math.tan(tiltRad)
        const edgeNear = Math.max(nearDistance, dClip + 0.05)

        const nearLeft  = _project(nearDistance, -halfWidth)
        const nearRight = _project(nearDistance,  halfWidth)
        const farLeft   = _project(farDistance,  -halfWidth)
        const farRight  = _project(farDistance,   halfWidth)
        const edgeNearLeft  = _project(edgeNear, -halfWidth)
        const edgeNearRight = _project(edgeNear,  halfWidth)

        ctx.strokeStyle = lineColor
        ctx.lineWidth = lineWidth

        // Draw whatever's valid rather than blanking the whole overlay — a configured near/far
        // distance can legitimately fall behind the camera (e.g. a large negative cameraFrontSetback,
        // meaning the camera is mounted well ahead of the reference edge), and that shouldn't hide
        // markers that project just fine.
        if (edgeNearLeft  && farLeft)   _line(ctx, edgeNearLeft,  farLeft)
        if (edgeNearRight && farRight)  _line(ctx, edgeNearRight, farRight)
        if (nearLeft  && nearRight) _line(ctx, nearLeft,  nearRight)
        if (farLeft   && farRight)  _line(ctx, farLeft,   farRight)

        // Extra 1m-spaced rungs between the near/far lines, like a car parking-guide overlay.
        for (let d = Math.floor(nearDistance) + 1; d < farDistance; d++) {
            const left  = _project(d, -halfWidth)
            const right = _project(d,  halfWidth)
            if (left && right) {
                _line(ctx, left, right)
            }
        }
    }
}
