#!/usr/bin/env python3
"""Compile the production session writer with host Qt; exercise incremental and failed saves.

Requires g++, pkg-config, Qt5Core and the xmake-installed nlohmann_json headers.
The serializer, data structures and writer are extracted verbatim from Backend, so
this test needs neither the vendor binary nor a connected device.
"""

import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
cpp = (root / "src/chatbot/Backend.cpp").read_text()
h = (root / "src/chatbot/Backend.h").read_text()
structs = h[h.index("struct MessagePart {") : h.index("class ChatBot :")]
serialize = cpp[
    cpp.index("static json messageDataToJson") : cpp.index(
        "static MessageData messageDataFromJson"
    )
]
writer = cpp[
    cpp.index("bool ChatBot::writeSessions") : cpp.index(
        "// -----------------------------------------------------------------------\n// 加载 sessions.json"
    )
]
source = (
    """#include <QtCore>
#include <nlohmann/json.hpp>
#include <cassert>
#include <iostream>
using json=nlohmann::json;
template<class... T> void error(T...) {}
"""
    + structs
    + """
class ChatBot { public:
QMap<QString, SessionData> m_persistedSessions;
QMap<QString, QString> m_sessionFiles;
bool m_sessionStoreReadFailed = false;
bool writeSessions(const QMap<QString, SessionData>&,const QString&,const QString&);
};
"""
    + serialize
    + writer
    + """
json read(const QString& path) {QFile f(path); assert(f.open(QIODevice::ReadOnly)); return json::parse(f.readAll().toStdString());}
int main(int argc,char** argv) {
QCoreApplication app(argc,argv); QTemporaryDir tmp; QString path=tmp.path()+"/sessions.json", dir=path+".d/";
ChatBot bot; QMap<QString,SessionData> sessions;
for(int i=0;i<30;++i){SessionData s; s.id=QString::number(i);s.title=s.id; for(int n=0;n<100;++n){ MessageData m;m.content=QString(6000,'x');s.messages.append(m);}sessions.insert(s.id,s);}
QElapsedTimer timer; timer.start(); assert(bot.writeSessions(sessions,"0",path)); auto first=timer.elapsed();
auto initial=bot.m_sessionFiles; auto manifest=read(dir+"index.json");assert(manifest["sessions"].size()==30);
assert(QDir(dir).entryList({"*.json"},QDir::Files).size()==31);
sessions["0"].messages[0].content="edited";timer.restart();assert(bot.writeSessions(sessions,"0",path));auto second=timer.elapsed();
int changed=0;for(auto it=initial.begin();it!=initial.end();++it) changed+=bot.m_sessionFiles[it.key()]!=it.value();assert(changed==1);
assert(read(dir+bot.m_sessionFiles["0"])["messages"][0]["content"]=="edited");
assert(!QFile::exists(dir+initial["0"]));
sessions.remove("1");assert(bot.writeSessions(sessions,"0",path));assert(read(dir+"index.json")["sessions"].size()==29);assert(!QFile::exists(dir+initial["1"]));
assert(bot.writeSessions(sessions,"2",path));assert(read(dir+"index.json")["activeSessionId"]=="2");
QFile blocker(tmp.path()+"/block");assert(blocker.open(QIODevice::WriteOnly));blocker.close();assert(!bot.writeSessions(sessions,"0",tmp.path()+"/block/sessions.json"));
std::cout<<"PASS: 30 sessions / 3000 messages; initial "<<first<<"ms, single edit "<<second<<"ms; only edited session rewritten; deletion, active selection and failed write verified\\n";
}
"""
)
include = next(
    (Path.home() / ".xmake/packages/n/nlohmann_json").glob(
        "**/include/nlohmann/json.hpp"
    )
).parents[1]
flags = subprocess.check_output(
    ["pkg-config", "--cflags", "--libs", "Qt5Core"], text=True
).split()
with tempfile.TemporaryDirectory(prefix="penmods-session-test-") as directory:
    test_source = Path(directory) / "test.cpp"
    executable = Path(directory) / "test"
    test_source.write_text(source)
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-O2",
            "-fPIC",
            f"-I{include}",
            str(test_source),
            "-o",
            str(executable),
            *flags,
        ],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
