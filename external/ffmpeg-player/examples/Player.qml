import QtQuick 2.15
import QtQuick.Window 2.15
import FFmpegPlayer 1.0

Window {
    id: app
    visible: true
    width: 800
    height: 480
    color: "black"
    title: "FFmpegPlayer"

    // 由宿主传入 MP4/.mpd URL；独立音轨通过 audioUrl 传入。
    property url videoUrl: ""
    property url audioUrl: ""
    property url subtitleUrl: ""
    property url subtitleFontUrl: ""
    property alias subtitlesEnabled: player.subtitlesEnabled
    property string displayTitle: ""
    // Optional cover supplied by the host; stays hidden after playback starts.
    property url coverSource: ""
    property bool playbackStarted: false
    readonly property bool playing: player.playbackState === VideoPlayer.PlayingState
    property var headers: ({ "Referer": "https://www.bilibili.com/" })
    property real seekPreviewPosition: 0
    property real uiScale: Math.max(0.5, Math.min(width / 320, height / 170))
    property bool zoomMode: false
    property real zoomLevel: 0
    property real panX: 0
    property real panY: 0
    property bool tapToToggle: true
    property bool holdToBoost: true
    property alias holdPlaybackRate: player.boostRate
    property real seekSecondsPerWidth: 60
    property bool edgeProgressEnabled: true
    property alias edgeProgressColor: player.progressBarColor
    property alias edgeProgressBackgroundColor: player.progressBarBackgroundColor
    property alias edgeProgressHeight: player.progressBarHeight
    readonly property bool swipeSeeking: gestureArea.pressed && gestureArea.gestureValid && gestureArea.dragMode === 1
    readonly property bool previewSeeking: swipeSeeking || seekMouseArea.pressed
    readonly property real displayedPosition: previewSeeking ? seekPreviewPosition : player.position
    property bool osdVisible: true
    property bool hideOsdAfterPlaybackStarts: false
    property bool componentReady: false
    property bool windowClosed: false
    readonly property bool controlPressed: gestureArea.pressed || seekMouseArea.pressed || resetZoomButton.pressed ||
            zoomInButton.pressed || zoomOutButton.pressed || closeButton.pressed ||
            backwardButton.pressed || playButton.pressed || forwardButton.pressed ||
            speedButton.pressed || subtitleButton.pressed

    onVideoUrlChanged: scheduleReload()
    onAudioUrlChanged: scheduleReload()
    onHeadersChanged: scheduleReload()
    onControlPressedChanged: {
        if (controlPressed)
            osdHideTimer.stop()
        else
            Qt.callLater(restartOsdTimer)
    }
    onClosing: function(close) {
        if (close.accepted) {
            windowClosed = true
            osdHideTimer.stop()
            player.stop()
            gestureArea.cancelGesture()
        }
    }
    onVisibleChanged: {
        if (!visible && componentReady)
            gestureArea.cancelGesture()
        if (visible && componentReady && windowClosed) {
            windowClosed = false
            scheduleReload()
        }
    }

    function scheduleReload() {
        if (componentReady && !windowClosed)
            Qt.callLater(reloadInputs)
    }

    function reloadInputs() {
        if (!componentReady || windowClosed)
            return
        // 合并同一轮事件中的地址、音轨和请求头更新，再一次性打开。
        if (videoUrl.toString().length)
            player.open(videoUrl, audioUrl, headers)
        else
            player.source = ""
    }

    function restartOsdTimer() {
        osdHideTimer.stop()
        if (osdVisible && !zoomMode && !controlPressed && !windowClosed &&
                player.playbackState === VideoPlayer.PlayingState)
            osdHideTimer.start()
    }

    function setOsdVisible(visible) {
        osdHideTimer.stop()
        hideOsdAfterPlaybackStarts = false
        osdVisible = visible || zoomMode
    }

    function showOsdForPlayback(initiatedPlayback) {
        osdHideTimer.stop()
        hideOsdAfterPlaybackStarts = false
        osdVisible = true
        if (player.playbackState === VideoPlayer.PlayingState)
            restartOsdTimer()
        else
            hideOsdAfterPlaybackStarts = initiatedPlayback &&
                    player.playbackState === VideoPlayer.LoadingState
    }

    function zoomIn() {
        gestureArea.cancelGesture()
        zoomMode = true
        setOsdVisible(true)
        zoomLevel += 0.25
    }

    function zoomOut() {
        zoomLevel = Math.max(0, zoomLevel - 0.25)
        if (zoomLevel === 0)
            resetZoom()
    }

    function resetZoom() {
        zoomMode = false
        zoomLevel = 0
        panX = 0
        panY = 0
        showOsdForPlayback(false)
    }

    function resolvedTitle() {
        if (displayTitle.trim().length)
            return displayTitle
        var sourceText = player.source.toString()
        if (!sourceText.length)
            return "FFmpeg Player"
        var path = sourceText.split(/[?#]/)[0]
        var name = path.substring(path.lastIndexOf("/") + 1)
        try { name = decodeURIComponent(name) } catch (e) {}
        return name || path || "FFmpeg Player"
    }

    function formatTime(value) {
        var seconds = Math.max(0, Math.floor(value || 0))
        var hours = Math.floor(seconds / 3600)
        var minutes = Math.floor((seconds % 3600) / 60)
        var remainder = seconds % 60
        function twoDigits(number) { return number < 10 ? "0" + number : "" + number }
        return hours > 0
                ? hours + ":" + twoDigits(minutes) + ":" + twoDigits(remainder)
                : twoDigits(minutes) + ":" + twoDigits(remainder)
    }

    Item {
        id: videoViewport
        anchors.fill: parent
        clip: true

        VideoPlayer {
            id: player
            objectName: "player"
            subtitleSource: app.subtitleUrl
            subtitleFontFile: app.subtitleFontUrl
            progressBarEnabled: app.edgeProgressEnabled && !app.zoomMode
            progressBarPosition: app.previewSeeking ? app.seekPreviewPosition : -1
            anchors.fill: parent
            scale: Math.pow(2, app.zoomLevel)
            transformOrigin: Item.Center
            transform: Translate { x: app.panX; y: app.panY }
            onSourceChanged: {
                gestureArea.cancelGesture()
                app.resetZoom()
                app.setOsdVisible(true)
            }
            onPlaybackStateChanged: {
                if (player.playbackState === VideoPlayer.PlayingState)
                    app.playbackStarted = true
                if (player.loading || player.playbackState === VideoPlayer.StoppedState ||
                        (player.playbackState !== VideoPlayer.PlayingState && player.boosted))
                    gestureArea.cancelGesture()
                if (app.hideOsdAfterPlaybackStarts && player.playbackState === VideoPlayer.PlayingState) {
                    app.hideOsdAfterPlaybackStarts = false
                    app.restartOsdTimer()
                } else if (app.hideOsdAfterPlaybackStarts && player.playbackState === VideoPlayer.StoppedState) {
                    app.hideOsdAfterPlaybackStarts = false
                }
            }
            onVisibleChanged: if (!visible) gestureArea.cancelGesture()
            onEnabledChanged: if (!enabled) gestureArea.cancelGesture()
            Component.onDestruction: endBoost()
        }

        Image {
            anchors.fill: parent
            source: app.coverSource
            visible: !app.playbackStarted
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
        }

        MouseArea {
            id: gestureArea
            objectName: "gestureArea"
            anchors.fill: parent
            enabled: player.enabled && !player.loading && !app.zoomMode
            pressAndHoldInterval: 400
            property bool held: false
            property bool moved: false
            property bool gestureValid: false
            // 0：未定方向；1：横向跳转；2：纵向控制层手势。
            property int dragMode: 0
            property real startX: 0
            property real startY: 0
            property real startPosition: 0

            function cancelGesture() {
                gestureValid = false
                moved = true
                dragMode = 0
                player.endBoost()
            }

            function updateSeekPreview(x) {
                var seconds = isFinite(app.seekSecondsPerWidth) && app.seekSecondsPerWidth > 0
                        ? app.seekSecondsPerWidth : 60
                app.seekPreviewPosition = Math.max(0, Math.min(player.duration,
                        startPosition + (x - startX) / Math.max(1, width) * seconds))
            }

            onEnabledChanged: if (!enabled) cancelGesture()
            onPressed: {
                held = false
                moved = false
                gestureValid = true
                dragMode = 0
                startX = mouse.x
                startY = mouse.y
                startPosition = player.position
            }
            onPositionChanged: {
                if (!pressed || !gestureValid)
                    return
                var dx = Math.abs(mouse.x - startX)
                var dy = Math.abs(mouse.y - startY)
                var threshold = Qt.styleHints.startDragDistance
                if (dx > threshold || dy > threshold) {
                    moved = true
                    if (held) {
                        cancelGesture()
                        return
                    }
                    if (dragMode === 0) {
                        if (dx > dy * 1.25 && player.duration > 0 &&
                                (player.playbackState === VideoPlayer.PlayingState ||
                                 player.playbackState === VideoPlayer.PausedState))
                            dragMode = 1
                        else if (dy > dx * 1.25)
                            dragMode = 2
                    }
                }
                if (dragMode === 1)
                    updateSeekPreview(mouse.x)
            }
            onPressAndHold: {
                if (app.holdToBoost && gestureValid && !moved &&
                        player.playbackState === VideoPlayer.PlayingState) {
                    held = true
                    player.beginBoost()
                }
            }
            onReleased: {
                player.endBoost()
                if (!gestureValid)
                    return
                if (dragMode === 1) {
                    updateSeekPreview(mouse.x)
                    player.seek(app.seekPreviewPosition)
                    dragMode = 0
                    return
                }
                var deltaX = mouse.x - startX
                var deltaY = mouse.y - startY
                if (!held && dragMode === 2 && Math.abs(deltaY) >= Math.max(40, height * 0.07) &&
                        Math.abs(deltaY) > Math.abs(deltaX) * 1.25)
                    app.setOsdVisible(deltaY > 0)
            }
            onCanceled: cancelGesture()
            onClicked: {
                if (app.tapToToggle && gestureValid && !held && !moved) {
                    player.togglePlayPause()
                    app.setOsdVisible(!app.osdVisible)
                }
            }
        }
    }

    Connections {
        target: app
        function onActiveChanged() {
            if (!app.active)
                gestureArea.cancelGesture()
        }
    }

    Timer {
        id: osdHideTimer
        interval: 2000
        repeat: false
        onTriggered: {
            if (!app.zoomMode && !app.controlPressed && !app.windowClosed &&
                    player.playbackState === VideoPlayer.PlayingState)
                app.osdVisible = false
        }
    }

    MouseArea {
        id: zoomPanArea
        anchors.fill: parent
        enabled: app.zoomMode
        z: 1
        property real lastX: 0
        property real lastY: 0
        onPressed: {
            lastX = mouse.x
            lastY = mouse.y
        }
        onPositionChanged: {
            if (!pressed)
                return
            app.panX += mouse.x - lastX
            app.panY += mouse.y - lastY
            lastX = mouse.x
            lastY = mouse.y
        }
    }

    Component.onCompleted: {
        componentReady = true
        scheduleReload()
    }

    Text {
        id: gestureIndicator
        objectName: "gestureIndicator"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 12
        anchors.topMargin: 6
        visible: app.swipeSeeking || player.boosted
        text: app.swipeSeeking
                ? app.formatTime(app.seekPreviewPosition) + "/" + app.formatTime(player.duration)
                : Number(player.playbackRate).toString() + "×"
        z: 4
        color: "white"
        font.pixelSize: Math.max(12, Math.min(16, 14 * app.uiScale))
        style: Text.Outline
        styleColor: "#aa000000"
    }

    Text {
        id: errorLabel
        objectName: "errorLabel"
        text: {
            var messages = []
            if (!player.loading && player.errorString.length)
                messages.push(player.errorString)
            if (player.subtitleErrorString.length)
                messages.push("字幕：" + player.subtitleErrorString)
            return messages.join("\n")
        }
        textFormat: Text.PlainText
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: controls.top
        anchors.bottomMargin: 10
        width: parent.width * 0.9
        visible: text.length > 0
        z: 4
        color: "#ffd2d2"
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: 13
        style: Text.Outline
        styleColor: "#dd000000"
    }

    Column {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 16 * app.uiScale
        anchors.topMargin: Math.max(25 * app.uiScale, gestureIndicator.y + gestureIndicator.height + 8)
        spacing: 10 * app.uiScale
        z: 3
        opacity: app.osdVisible ? 1 : 0
        enabled: app.osdVisible
        Behavior on opacity { NumberAnimation { duration: 250 } }

        PlayerButton {
            id: resetZoomButton
            objectName: "resetZoomButton"
            width: 21 * app.uiScale
            height: 21 * app.uiScale
            onClicked: app.resetZoom()
            contentItem: Text {
                text: "↺"
                color: "#f4f6f8"
                font.pixelSize: 16 * app.uiScale
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 5
                color: "transparent"
            }
        }

        PlayerButton {
            id: zoomInButton
            objectName: "zoomInButton"
            width: 21 * app.uiScale
            height: 21 * app.uiScale
            onClicked: app.zoomIn()
            contentItem: Text {
                text: "+"
                color: "#f4f6f8"
                font.pixelSize: 16 * app.uiScale
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 5
                color: "transparent"
            }
        }

        PlayerButton {
            id: zoomOutButton
            objectName: "zoomOutButton"
            visible: app.zoomMode && app.zoomLevel > 0
            width: 21 * app.uiScale
            height: 21 * app.uiScale
            onClicked: app.zoomOut()
            contentItem: Text {
                text: "−"
                color: "#f4f6f8"
                font.pixelSize: 16 * app.uiScale
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 5
                color: "transparent"
            }
        }
    }

    PlayerButton {
        id: closeButton
        objectName: "closeButton"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 10 * app.uiScale
        anchors.rightMargin: 10 * app.uiScale
        width: 23 * app.uiScale
        height: 23 * app.uiScale
        z: 3
        opacity: app.osdVisible ? 1 : 0
        enabled: app.osdVisible
        Behavior on opacity { NumberAnimation { duration: 250 } }
        onClicked: app.close()
        contentItem: Text {
            text: "×"
            color: "#e8edf2"
            font.pixelSize: 17 * app.uiScale
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 5
            color: "transparent"
        }
    }

    Rectangle {
        id: controls
        objectName: "controls"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: app.edgeProgressEnabled && !app.zoomMode ? app.edgeProgressHeight : 0
        height: app.height < 250 ? 92 : 108
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#00151c26" }
            GradientStop { position: 0.35; color: "#80151c26" }
            GradientStop { position: 1.0; color: "#dc101620" }
        }
        z: 2
        visible: !app.zoomMode
        opacity: app.osdVisible ? 1 : 0
        enabled: app.osdVisible
        Behavior on opacity { NumberAnimation { duration: 250 } }

        Column {
            id: controlContent
            anchors.fill: parent
            anchors.leftMargin: Math.max(8, parent.width * 0.025)
            anchors.rightMargin: Math.max(8, parent.width * 0.025)
            anchors.topMargin: 6
            anchors.bottomMargin: 6
            spacing: 3

            Item {
                id: titleRow
                width: parent.width
                height: app.height < 250 ? 18 : 22

                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: app.resolvedTitle()
                    color: "#f4f6f8"
                    font.pixelSize: Math.max(12, Math.min(16, parent.height * 0.57))
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Item {
                id: seekRow
                width: parent.width
                height: 20

                Text {
                    id: elapsedLabel
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 36
                    text: app.formatTime(app.displayedPosition)
                    color: "#e6eaf0"
                    font.pixelSize: 10
                    font.family: "monospace"
                    verticalAlignment: Text.AlignVCenter
                }

                Item {
                    id: seekTrack
                    anchors.left: elapsedLabel.right
                    anchors.right: remainingLabel.left
                    anchors.leftMargin: Math.max(12, parent.width * 0.065)
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        height: 3
                        radius: 2
                        color: "#70dbe4ee"
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: player.duration > 0
                               ? parent.width * Math.max(0, Math.min(1, app.displayedPosition / player.duration))
                               : 0
                        height: 3
                        radius: 2
                        color: "#39a9ff"
                    }

                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        y: (parent.height - height) / 2
                        x: {
                            var fraction = player.duration > 0
                                    ? Math.max(0, Math.min(1, app.displayedPosition / player.duration))
                                    : 0
                            return Math.max(0, Math.min(parent.width - width, fraction * parent.width - width / 2))
                        }
                        color: "#39a9ff"
                    }

                    MouseArea {
                        id: seekMouseArea
                        objectName: "seekMouseArea"
                        anchors.fill: parent
                        enabled: !player.loading && player.duration > 0
                        onPressed: app.seekPreviewPosition = Math.max(0, Math.min(1, mouse.x / width)) * player.duration
                        onPositionChanged: {
                            if (pressed)
                                app.seekPreviewPosition = Math.max(0, Math.min(1, mouse.x / width)) * player.duration
                        }
                        onReleased: player.seek(app.seekPreviewPosition)
                    }
                }

                Text {
                    id: remainingLabel
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 36
                    text: app.formatTime(player.duration)
                    color: "#e6eaf0"
                    font.pixelSize: 10
                    font.family: "monospace"
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Item {
                id: transportRow
                width: parent.width
                height: parent.height - titleRow.height - seekRow.height - parent.spacing * 2

                Row {
                    anchors.centerIn: parent
                    spacing: 8

                    PlayerButton {
                        id: backwardButton
                        width: app.height < 250 ? 36 : 44
                        height: app.height < 250 ? 32 : 40
                        onClicked: player.seekBackward(10)
                        contentItem: PlayerTransportIcon { direction: -1 }
                        background: Rectangle {
                            radius: 5
                            color: "transparent"
                        }
                    }

                    PlayerButton {
                        id: playButton
                        objectName: "playButton"
                        width: app.height < 250 ? 36 : 44
                        height: app.height < 250 ? 32 : 40
                        onClicked: {
                            var startsPlayback = player.playbackState === VideoPlayer.PausedState ||
                                    player.playbackState === VideoPlayer.StoppedState
                            player.togglePlayPause()
                            app.showOsdForPlayback(startsPlayback)
                        }
                        contentItem: PlayerTransportIcon {
                            playing: app.playing
                        }
                        background: Rectangle {
                            radius: 5
                            color: "transparent"
                        }
                    }

                    PlayerButton {
                        id: forwardButton
                        width: app.height < 250 ? 36 : 44
                        height: app.height < 250 ? 32 : 40
                        onClicked: player.seekForward(10)
                        contentItem: PlayerTransportIcon { direction: 1 }
                        background: Rectangle {
                            radius: 5
                            color: "transparent"
                        }
                    }
                }

                PlayerButton {
                    id: subtitleButton
                    objectName: "subtitleButton"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 48
                    height: app.height < 250 ? 28 : 32
                    enabled: app.subtitleUrl.toString().length > 0
                    checked: app.subtitlesEnabled
                    onClicked: app.subtitlesEnabled = !app.subtitlesEnabled
                    Accessible.name: checked ? "关闭字幕" : "开启字幕"
                    contentItem: Text {
                        text: "字幕"
                        color: !subtitleButton.enabled ? "#777f89" : subtitleButton.checked ? "#39a9ff" : "#edf1f5"
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 4
                        color: "transparent"
                    }
                }

                PlayerButton {
                    id: speedButton
                    objectName: "speedButton"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 46
                    height: app.height < 250 ? 28 : 32
                    onClicked: {
                        var rates = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]
                        var index = 0
                        for (var i = 0; i < rates.length; ++i) {
                            if (Math.abs(rates[i] - player.playbackRate) < 0.001) {
                                index = i
                                break
                            }
                        }
                        player.playbackRate = rates[(index + 1) % rates.length]
                    }
                    contentItem: Text {
                        text: (player.playbackRate % 1 ? Number(player.playbackRate).toString() : Number(player.playbackRate).toFixed(0)) + "×"
                        color: "#edf1f5"
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 4
                        color: "transparent"
                    }
                }
            }
        }
    }
}
