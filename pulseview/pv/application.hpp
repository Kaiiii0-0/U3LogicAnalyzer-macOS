/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
 *
 * Copyright (C) 2014 Martin Ling <martin-sigrok@earth.li>
 * Copyright (C) 2026 Q2H2
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PULSEVIEW_PV_APPLICATION_HPP
#define PULSEVIEW_PV_APPLICATION_HPP

#include <vector>

#include <QApplication>
#include <QStringList>
#include <QTranslator>
#include <QProcess>
#include <QLocalServer>
#include <libsigrokcxx/libsigrokcxx.hpp>

#include "devicemanager.hpp"
#include "globalsettings.hpp"

using std::shared_ptr;
using std::pair;
using std::vector;

class Application : public QApplication, public pv::GlobalSettingsInterface
{
	Q_OBJECT

public:
	Application(int &argc, char* argv[]);

	const QStringList get_languages() const;
	const QString get_language_editors(const QString& language) const;
	void switch_language(const QString& language);

	/*
	 * Path of the local socket a running instance listens on, so another
	 * process (u3ctl, or a second invocation of this program) can hand it
	 * a capture to open instead of starting yet another instance.
	 */
	static QString ipc_socket_name();

	/*
	 * Hand a capture path to the instance that is already running, if any.
	 * Returns true when it was delivered there; false when no instance is
	 * listening, and the caller should go on to load the file itself.
	 */
	static bool forward_to_running_gui(const QString &path);

Q_SIGNALS:
	/*
	 * A capture path arrived over the IPC channel, or as a macOS
	 * open-document event, and wants to be opened as a new session.
	 */
	void fileOpenRequested(const QString &path);

public:
	bool event(QEvent *event) override;

	void on_setting_changed(const QString &key, const QVariant &value);

	void collect_version_info(pv::DeviceManager &device_manager);
	void print_version_info();

	vector< pair<QString, QString> > get_version_info() const;
	vector<QString> get_fw_path_list() const;
	vector<QString> get_pd_path_list() const;
	vector< pair<QString, QString> > get_driver_list() const;
	vector< pair<QString, QString> > get_input_format_list() const;
	vector< pair<QString, QString> > get_output_format_list() const;
	vector< pair<QString, QString> > get_pd_list() const;

	void set_hardware_version(uint64_t hardware_version);
	void renew_version_info();

private:
	bool notify(QObject *receiver, QEvent *event);

	void start_ipc_server();
	void on_ipc_new_connection();

	QLocalServer *ipc_server_ = nullptr;
	QHash<QLocalSocket *, QByteArray> ipc_buf_;

	vector< pair<QString, QString> > version_info_;
	vector<QString> fw_path_list_;
	vector<QString> pd_path_list_;
	vector< pair<QString, QString> > driver_list_;
	vector< pair<QString, QString> > input_format_list_;
	vector< pair<QString, QString> > output_format_list_;
	vector< pair<QString, QString> > pd_list_;
	QProcess * UpdateFwVersion_;
	QTranslator app_translator_, qt_translator_, qtbase_translator_;
	uint64_t hd_version_ = 0;
};

#endif // PULSEVIEW_PV_APPLICATION_HPP
