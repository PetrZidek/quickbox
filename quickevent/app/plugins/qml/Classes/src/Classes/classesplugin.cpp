#include "classesplugin.h"
#include "../thispartwidget.h"
#include "classdocument.h"

#include "../coursedef.h"

#include <Event/eventplugin.h>

#include <qf/qmlwidgets/framework/mainwindow.h>
#include <qf/qmlwidgets/dialogs/messagebox.h>
#include <qf/qmlwidgets/action.h>

#include <qf/core/log.h>
#include <qf/core/assert.h>
#include <qf/core/exception.h>
#include <qf/core/sql/query.h>
#include <qf/core/sql/querybuilder.h>
#include <qf/core/sql/transaction.h>

#include <QQmlEngine>

namespace qfw = qf::qmlwidgets;
namespace qff = qf::qmlwidgets::framework;
//namespace qfd = qf::qmlwidgets::dialogs;
namespace qfs = qf::core::sql;

using namespace Classes;

static Event::EventPlugin* eventPlugin()
{
	qf::qmlwidgets::framework::MainWindow *fwk = qf::qmlwidgets::framework::MainWindow::frameWork();
	auto *plugin = qobject_cast<Event::EventPlugin*>(fwk->plugin("Event"));
	QF_ASSERT_EX(plugin != nullptr, "Bad event plugin!");
	return plugin;
}

ClassesPlugin::ClassesPlugin(QObject *parent)
	: Super(parent)
{
	connect(this, &ClassesPlugin::installed, this, &ClassesPlugin::onInstalled, Qt::QueuedConnection);
}

void ClassesPlugin::onInstalled()
{
	qff::MainWindow *fwk = qff::MainWindow::frameWork();
	m_partWidget = new ThisPartWidget();
	fwk->addPartWidget(m_partWidget, manifest()->featureId());

	emit nativeInstalled();
}

QObject *ClassesPlugin::createClassDocument(QObject *parent)
{
	ClassDocument *ret = new ClassDocument(parent);
	if(!parent) {
		qfWarning() << "Parent is NULL, created class will have QQmlEngine::JavaScriptOwnership.";
		qmlEngine()->setObjectOwnership(ret, QQmlEngine::JavaScriptOwnership);
	}
	return ret;
}

void ClassesPlugin::createClass(const QString &class_name)
{
	//qf::core::sql::Transaction transaction;
	ClassDocument doc;
	doc.loadForInsert();
	doc.setValue(QStringLiteral("classes.name"), class_name);
	doc.save();
	//transaction.commit();
}

void ClassesPlugin::dropClass(int class_id)
{
	QF_ASSERT_EX(class_id > 0, "Bad classes.id value.");
	//qfInfo() << "Dropping class id:" << class_id;
	ClassDocument doc;
	doc.load(class_id, ClassDocument::RecordEditMode::ModeDelete);
	doc.drop();
}

void ClassesPlugin::createCourses(int stage_id, const QVariantList &courses)
{
	qfLogFuncFrame() << courses;
	try {
		bool is_relays = eventPlugin()->eventConfig()->isRelays();
		qf::core::sql::Transaction transaction(qf::core::sql::Connection::forName());
		qf::core::sql::Query q;
		deleteCourses(stage_id);

		QSet<int> all_codes;
		QMap<QString, int> course_ids;
		QMap<int, QList<int> > course_codes;
		{
			// if classes are not imported from Oris, import also classes
			q.exec("SELECT COUNT(*) FROM classes", qf::core::Exception::Throw);
			if(q.next() && q.value(0).toInt() == 0) {
				QSet<QString> class_names;
				for(auto v : courses) {
					CourseDef cd(v.toMap());
					for(auto class_name : cd.classes())
						class_names << class_name;
				}
				ClassDocument doc;
				for(auto class_name : class_names) {
					qfInfo() << "inserting class" << class_name;
					doc.loadForInsert();
					doc.setValue("name", class_name);
					doc.save();
				}
			}
		}
		QMap<QString, int> class_ids;
		{
			q.exec("SELECT id, name FROM classes");
			while(q.next()) {
				class_ids[q.value("name").toString()] = q.value("id").toInt();
			}
		}
		for(auto v : courses) {
			CourseDef cd(v.toMap());
			int course_id = 0;
			{
				qfInfo() << "inserting course" << cd.name() << "stage:" << stage_id << "classes:" << cd.classes().join(',');
				QString qs = "INSERT INTO courses (name, length, climb, note) VALUES (:name, :length, :climb, :note)";
				q.prepare(qs, qf::core::Exception::Throw);
				q.bindValue(":name", cd.name());
				q.bindValue(":length", cd.lenght());
				q.bindValue(":climb", cd.climb());
				q.bindValue(":note", QString("E%1 ").arg(stage_id) + cd.classes().join(','));
				q.exec(qf::core::Exception::Throw);
				course_id = q.lastInsertId().toInt();
			}
			if(!is_relays) {
				QString qs = "UPDATE classdefs SET courseId=:courseId WHERE classId=:classId AND stageId=:stageId";
				q.prepare(qs, qf::core::Exception::Throw);
				for(auto class_name : cd.classes()) {
					int class_id = class_ids.value(class_name);
					if(class_id > 0) {
						qfInfo() << "\t" << "updating classdefs for" << class_name << "stage:" << stage_id;
						q.bindValue(":classId", class_id);
						q.bindValue(":courseId", course_id);
						q.bindValue(":stageId", stage_id);
						q.exec(qf::core::Exception::Throw);
					}
					else {
						qfError() << class_name << "not found in defined classes";
					}
				}
			}
			course_ids[cd.name()] = course_id;
			for(auto v : cd.codes()) {
				int code = v.toInt();
				all_codes << code;
				course_codes[course_id] << code;
			}
		}
		if(is_relays) {
			/// guess first relay number and number of legs in each class
			struct RelNoLegs {
				int relayNo = std::numeric_limits<int>::max();
				int legCnt = 0;
				bool isValid() const {return relayNo < std::numeric_limits<int>::max() && legCnt > 0;}
			};
			QMap<QString, RelNoLegs> relnolegs;
			for(auto v : courses) {
				CourseDef cd(v.toMap());
				if(cd.classes().count() == 1) {
					QString name = cd.name();
					int num = name.section('.', 0, 0).toInt();
					int leg = name.section('.', 1, 1).toInt();
					if(num > 0 && leg > 0) {
						RelNoLegs &rnl = relnolegs[cd.classes().value(0)];
						rnl.relayNo = qMin(rnl.relayNo, num);
						rnl.legCnt = qMax(rnl.legCnt, leg);
					}
					else {
						qfWarning() << "Cannot deduce relay number and leg for course:" << cd.name() << "classes:" << cd.classes().join(',');
					}
				}
				else {
					qfWarning() << "Cannot deduce class name for course:" << cd.name() << "classes:" << cd.classes().join(',');
				}
			}
			QString qs = "UPDATE classdefs SET relayStartNumber=:relayStartNumber, relayLegCount=:relayLegCount WHERE classId=:classId AND stageId=:stageId";
			q.prepare(qs, qf::core::Exception::Throw);
			QMapIterator<QString, RelNoLegs> it(relnolegs);
			while(it.hasNext()) {
				it.next();
				const QString class_name = it.key();
				if(it.value().isValid()) {
					int class_id = class_ids.value(class_name);
					if(class_id > 0) {
						qfInfo() << "\t" << "updating classdefs for" << class_name << "stage:" << stage_id
								 << "relayStartNumber:" << it.value().relayNo << "relayLegCount:" << it.value().legCnt;
						q.bindValue(":relayStartNumber", it.value().relayNo);
						q.bindValue(":relayLegCount", it.value().legCnt);
						q.bindValue(":classId", class_id);
						q.bindValue(":stageId", stage_id);
						q.exec(qf::core::Exception::Throw);
					}
					else {
						qfError() << class_name << "not found in defined classes";
					}
				}
			}
		}
		QMap<int, int> code_to_id;
		{
			QString qs = "INSERT INTO codes (code, note) VALUES (:code, :note)";
			q.prepare(qs, qf::core::Exception::Throw);
			for(auto code : all_codes) {
				qfDebug() << "inserting code" << code;
				q.bindValue(":code", code);
				q.bindValue(":note", QString("E%1").arg(stage_id));
				q.exec(qf::core::Exception::Throw);
				code_to_id[code] = q.lastInsertId().toInt();
			}
		}
		{
			QString qs = "INSERT INTO coursecodes (courseId, position, codeId) VALUES (:courseId, :position, :codeId)";
			q.prepare(qs, qf::core::Exception::Throw);
			QMapIterator<int, QList<int> > it(course_codes);
			while(it.hasNext()) {
				it.next();
				int pos = 0;
				for(auto code : it.value()) {
					int code_id = code_to_id.value(code);
					if(code_id > 0) {
						qfDebug() << "courseId" << it.key() << "-> code:" << code << "codeId:" << code_id;
						q.bindValue(":courseId", it.key());
						q.bindValue(":position", ++pos);
						q.bindValue(":codeId", code_id);
						q.exec(qf::core::Exception::Throw);
					}
					else {
						QF_EXCEPTION(tr("Cannot find id for code: %1").arg(code));
					}
				}
			}
		}
		transaction.commit();
	}
	catch (const qf::core::Exception &e) {
		qf::qmlwidgets::framework::MainWindow *fwk = qf::qmlwidgets::framework::MainWindow::frameWork();
		qf::qmlwidgets::dialogs::MessageBox::showException(fwk, e);
	}
}

void ClassesPlugin::deleteCourses(int stage_id)
{
	qf::core::sql::Query q;
	q.exec("UPDATE classdefs SET courseId=NULL WHERE stageId=" QF_IARG(stage_id), qf::core::Exception::Throw);
	gcCourses();
}

void ClassesPlugin::gcCourses()
{
	qf::core::sql::Query q;
	q.exec("DELETE FROM coursecodes WHERE id IN ("
		"SELECT coursecodes.id FROM coursecodes"
		   " LEFT JOIN courses ON coursecodes.courseId=courses.id"
		   " LEFT JOIN classdefs ON classdefs.courseId=courses.id"
		   " WHERE classdefs.Id IS NULL"
		")", qf::core::Exception::Throw);
	q.exec("DELETE FROM courses WHERE id IN ("
		"SELECT courses.id FROM courses LEFT JOIN classdefs ON classdefs.courseId=courses.id WHERE classdefs.Id IS NULL"
		")", qf::core::Exception::Throw);
	q.exec("DELETE FROM codes WHERE id IN ("
		"SELECT codes.id FROM codes LEFT JOIN coursecodes ON coursecodes.codeId=codes.id WHERE coursecodes.Id IS NULL"
		")", qf::core::Exception::Throw);
}

