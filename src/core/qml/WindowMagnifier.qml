// SPDX-FileCopyrightText: 2026 PicaLens contributors
// SPDX-License-Identifier: Apache-2.0

import QtQuick

Item {
    id: magnifier
    required property Item sourceWindow
    required property rect sourceRect
    required property size sourcePixels
    required property rect picture
    property bool captureHidden: false
    property bool ready: false

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    ShaderEffectSource {
        id: image
        sourceItem: magnifier.sourceWindow
        sourceRect: magnifier.sourceRect
        textureSize: magnifier.sourcePixels
        hideSource: magnifier.captureHidden
        live: true
        smooth: true
        x: magnifier.picture.x
        y: magnifier.picture.y
        width: magnifier.picture.width
        height: magnifier.picture.height
        onScheduledUpdateCompleted: magnifier.ready = true
        Component.onCompleted: scheduleUpdate()
    }
}
