#pragma once

namespace mod::filemanager {

class ImageViewer : public QObject, public Singleton<ImageViewer> {
    Q_OBJECT
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)

public:
    QString source() const;

    Q_INVOKABLE void open(const QString &path);

signals:
    void sourceChanged();

private:
    friend class Singleton<ImageViewer>;
    explicit ImageViewer(QObject *parent = nullptr);

    QString m_openingFileName;
};

} // namespace mod::filemanager
