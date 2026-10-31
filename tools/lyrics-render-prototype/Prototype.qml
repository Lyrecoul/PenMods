import QtQuick 2.15
import LyricsPrototype 1.0

// Throwaway: identical layout/time in both variants; no production player state.
Rectangle {
    id: root
    width: 320; height: 170
    color: "#101216"
    focus: true
    Keys.onEscapePressed: Qt.quit()
    property bool nativeMode: initialNative
    property bool paused: false
    property bool capturing: false
    property double elapsed: 0
    Text {
        x: 10; y: 3; color: "#9ba1ab"; font.pixelSize: 11
        text: (root.nativeMode ? "B · C++ 缓存纹理" : "A · QML 双层裁切") + "   样例 " + (probe.sample + 1)
    }
    Item {
        x: 54; y: 23; width: 256; height: 116; clip: true
        // Both variants use the exact same visual-line positions from QTextLayout.
        Loader {
            active: !root.nativeMode
            sourceComponent: Component { Item {
            Repeater {
                model: probe.lines
                Text {
                    x: 0; y: modelData.y
                    text: modelData.text
                    font.family: probe.family; font.pixelSize: 21
                    color: "#606671"; textFormat: Text.PlainText
                    renderType: Text.NativeRendering
                }
            }
            Repeater {
                model: 12 // Fixed clip pool: changes geometry without recreating Text on every frame.
                Item {
                    property var band: index < probe.bands.length ? probe.bands[index] : null
                    visible: band !== null
                    x: band ? band.x : 0; y: band ? band.y : 0
                    width: band ? band.w : 0; height: band ? band.h : 0; clip: true
                    Text {
                        x: -parent.x; y: 0
                        text: {
                            if (!parent.band) return ""
                            var rows = probe.lines
                            for (var i = 0; i < rows.length; ++i)
                                if (rows[i].y === parent.band.y) return rows[i].text
                            return ""
                        }
                        font.family: probe.family; font.pixelSize: 21
                        color: "#8ac8ff"; textFormat: Text.PlainText
                        renderType: Text.NativeRendering
                    }
                }
            }
        } } }
        Loader {
            active: root.nativeMode
            sourceComponent: Component {
                CachedLine { width: 256; height: probe.height; layout: probe }
            }
        }
    }
    Row {
        x: 6; y: 141; spacing: 4
        Repeater {
            model: ["A / B", "换样例", "暂停", "归零"]
            Rectangle {
                width: 74; height: 27; radius: 5; color: "#303641"
                Text {
                    anchors.centerIn: parent; color: "white"; font.pixelSize: 12
                    text: index === 2 && root.paused ? "继续" : modelData
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (index === 0) root.nativeMode = !root.nativeMode
                        else if (index === 1) { probe.sample += 1; root.elapsed = 0 }
                        else if (index === 2) root.paused = !root.paused
                        else root.elapsed = 0
                        console.log("STATE native=" + root.nativeMode + " sample=" + probe.sample
                                    + " paused=" + root.paused + " time=" + root.elapsed)
                    }
                }
            }
        }
    }
    Rectangle {
        x: 288; y: 0; width: 32; height: 23; color: "#303641"
        Text { anchors.centerIn: parent; text: "×"; color: "white" }
        MouseArea { anchors.fill: parent; onClicked: Qt.quit() }
    }
    Timer {
        interval: 16; running: !root.capturing; repeat: true
        property double last: Date.now()
        onTriggered: {
            var now = Date.now()
            if (!root.paused) root.elapsed += now - last
            last = now
            if (benchmark) {
                var nextSample = Math.min(4, Math.floor(root.elapsed / 2000))
                if (nextSample !== probe.sample) probe.sample = nextSample
                probe.time = (root.elapsed % 2000) * 3
            } else probe.time = root.elapsed % 6500
        }
    }
}
