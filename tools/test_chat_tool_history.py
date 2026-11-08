#!/usr/bin/env python3
"""Replay saved tool history through the production QML projection and tool views."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("QT_QUICK_BACKEND", "software")
from PySide6.QtCore import QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlComponent, QQmlEngine, QQmlExpression
from PySide6.QtQuick import QQuickItem

ROOT = Path(__file__).resolve().parents[1]
app = QGuiApplication([])
engine = QQmlEngine()


def evaluate(obj, script):
    expression = QQmlExpression(engine.rootContext(), obj, script)
    result = expression.evaluate()[0]
    assert not expression.hasError(), expression.error().toString()
    return result


def create(source, url=QUrl()):
    component = QQmlComponent(engine)
    component.setData(source.encode(), url)
    obj = component.create()
    assert obj is not None, [error.toString() for error in component.errors()]
    return component, obj


def run(qml):
    source = (qml / "ChatAssistant.qml").read_text()
    functions = []
    for name in ("_toolRow", "_makeReasoningEntry", "_historyRows"):
        match = re.search(r"^    function " + name + r"\(.*?^    }", source, re.M | re.S)
        assert match, name
        functions.append(match.group())
    component, projection = create("import QtQuick 2.15\nQtObject {\n"
        "property var chatbot: ({toolStates: [], responseMessageId: 'response'})\n"
        + "\n".join(functions) + "\n}")
    shell_result = "Exit code: 7\nstdout:\n中文输出\nstderr:\n执行失败\n"
    calls = [
        {"id": "shell", "function": {"name": "shell_exec", "arguments": '{"command":"printf 中文"}'}},
        {"id": "search", "function": {"name": "web_search", "arguments": '{"query":"示例"}'}},
    ]
    history = [
        {"messageId": "question", "role": "user", "content": "检查并搜索"},
        {"messageId": "batch", "role": "assistant", "content": "", "toolCallsJson": json.dumps(calls)},
        {"messageId": "result-shell", "role": "tool", "toolCallId": "shell", "content": shell_result},
        {"messageId": "result-search", "role": "tool", "toolCallId": "search", "content": "已取消搜索"},
        {"messageId": "answer", "role": "assistant", "content": "检查结束"},
    ]
    # JSON round-trip matches the saved-history boundary; no live signal state survives.
    rows = json.loads(evaluate(projection, "JSON.stringify(_historyRows(" + json.dumps(history) + "))"))
    assert [row["messageId"] for row in rows] == ["question:user", "batch:tools", "answer:answer"]
    tool = rows[1]
    assert tool["isToolCall"] and tool["isComplete"]
    entries = json.loads(tool["toolEntriesJson"])
    assert entries[0].get("name") == "shell_exec", "Reloaded shell call lost its tool type; result falls back to plain text"
    assert json.loads(entries[0]["arguments"])["command"] == "printf 中文"
    assert entries[0]["state"] != "done", "Legacy history must not invent execution success"
    assert entries[1]["state"] == "cancelled"
    snapshots = json.loads(json.dumps(entries))
    snapshots[0]["state"] = "error"
    snapshots[1]["state"] = "done"
    snapshots[1]["result"] = '{"results":[{"title":"示例","url":"https://example.com"}]}'
    evaluate(projection, "chatbot = ({toolStates: " + json.dumps(snapshots) + "})")
    restored = json.loads(evaluate(projection, "JSON.stringify(_historyRows(" + json.dumps(history) + "))"))
    assert json.loads(restored[1]["toolEntriesJson"]) == snapshots, "Recorded outcomes must survive without legacy reclassification"
    assert sum(row["isToolCall"] for row in restored) == 1, "History duplicated the recorded tool batch"
    evaluate(projection, "chatbot = ({toolStates: []})")
    incomplete = [history[0], {"messageId": "unknown", "role": "assistant", "content": "",
                              "toolCallsJson": '[{"id":"unrecorded"}]'}]
    unknown = json.loads(evaluate(projection, "JSON.stringify(_historyRows(" + json.dumps(incomplete) + "))"))
    unknown_entry = json.loads(unknown[1]["toolEntriesJson"])[0]
    assert unknown_entry["name"] == "" and unknown_entry["arguments"] == ""
    assert unknown_entry["state"] == "error" and unknown_entry["result"] == "调用结果未知"
    card_component, card = create(
        'import QtQuick 2.15\nItem { width:320; height:170\n'
        'property var qmlGlobal: ({fontFamilyZhCn: "sans-serif"})\n'
        'ToolCallCard { id: card; objectName: "card"; fontFamily: "sans-serif"; expanded:true; '
        'entriesJson: ' + json.dumps(tool["toolEntriesJson"]) + '; toolState: "error" }\n}',
        QUrl.fromLocalFile(str(qml / "assistant/messages/HistoryReplay.qml")))
    for _ in range(10):
        app.processEvents()
    def visual_items(item):
        for child in item.childItems():
            yield child
            yield from visual_items(child)
    assert isinstance(card, QQuickItem)
    texts = [obj.property("text") for obj in visual_items(card)]
    for expected in ("Shell 命令", "调用参数", "command", "printf 中文", "退出码", "7", "标准输出", "中文输出", "错误输出", "执行失败"):
        assert expected in texts, f"Restored tool card did not render structured field {expected!r}"
    # Render the actual expanded card into a host window, not just its row data.
    from PySide6.QtQuick import QQuickWindow
    from PySide6.QtTest import QTest
    window = QQuickWindow()
    window.resize(320, 170)
    card.setParentItem(window.contentItem())
    window.show()
    QTest.qWait(100)
    image = window.grabWindow()
    assert not image.isNull(), "Tool card surface did not render"
    screenshot = os.environ.get("PENMODS_TOOL_HISTORY_SCREENSHOT")
    if screenshot:
        assert image.save(screenshot)
    window.close()
    print("PASS: reloaded legacy/snapshot calls retain cards, arguments, structured shell results and recorded outcomes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", action="store_true")
    args = parser.parse_args()
    qml = ROOT / "resource/models/YDP02X/qml"
    with tempfile.TemporaryDirectory(prefix="penmods-tool-history-") as directory:
        if args.resources or not (qml / "ChatAssistant.qml").is_file():
            subprocess.run(["sh", str(ROOT / "scripts/unpack_qt_res.sh"), "YDP02X", "-o", directory], check=True)
            qml = Path(directory) / "qml"
        run(qml)


if __name__ == "__main__":
    main()
