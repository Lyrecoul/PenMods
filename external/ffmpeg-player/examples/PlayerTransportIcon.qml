import QtQuick 2.15

Item {
    id: icon
    // -1: backward, 0: play/pause, 1: forward.
    property int direction: 0
    property bool playing: false
    onDirectionChanged: drawing.requestPaint()
    onPlayingChanged: drawing.requestPaint()

    Canvas {
        id: drawing
        anchors.centerIn: parent
        width: 20
        height: 20
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.fillStyle = "#ffffff"
            if (icon.direction === 0) {
                if (icon.playing) {
                    ctx.fillRect(4, 3, 4, 14)
                    ctx.fillRect(12, 3, 4, 14)
                } else {
                    ctx.beginPath()
                    ctx.moveTo(5, 3)
                    ctx.lineTo(17, 10)
                    ctx.lineTo(5, 17)
                    ctx.closePath()
                    ctx.fill()
                }
            } else {
                ctx.save()
                if (icon.direction < 0) {
                    ctx.translate(20, 0)
                    ctx.scale(-1, 1)
                }
                for (var i = 0; i < 2; ++i) {
                    var x = 2 + i * 8
                    ctx.beginPath()
                    ctx.moveTo(x, 4)
                    ctx.lineTo(x + 8, 10)
                    ctx.lineTo(x, 16)
                    ctx.closePath()
                    ctx.fill()
                }
                ctx.restore()
            }
        }
    }
}
