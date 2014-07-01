import QtQml 2.0
import qf.core 1.0
import qf.qmlwidgets 1.0
import qf.qmlwidgets.framework 1.0

Plugin {
	id: root
	//featureId: 'SqlDb'
	//dependsOnFeatureIds: "Core"
	property SqlDatabase database: {
		Sql.addDatabase('QPSQL');
		Sql.database();
	}

	actions: [
		Action {
			id: actConnectDb
			text: qsTr('&Connect to database')
			//shortcut: "Ctrl+E"
			onTriggered: {
				Log.info(text, "triggered");
				connectToSqlServer(false);
			}
		}
	]

	property QfObject internals: QfObject
	{
		Component {
			id: dlgConnectDb
			DlgConnectDb {}
		}
	}

	Component.onCompleted:
	{
		//_Plugin_install();
		var quit = FrameWork.menuBar.actionForPath('file/quit', false);
		quit.prependAction(actConnectDb);
		quit.prependSeparator();

		FrameWork.pluginsLoaded.connect(postInstall);
	}

	function postInstall()
	{
		connectToSqlServer(false);
	}

	function connectToSqlServer(silent)
	{
		var cancelled = false;
		if(!silent) {
			var dlg = dlgConnectDb.createObject(FrameWork);
			cancelled = !dlg.exec();
			dlg.destroy();
		}
		if(!cancelled) {
			var core_feature = FrameWork.plugin("Core");
			var db = Sql.database();
			var settings = core_feature.createSettings();
			settings.beginGroup("sql/connection");
			db.hostName = settings.value('host');
			db.userName = settings.value('user');
			db.password = core_feature.crypt.decrypt(settings.value("password", ""));
			db.databaseName = 'quickevent';
			db.open();
			settings.destroy();
		}
	}
}
