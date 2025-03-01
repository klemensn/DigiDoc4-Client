/*
 * QDigiDoc4
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

#include "SslCertificate.h"

#include "Common.h"

#include <digidocpp/Exception.h>
#include <digidocpp/crypto/X509Cert.h>

#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QMap>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSslKey>

#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/pkcs12.h>
#include <openssl/x509v3.h>

#include <memory>

#define SCOPE(TYPE, DATA) std::unique_ptr<TYPE,decltype(&TYPE##_free)>(static_cast<TYPE*>(DATA), TYPE##_free)
#define toQByteArray(x) QByteArray((const char*)x->data, x->length)
template <typename Func, typename Arg>
static QByteArray i2dDer(Func func, Arg arg)
{
	QByteArray der;
	if(!arg)
		return der;
	der.resize(func(arg, nullptr));
	if(der.isEmpty())
		return der;
	unsigned char *p = (unsigned char*)der.data();
	if(func(arg, &p) != der.size())
		der.clear();
	return der;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
size_t qHash(const SslCertificate &cert) { return qHash(cert.digest()); }
#else
uint qHash(const SslCertificate &cert) { return qHash(cert.digest()); }
#endif

SslCertificate::SslCertificate() = default;

SslCertificate::SslCertificate( const QByteArray &data, QSsl::EncodingFormat format )
: QSslCertificate( data, format ) {}

SslCertificate::SslCertificate( const QSslCertificate &cert )
: QSslCertificate( cert ) {}

QString SslCertificate::issuerInfo( const QByteArray &tag ) const
{ return QSslCertificate::issuerInfo(tag).join(' '); }

QString SslCertificate::issuerInfo( QSslCertificate::SubjectInfo subject ) const
 { return QSslCertificate::issuerInfo(subject).join(' '); }

QString SslCertificate::subjectInfo( const QByteArray &tag ) const
 { return QSslCertificate::subjectInfo(tag).join(' '); }

QString SslCertificate::subjectInfo( QSslCertificate::SubjectInfo subject ) const
{ return QSslCertificate::subjectInfo(subject).join(' '); }

QMultiHash<SslCertificate::AuthorityInfoAccess, QString> SslCertificate::authorityInfoAccess() const
{
	QMultiHash<AuthorityInfoAccess, QString> result;
	auto info = SCOPE(AUTHORITY_INFO_ACCESS, extension(NID_info_access));
	if(!info)
		return result;
	for(int i = 0; i < sk_ACCESS_DESCRIPTION_num(info.get()); ++i)
	{
		ACCESS_DESCRIPTION *ad = sk_ACCESS_DESCRIPTION_value(info.get(), i);
		if(ad->location->type != GEN_URI)
			continue;
		switch(OBJ_obj2nid(ad->method))
		{
		case NID_ad_OCSP:
#if QT_VERSION > QT_VERSION_CHECK(5, 14, 0)
			result.insert(ad_OCSP, toQByteArray(ad->location->d.uniformResourceIdentifier));
#else
			result.insertMulti(ad_OCSP, toQByteArray(ad->location->d.uniformResourceIdentifier));
#endif
			break;
		case NID_ad_ca_issuers:
#if QT_VERSION > QT_VERSION_CHECK(5, 14, 0)
			result.insert(ad_CAIssuers, toQByteArray(ad->location->d.uniformResourceIdentifier));
#else
			result.insertMulti(ad_CAIssuers, toQByteArray(ad->location->d.uniformResourceIdentifier));
#endif
			break;
		default: break;
		}
	}
	return result;
}

QByteArray SslCertificate::authorityKeyIdentifier() const
{
	auto id = SCOPE(AUTHORITY_KEYID, extension(NID_authority_key_identifier));
	return id && id->keyid ? toQByteArray(id->keyid) : QByteArray();
}

QHash<SslCertificate::EnhancedKeyUsage,QString> SslCertificate::enhancedKeyUsage() const
{
	QHash<EnhancedKeyUsage,QString> list;
	auto usage = SCOPE(EXTENDED_KEY_USAGE, extension(NID_ext_key_usage));
	if( !usage )
	{
		list[All] = tr("All application policies");
		return list;
	}

	for(int i = 0; i < sk_ASN1_OBJECT_num(usage.get()); ++i)
	{
		ASN1_OBJECT *obj = sk_ASN1_OBJECT_value(usage.get(), i);
		switch( OBJ_obj2nid( obj ) )
		{
		case NID_client_auth:
			list[ClientAuth] = tr("Proves your identity to a remote computer"); break;
		case NID_server_auth:
			list[ServerAuth] = tr("Ensures the identity of a remote computer"); break;
		case NID_email_protect:
			list[EmailProtect] = tr("Protects email messages"); break;
		case NID_OCSP_sign:
			list[OCSPSign] = tr("OCSP signing"); break;
		case NID_time_stamp:
			list[TimeStamping] = tr("Time Stamping"); break;
		default: break;
		}
	}
	return list;
}

bool SslCertificate::isCA() const
{
	auto cons = SCOPE(BASIC_CONSTRAINTS, extension(NID_basic_constraints));
	return cons && cons->ca > 0;
}

QString SslCertificate::keyName() const
{
	switch(publicKey().algorithm())
	{
	case QSsl::Dsa:
		return QStringLiteral("DSA (%1)").arg( publicKey().length() );
	case QSsl::Rsa:
		return QStringLiteral("RSA (%1)").arg( publicKey().length() );
	default:
#ifndef OPENSSL_NO_ECDSA
		if(X509 *c = (X509*)handle())
		{
			EVP_PKEY *key = X509_get0_pubkey(c);
			const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(key);
			int nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(ec));
			ASN1_OBJECT *obj = OBJ_nid2obj(nid);
			QByteArray buff(50, 0);
			if(OBJ_obj2txt(buff.data(), buff.size(), obj, 0) > 0)
				return buff;
		}
#endif
	}
	return tr("Unknown");
}

Qt::HANDLE SslCertificate::extension( int nid ) const
{
	return handle() ? Qt::HANDLE(X509_get_ext_d2i((X509*)handle(), nid, nullptr, nullptr)) : nullptr;
}

QHash<SslCertificate::KeyUsage,QString> SslCertificate::keyUsage() const
{
	QHash<KeyUsage,QString> list;
	auto keyusage = SCOPE(ASN1_BIT_STRING, extension(NID_key_usage));
	if(!keyusage)
		return list;
	for( int n = 0; n < 9; ++n )
	{
		if(!ASN1_BIT_STRING_get_bit(keyusage.get(), n))
			continue;
		switch( n )
		{
		case DigitalSignature: list[KeyUsage(n)] = tr("Digital signature"); break;
		case NonRepudiation: list[KeyUsage(n)] = tr("Non repudiation"); break;
		case KeyEncipherment: list[KeyUsage(n)] = tr("Key encipherment"); break;
		case DataEncipherment: list[KeyUsage(n)] = tr("Data encipherment"); break;
		case KeyAgreement: list[KeyUsage(n)] = tr("Key agreement"); break;
		case KeyCertificateSign: list[KeyUsage(n)] = tr("Key certificate sign"); break;
		case CRLSign: list[KeyUsage(n)] = tr("CRL sign"); break;
		case EncipherOnly: list[KeyUsage(n)] = tr("Encipher only"); break;
		case DecipherOnly: list[KeyUsage(n)] = tr("Decipher only"); break;
		default: break;
		}
	}
	return list;
}

QString SslCertificate::personalCode() const
{
	// http://www.etsi.org/deliver/etsi_en/319400_319499/31941201/01.01.01_60/en_31941201v010101p.pdf
	static const QStringList types {"PAS", "IDC", "PNO", "TAX", "TIN"};
	QString data = subjectInfo(QSslCertificate::SerialNumber);
	if(data.size() > 6 && (types.contains(data.left(3)) || data[2] == ':') && data[5] == '-')
		return data.mid(6);
	if(!data.isEmpty())
		return data;
	return QString(serialNumber()).remove(':');
}

QStringList SslCertificate::policies() const
{
	auto cp = SCOPE(CERTIFICATEPOLICIES, extension(NID_certificate_policies));
	QStringList list;
	if( !cp )
		return list;

	for(int i = 0; i < sk_POLICYINFO_num(cp.get()); ++i)
	{
		POLICYINFO *pi = sk_POLICYINFO_value(cp.get(), i);
		QByteArray buf(50, 0);
		int len = OBJ_obj2txt(buf.data(), buf.size(), pi->policyid, 1);
		if( len != NID_undef )
			list << buf;
	}
	return list;
}

bool SslCertificate::showCN() const
{ return subjectInfo( "GN" ).isEmpty() && subjectInfo( "SN" ).isEmpty(); }

QString SslCertificate::signatureAlgorithm() const
{
	if(!handle())
		return {};
	const X509_ALGOR *algo = nullptr;
	X509_get0_signature(nullptr, &algo, (X509*)handle());
	QByteArray buf(50, 0);
	i2t_ASN1_OBJECT(buf.data(), buf.size(), algo->algorithm);
	return buf;
}

QByteArray SslCertificate::subjectKeyIdentifier() const
{
	auto id = SCOPE(ASN1_OCTET_STRING, extension(NID_subject_key_identifier));
	return !id ? QByteArray() : toQByteArray(id);
}

QByteArray SslCertificate::toHex( const QByteArray &in, QChar separator )
{
#if QT_VERSION > QT_VERSION_CHECK(5, 14, 0)
	QString ret = {in.toHex().toUpper()};
	for( int i = 2; i < ret.size(); i += 3 )
		ret.insert( i, separator );
	return ret.toUtf8();
#else
	QByteArray ret = in.toHex().toUpper();
	for( int i = 2; i < ret.size(); i += 3 )
		ret.insert( i, separator );
	return ret;
#endif
}

QString SslCertificate::toString( const QString &format ) const
{
	QRegularExpression r(QStringLiteral("[a-zA-Z]+"));
	QString ret = format;
	QRegularExpressionMatch match;
	int pos = 0;
	while((match = r.match(ret, pos)).hasMatch()) {
		QString cap = match.captured();
		QString si = cap == QStringLiteral("serialNumber") ? personalCode() : subjectInfo(cap.toLatin1());
		ret.replace(match.capturedStart(), cap.size(), si);
		pos = match.capturedStart() + si.size();
	}
	return ret;
}

SslCertificate::CertType SslCertificate::type() const
{
	for(const QString &p: policies())
	{
		if(p.startsWith(QLatin1String("1.3.6.1.4.1.10015.1.1")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.10015.3.1")))
			return EstEidType;
		if(p.startsWith(QLatin1String("1.3.6.1.4.1.10015.1.2")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.10015.3.2")))
			return subjectInfo(QSslCertificate::Organization).contains(QStringLiteral("E-RESIDENT")) ? EResidentType : DigiIDType;
		if(p.startsWith(QLatin1String("1.3.6.1.4.1.10015.1.3")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.10015.11.1")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.10015.3.3")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.10015.3.11")))
			return MobileIDType;
		if(p.startsWith(QLatin1String("1.3.6.1.4.1.10015.7.1")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.10015.7.3")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.10015.2.1")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.10015.3.7")))
			return TempelType;
		if(p.startsWith(QLatin1String("1.3.6.1.4.1.51361.1.1.3")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.51361.1.2.3")))
			return DigiIDType;
		if(p.startsWith(QLatin1String("1.3.6.1.4.1.51361.1.1.4")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.51361.1.2.4")))
			return EResidentType;
		if(p.startsWith(QLatin1String("1.3.6.1.4.1.51361.1.1")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.51455.1.1")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.51361.1.2")) ||
			p.startsWith(QLatin1String("1.3.6.1.4.1.51455.1.2")))
			return EstEidType;
	}

	// Check qcStatements extension according to ETSI EN 319 412-5
	QByteArray der = toDer();
	if (!der.isNull())
	{
		try {
			digidoc::X509Cert x509Cert((const unsigned char*)der.constData(),
									   size_t(der.size()), digidoc::X509Cert::Der);
			for(const std::string &statement: x509Cert.qcStatements())
			{
				if(statement == digidoc::X509Cert::QCT_ESEAL)
					return TempelType;
			}
		} catch (const digidoc::Exception &e) {
			qWarning() << "digidoc::X509Cert error:" << QString::fromStdString(e.msg());
		}
	}
	return UnknownType;
}

SslCertificate::Validity SslCertificate::validateOnline() const
{
	QMultiHash<SslCertificate::AuthorityInfoAccess,QString> urls = authorityInfoAccess();
	if(urls.isEmpty())
		return Unknown;

	QEventLoop e;
	QNetworkAccessManager m;
	QNetworkAccessManager::connect(&m, &QNetworkAccessManager::finished, &e, &QEventLoop::quit);
	QNetworkAccessManager::connect(&m, &QNetworkAccessManager::sslErrors, &m,
		[](QNetworkReply *reply, const QList<QSslError> &errors){
		reply->ignoreSslErrors(errors);
	});

	// Get issuer
	QNetworkRequest r(urls.values(SslCertificate::ad_CAIssuers).first());
	r.setRawHeader("User-Agent", QStringLiteral("%1/%2 (%3)")
		.arg(qApp->applicationName(), qApp->applicationVersion(), Common::applicationOs()).toUtf8());
	QNetworkReply *repl = m.get(r);
	e.exec();
	QSslCertificate issuer(repl->readAll(), QSsl::Der);
	repl->deleteLater();
	if(issuer.isNull())
		return Unknown;

	// Build request
	auto ocspReq = SCOPE(OCSP_REQUEST, OCSP_REQUEST_new());
	if(!ocspReq)
		return Unknown;
	OCSP_CERTID *certId = OCSP_cert_to_id(nullptr, (X509*)handle(), (X509*)issuer.handle());
	if(!OCSP_request_add0_id(ocspReq.get(), certId))
		return Unknown;

	// Send request
	r.setUrl(urls.values(SslCertificate::ad_OCSP).first());
	r.setHeader(QNetworkRequest::ContentTypeHeader, "application/ocsp-request");
	repl = m.post(r, i2dDer(i2d_OCSP_REQUEST, ocspReq.get()));
	e.exec();

	// Parse response
	QByteArray respData = repl->readAll();
	repl->deleteLater();
	const unsigned char *p = (const unsigned char*)respData.constData();
	auto resp = SCOPE(OCSP_RESPONSE, d2i_OCSP_RESPONSE(nullptr, &p, respData.size()));
	if(!resp || OCSP_response_status(resp.get()) != OCSP_RESPONSE_STATUS_SUCCESSFUL)
		return Unknown;

	// Validate response
	auto basic = SCOPE(OCSP_BASICRESP, OCSP_response_get1_basic(resp.get()));
	if(!basic)
		return Unknown;
	//OCSP_TRUSTOTHER - enables OCSP_NOVERIFY
	//OCSP_NOSIGS - does not verify ocsp signatures
	//OCSP_NOVERIFY - ignores signer(responder) cert verification, requires store otherwise crashes
	//OCSP_NOCHECKS - cancel futurer responder issuer checks and trust bits
	//OCSP_NOEXPLICIT - returns 0 by mistake
	//all checks enabled fails trust bit check, cant use OCSP_NOEXPLICIT instead using OCSP_NOCHECKS
	if(OCSP_basic_verify(basic.get(), nullptr, nullptr, OCSP_NOVERIFY) <= 0)
		return Unknown;
	int status = -1;
	if(OCSP_resp_find_status(basic.get(), certId, &status, nullptr, nullptr, nullptr, nullptr) <= 0)
		return Unknown;
	return Validity(status);
}



class PKCS12Certificate::Private final: public QSharedData
{
public:
	void setLastError()
	{
		error = PKCS12Certificate::NullError;
		errorString.clear();
		while( ERR_peek_error() > ERR_LIB_NONE )
		{
			unsigned long err = ERR_get_error();
			if( ERR_GET_LIB(err) == ERR_LIB_PKCS12 &&
				ERR_GET_REASON(err) == PKCS12_R_MAC_VERIFY_FAILURE )
			{
				error = PKCS12Certificate::InvalidPasswordError;
				return;
			}
			error = PKCS12Certificate::UnknownError;
			errorString += ERR_error_string(err, nullptr);
		}
	}

	QSslCertificate cert;
	PKCS12Certificate::ErrorType error = PKCS12Certificate::NullError;
	QString errorString;
};

PKCS12Certificate::PKCS12Certificate( QIODevice *device, const QString &pin )
	: PKCS12Certificate(device ? device->readAll() : QByteArray(), pin)
{}

PKCS12Certificate::PKCS12Certificate( const QByteArray &data, const QString &pin )
	: d(new Private)
{
	if(data.isEmpty())
		return;
	const unsigned char *p = (const unsigned char*)data.constData();
	PKCS12 *p12 = d2i_PKCS12(nullptr, &p, data.size());
	if(!p12)
	{
		d->setLastError();
		return;
	}

	X509 *c = nullptr;
	EVP_PKEY *k = nullptr;
	QByteArray _pin = pin.toUtf8();
	int ret = PKCS12_parse(p12, _pin.constData(), &k, &c, nullptr);
	PKCS12_free(p12);
	if(!ret)
	{
		d->setLastError();
		return;
	}
	ERR_get_error();

	d->cert = QSslCertificate(i2dDer(i2d_X509, c), QSsl::Der);

	X509_free(c);
	EVP_PKEY_free(k);
}

PKCS12Certificate::PKCS12Certificate( const PKCS12Certificate &other ) = default;
PKCS12Certificate::PKCS12Certificate(PKCS12Certificate &&other) Q_DECL_NOEXCEPT = default;
PKCS12Certificate& PKCS12Certificate::operator=(const PKCS12Certificate &other) = default;
PKCS12Certificate& PKCS12Certificate::operator=(PKCS12Certificate &&other) Q_DECL_NOEXCEPT = default;

PKCS12Certificate::~PKCS12Certificate() = default;
QSslCertificate PKCS12Certificate::certificate() const { return d->cert; }
PKCS12Certificate::ErrorType PKCS12Certificate::error() const { return d->error; }
QString PKCS12Certificate::errorString() const { return d->errorString; }

PKCS12Certificate PKCS12Certificate::fromPath( const QString &path, const QString &pin )
{
	PKCS12Certificate p12(nullptr, {});
	QFile f( path );
	if( !f.exists() )
		p12.d->error = PKCS12Certificate::FileNotExist;
	else if( !f.open( QFile::ReadOnly ) )
		p12.d->error = PKCS12Certificate::FailedToRead;
	else
		return {&f, pin};
	return p12;
}

bool PKCS12Certificate::isNull() const { return d->cert.isNull(); }
