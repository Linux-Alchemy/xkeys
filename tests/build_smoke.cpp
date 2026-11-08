#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

class BuildSmoke : public QObject
{
    Q_OBJECT

  private slots:
    void roundTripsJson();
};

void BuildSmoke::roundTripsJson()
{
    const QJsonObject original{{QStringLiteral("project"), QStringLiteral("xkeys")}};
    const QByteArray encoded = QJsonDocument(original).toJson(QJsonDocument::Compact);

    QJsonParseError error;
    const QJsonDocument decoded = QJsonDocument::fromJson(encoded, &error);

    QCOMPARE(error.error, QJsonParseError::NoError);
    QCOMPARE(decoded.object(), original);
}

QTEST_GUILESS_MAIN(BuildSmoke)
#include "build_smoke.moc"
