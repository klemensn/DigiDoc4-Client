/*
 * QEstEidUtil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
 
#include "sslConnect.h"

#include "Application.h"
#include "MainWindow.h"
#include "QSigner.h"
#include "TokenData.h"
#include "dialogs/WaitDialog.h"

#include <common/Configuration.h>

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslKey>

class SSLConnect::Private
{
public:
	QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
	QList<QSslCertificate> trusted;
};

SSLConnect::SSLConnect(QObject *parent)
	: QObject(parent)
	, d(new Private)
{
#ifdef CONFIG_URL
	d->ssl.setCaCertificates({});
	for(const QJsonValue c: Configuration::instance().object().value(QStringLiteral("CERT-BUNDLE")).toArray())
		d->trusted << QSslCertificate(QByteArray::fromBase64(c.toString().toLatin1()), QSsl::Der);
#endif
}

SSLConnect::~SSLConnect()
{
	delete d;
}

void SSLConnect::fetch()
{
	QWidget *active = qApp->activeWindow();
	for(QWidget *w: qApp->topLevelWidgets())
	{
		if(MainWindow *main = qobject_cast<MainWindow*>(w))
		{
			active = main;
			break;
		}
	}
	WaitDialogHolder *popup = new WaitDialogHolder(active, tr("Downloading picture"));

	QSslCertificate cert = qApp->signer()->tokenauth().cert();
	QSslKey key = qApp->signer()->key();
	if(cert.isNull() || key.isNull())
	{
		delete popup;
		emit error(QStringLiteral("Private key is missing"));
		return;
	}
	d->ssl.setPrivateKey(key);
	d->ssl.setLocalCertificate(cert);

	QJsonObject obj;
#ifdef CONFIG_URL
	obj = Configuration::instance().object();
#endif
	QNetworkRequest req;
	req.setSslConfiguration(d->ssl);
	req.setRawHeader("User-Agent", QString(QStringLiteral("%1/%2 (%3)"))
		.arg(qApp->applicationName(), qApp->applicationVersion(), Common::applicationOs()).toUtf8());
	req.setUrl(obj.value(QLatin1String("PICTURE-URL")).toString(QStringLiteral("https://sisene.www.eesti.ee/idportaal/portaal.idpilt")));

	QNetworkAccessManager *nam = new QNetworkAccessManager(this);
	connect(nam, &QNetworkAccessManager::sslErrors, this, [=](QNetworkReply *reply, const QList<QSslError> &errors){
		QList<QSslError> ignore;
		for(const QSslError &error: errors)
		{
			switch(error.error())
			{
			case QSslError::UnableToGetLocalIssuerCertificate:
			case QSslError::CertificateUntrusted:
			case QSslError::SelfSignedCertificateInChain:
				if(d->trusted.contains(reply->sslConfiguration().peerCertificate())) {
					ignore << error;
					break;
				}
			default: break;
			}
		}
		reply->ignoreSslErrors(ignore);
	});
	QNetworkReply *reply = nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, popup, reply] {
		qApp->signer()->logout();
		delete popup;
		if(reply->error() != QNetworkReply::NoError)
		{
			emit error(reply->errorString());
			return;
		}
		if(!reply->header(QNetworkRequest::ContentTypeHeader).toByteArray().contains("image/jpeg"))
		{
			emit error(QStringLiteral("Invalid Content-Type"));
			return;
		}
		QByteArray result = reply->readAll();
		reply->manager()->deleteLater();

		if(result.isEmpty())
		{
			emit error(QStringLiteral("Empty content"));
			return;
		}

		QImage img;
		if(!img.loadFromData(result))
		{
			emit error(QStringLiteral("Failed to parse image"));
			return;
		}
		emit image(img);
	});
}

SSLConnect* SSLConnect::instance()
{
	static SSLConnect sslConnect;
	return &sslConnect;
}
