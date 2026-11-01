#!/usr/bin/env python3
"""Exercise the real update page with progress signals and assert stable scroll position.

Requires PySide6. Device controls are replaced with basic QtQuick fixtures; the page
and all its ListView/model/progress bindings are loaded unchanged from source.
"""
import os
from pathlib import Path
import tempfile

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("QT_QUICK_BACKEND", "software")
from PySide6.QtCore import QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlComponent, QQmlEngine, QQmlExpression
from PySide6.QtQuick import QQuickItem

root = Path(__file__).resolve().parents[1]
source = root / "resource/models/YDP02X/qml/settingpages/YSettingUpdate.qml"
app = QGuiApplication([])
engine = QQmlEngine()
kept = []


def evaluate(obj, script):
    expression = QQmlExpression(engine.rootContext(), obj, script)
    value = expression.evaluate()[0]
    assert not expression.hasError(), expression.error().toString()
    return value


def native(name, body):
    component = QQmlComponent(engine)
    component.setData(("import QtQuick 2.12; QtObject {" + body + "}").encode(), QUrl())
    obj = component.create()
    assert obj, [e.toString() for e in component.errors()]
    kept.extend([component, obj])
    engine.rootContext().setContextProperty(name, obj)
    return obj


center = native("updateCenter", """
 property var components: ['core','player','mathjax','ppocr'].map(function(id){return {
 id:id,name:id,installedVersion:'1.0.0',availableVersion:'1.0.0',note:'note',sizeText:'10 MB',
 stateText:'downloading',error:'',progress:0,available:true,installed:true,pending:false,
 busy:true,autoUpdate:false,canDownload:false}})
 property bool busy:true
 property bool pendingRestart:false
 property bool launcherReady:true
 property string statusText:'ready'
 function refresh(){}
""")
native("updater", "property bool checking:false; property string sourceStatus:''")
native("ocrBackend", "property bool enabled:false")
native("chatbot", "property bool mathRenderEnabled:false; property string bubbleRenderMode:'full'; property string mathServerStatus:''")

with tempfile.TemporaryDirectory(prefix="penmods-update-scroll-") as directory:
    qml = Path(directory)
    (qml / "settingpages").mkdir()
    (qml / "commons").mkdir()
    (qml / "components").mkdir()
    (qml / "commons/qmldir").write_text("singleton YColors 1.0 YColors.qml\n")
    (qml / "commons/YColors.qml").write_text('pragma Singleton\nimport QtQuick 2.12\nQtObject { property color grayNormal:"gray"; property color grayText:"gray"; property color blueText:"blue"; property color blueRect:"blue"; property color yellow:"yellow" }')
    (qml / "settingpages/YSettingItemPage.qml").write_text('import QtQuick 2.12\nItem { width:320; height:170; property bool ignoreDefaultBackButtonClicked:false; signal backButtonClickedCallback(); function openSettingPage(name, properties){} }')
    controls = {
        "YText": "Text {}",
        "YMouseArea": "MouseArea {}",
        "YIconButton": 'Rectangle { property string source:""; signal validClicked() }',
        "YPressedButton": 'Rectangle { property string text:""; property int pixelSize:13; signal clicked() }',
        "YSwitch": 'Item { property bool switchOn:false }',
    }
    for name, body in controls.items():
        (qml / ("components/" + name + ".qml")).write_text("import QtQuick 2.12\n" + body)
    page_path = qml / "settingpages/YSettingUpdate.qml"
    page_path.write_text(source.read_text())
    component = QQmlComponent(engine, QUrl.fromLocalFile(str(page_path)))
    assert component.status() == QQmlComponent.Ready, [e.toString() for e in component.errors()]
    page = component.create()
    assert page, [e.toString() for e in component.errors()]
    kept.extend([component, page])
    for _ in range(5):
        app.processEvents()
    lists = [o for o in page.findChildren(QQuickItem)
             if o.property("count") == 4 and o.property("contentY") is not None]
    assert len(lists) == 1, "component list not found"
    view = lists[0]
    evaluate(view, "forceLayout()")

    def visual_items(item):
        for child in item.childItems():
            yield child
            yield from visual_items(child)

    view.setProperty("contentY", 100.0)
    app.processEvents()
    original = view.property("contentY")
    assert original > 50, original
    for progress in (1, 25, 50, 75, 100):
        evaluate(center, "components = components.map(function(c){var row = Object.assign({}, c); row.progress = " + str(progress) + "; row.stateText = 'progress " + str(progress) + "'; return row;})")
        for _ in range(5):
            app.processEvents()
        current = view.property("contentY")
        assert abs(current - original) < 0.01, f"progress {progress}: scroll jumped {original} -> {current}"
        assert any(o.property("text") == f"progress {progress}" for o in visual_items(view)), "row state did not refresh"
        rows = [o for o in visual_items(view) if o.property("component") is not None]
        assert rows and all(evaluate(row, "component.progress") == progress for row in rows), "row progress stale"
    print(f"PASS: five progress updates preserve contentY={original}; row text/progress refresh")
