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

#pragma once

#include "common_enums.h"
#include "widgets/Item.h"

class DigiDocSignature;

class SignatureItem final : public Item
{
	Q_OBJECT

public:
	explicit SignatureItem(DigiDocSignature s, ria::qdigidoc4::ContainerState state, QWidget *parent = nullptr);
	~SignatureItem() override;

	ria::qdigidoc4::WarningType getError() const;
	QString id() const override;
	QWidget* initTabOrder(QWidget *item) final;
	bool isInvalid() const;
	bool isSelfSigned(const QString& cardCode, const QString& mobileCode) const;

public slots:
	void details() final;

protected:
	bool event(QEvent *event) final;
	bool eventFilter(QObject *o, QEvent *e) final;

private:
	void init();
	QString red(const QString &text, bool paint = true) const;
	void removeSignature();
	void updateNameField();

	class Private;
	Private *ui;
};
