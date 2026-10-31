// Diagnostic client. Pairing material is read directly into process memory;
// only HTTP status, pin result and body length are written to the receipt.
#include "backend/pairedcertificate.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSslKey>
#include <QTimer>
#include <QUrl>
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (argc != 4) return 2;
    QSettings settings(QString::fromLocal8Bit(argv[1]), QSettings::IniFormat);
    QSslCertificate local(settings.value(QStringLiteral("certificate")).toByteArray());
    QSslKey key(settings.value(QStringLiteral("key")).toByteArray(), QSsl::Rsa);
    QStringList certificates;
    for (const auto &name : settings.allKeys())
        if (name.endsWith(QStringLiteral("/srvcert"))) certificates.append(name);
    if (certificates.size() != 1 || local.isNull() || key.isNull()) return 3;
    QSslCertificate pinned(settings.value(certificates.front()).toByteArray());
    if (pinned.isNull()) return 3;
    QNetworkAccessManager manager;
    PairedCertificate::enforce(&manager, &manager, pinned);
    QObject::connect(&manager, &QNetworkAccessManager::sslErrors, &manager,
                     [pinned](QNetworkReply *reply, const QList<QSslError> &errors) {
        if (PairedCertificate::canIgnoreErrors(pinned, errors)) reply->ignoreSslErrors(errors);
    });
    QUrl url; url.setScheme(QStringLiteral("https")); url.setHost(QString::fromLocal8Bit(argv[2]));
    url.setPort(47984); url.setPath(QStringLiteral("/api/v1/clipboard/files/open"));
    QNetworkRequest request(url); auto ssl = QSslConfiguration::defaultConfiguration();
    ssl.setLocalCertificate(local); ssl.setPrivateKey(key); request.setSslConfiguration(ssl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Content-Type", "application/octet-stream");
    auto reply = manager.post(request, QByteArray());
    QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout.start(8000); if (!reply->isFinished()) loop.exec();
    auto body = reply->readAll();
    QJsonObject result {{QStringLiteral("HTTPStatus"), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()},
        {QStringLiteral("NetworkError"), static_cast<int>(reply->error())},
        {QStringLiteral("PinnedPeerMatched"), PairedCertificate::matches(pinned, reply->sslConfiguration().peerCertificate())},
        {QStringLiteral("BodyBytes"), body.size()}};
    body.fill('\0');
    QFile output(QString::fromLocal8Bit(argv[3])); if (!output.open(QIODevice::WriteOnly)) return 4;
    output.write(QJsonDocument(result).toJson()); return 0;
}
