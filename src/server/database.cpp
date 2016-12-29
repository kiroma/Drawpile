/*
   Drawpile - a collaborative drawing program.

   Copyright (C) 2016 Calle Laakkonen

   Drawpile is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Drawpile is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Drawpile.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "database.h"
#include "../shared/util/logger.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <QUrl>
#include <QRegularExpression>

namespace server {

struct Database::Private {
	QSqlDatabase db;
};

static bool initDatabase(QSqlDatabase db)
{
	QSqlQuery q(db);

	if(!q.exec(
		"CREATE TABLE IF NOT EXISTS settings (key PRIMARY KEY, value);"
		))
		return false;

	if(!q.exec(
		"CREATE TABLE IF NOT EXISTS listingservers (url);"
		))
		return false;

	return true;
}

Database::Database(QObject *parent) : ServerConfig(parent), d(new Private)
{
	d->db = QSqlDatabase::addDatabase("QSQLITE");
}

Database::~Database()
{
	delete d;
}

bool Database::openFile(const QString &path)
{
	d->db.setDatabaseName(path);
	if(!d->db.open()) {
		logger::warning() << "Unable to open database:" << path;
		return false;
	}

	if(!initDatabase(d->db)) {
		logger::warning() << "Database initialization failed:" << path;
		return false;
	}

	logger::info() << "Opened configuration database:" << path;
	return true;
}

void Database::setConfigString(ConfigKey key, const QString &value)
{
	QSqlQuery q(d->db);
	q.prepare("INSERT OR REPLACE INTO settings VALUES (?, ?)");
	q.bindValue(0, key.name);
	q.bindValue(1, value);
	q.exec();
}

QString Database::getConfigValue(const ConfigKey key, bool &found) const
{
	QSqlQuery q(d->db);
	q.prepare("SELECT value FROM settings WHERE key=?");
	q.bindValue(0, key.name);
	q.exec();
	if(q.next()) {
		found = true;
		return q.value(0).toString();
	} else {
		found = false;
		return QString();
	}
}

bool Database::isAllowedAnnouncementUrl(const QUrl &url)
{
	if(!url.isValid())
		return false;

	// If whitelisting is not enabled, allow all URLs
	if(!getConfigBool(config::AnnounceWhiteList))
		return true;

	const QString urlStr = url.toString();

	QSqlQuery q(d->db);
	q.exec("SELECT url FROM listingservers");
	while(q.next()) {
		const QString serverUrl = q.value(0).toString();
		const QRegularExpression re(serverUrl);
		if(!re.isValid()) {
			logger::warning() << "Invalid listingserver whitelist regular expression:" << serverUrl;
		} else {
			if(re.match(urlStr).hasMatch())
				return true;
		}
	}

	return false;
}

}