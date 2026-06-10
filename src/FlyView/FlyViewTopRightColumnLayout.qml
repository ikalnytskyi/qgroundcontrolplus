import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlyView
import QGroundControl.FlightMap

ColumnLayout {
    QGCPalette { id: qgcPal }

    spacing: ScreenTools.defaultFontPixelHeight / 2

    TerrainProgress {
        Layout.fillWidth: true
    }

    // We use a Loader to load the photoVideoControlComponent only when we have an active vehicle and a camera manager.
    // This make it easier to implement PhotoVideoControl without having to check for the mavlink camera
    // to be null all over the place
    Loader {
        id:                 photoVideoControlLoader
        Layout.alignment:   Qt.AlignRight
        sourceComponent:    globals.activeVehicle && globals.activeVehicle.cameraManager ? photoVideoControlComponent : undefined

        property real rightEdgeCenterInset: visible ? parent.width - x : 0

        Component {
            id: photoVideoControlComponent

            PhotoVideoControl {
            }
        }
    }

    Rectangle {
        Layout.alignment:       Qt.AlignRight
        visible:                QGroundControl.videoManager.isStreamSource && !photoVideoControlLoader.item
        width:                  audioIcon.height + (ScreenTools.defaultFontPixelWidth * 2)
        height:                 audioIcon.height + ScreenTools.defaultFontPixelWidth
        radius:                 ScreenTools.defaultFontPixelHeight / 2
        color:                  Qt.rgba(qgcPal.window.r, qgcPal.window.g, qgcPal.window.b, 0.5)

        QGCColoredImage {
            id:                     audioIcon
            anchors.centerIn:       parent
            width:                  ScreenTools.defaultFontPixelHeight * 1.5
            height:                 width
            sourceSize.height:      height
            fillMode:               Image.PreserveAspectFit
            mipmap:                 true
            smooth:                 true
            color:                  "white"
            source:                 QGroundControl.videoManager.audioMuted
                                      ? "/InstrumentValueIcons/volume-off.svg"
                                      : "/InstrumentValueIcons/volume-up.svg"

            QGCMouseArea {
                fillItem: parent
                onClicked: QGroundControl.videoManager.toggleAudioMuted()
            }
        }
    }
}
