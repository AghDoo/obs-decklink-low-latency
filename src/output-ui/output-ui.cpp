#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <util/util.hpp>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVariant>
#include <QWidget>

#include "../decklink/const.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-decklink-low-latency-ui", "en-US")

namespace {

constexpr const char *outputId = OBS_DECKLINK_LOW_LATENCY_OUTPUT_ID;
constexpr const char *dockId = "obs-decklink-low-latency-output-dock";
constexpr const char *settingsFile = "program-output.json";
constexpr const char *autoStartKey = "auto_start";

void fillChoices(QComboBox &combo, obs_property_t *property, const QVariant &selected)
{
	const QSignalBlocker blocker(&combo);
	combo.clear();
	if (!property) {
		return;
	}

	const bool isString = obs_property_list_format(property) == OBS_COMBO_FORMAT_STRING;
	const auto count = obs_property_list_item_count(property);
	for (size_t i = 0; i < count; ++i) {
		if (obs_property_list_item_disabled(property, i)) {
			continue;
		}
		const auto label = QString::fromUtf8(obs_property_list_item_name(property, i));
		const QVariant value =
			isString ? QVariant(QString::fromUtf8(obs_property_list_item_string(property, i)))
				 : QVariant::fromValue<qlonglong>(obs_property_list_item_int(property, i));
		combo.addItem(label, value);
	}
	const int index = combo.findData(selected);
	if (index >= 0) {
		combo.setCurrentIndex(index);
	}
}

class OutputPanel final : public QWidget {
public:
	OutputPanel()
	{
		auto *form = new QFormLayout(this);
		form->addRow(QString::fromUtf8(obs_module_text("Device")), &device);
		form->addRow(QString::fromUtf8(obs_module_text("OutputMode")), &mode);
		form->addRow(&autoStart);
		status.setWordWrap(true);
		form->addRow(&status);
		form->addRow(&startButton, &stopButton);

		const BPtr<char> path = obs_module_config_path(settingsFile);
		if (path.Get()) {
			settings = obs_data_create_from_json_file_safe(path.Get(), "bak");
		}
		if (!settings) {
			settings = obs_data_create();
		}
		autoStart.setChecked(obs_data_get_bool(settings, autoStartKey));

		connect(&device, &QComboBox::currentIndexChanged, this, [this] { refreshModes(); });
		for (QComboBox *combo : {&device, &mode}) {
			connect(combo, &QComboBox::activated, this, [this] { saveSettings(); });
		}
		connect(&autoStart, &QCheckBox::toggled, this, [this] { saveSettings(); });
		connect(&startButton, &QPushButton::clicked, this, [this] { startOutput(); });
		connect(&stopButton, &QPushButton::clicked, this, [this] {
			stopButton.setEnabled(false);
			setStatus("Stopping");
			obs_output_stop(output);
		});
	}

	~OutputPanel() override { shutdown(); }

	void shutdown()
	{
		stoppedSignal.Disconnect();
		if (output && obs_output_active(output)) {
			obs_output_force_stop(output);
		}
		output = nullptr;
	}

	void frontendReady()
	{
		refresh();
		if (!autoStart.isChecked()) {
			return;
		}

		if (!startButton.isEnabled() || !obs_data_has_user_value(settings, MODE_ID) ||
		    device.currentData().toString() != QString::fromUtf8(obs_data_get_string(settings, DEVICE_HASH)) ||
		    mode.currentData().toLongLong() != obs_data_get_int(settings, MODE_ID)) {
			setStatus("AutoStartUnavailable");
			blog(LOG_WARNING,
			     "DeckLink Low Latency: auto-start skipped because the saved output device or mode is unavailable");
			return;
		}

		blog(LOG_INFO, "DeckLink Low Latency: starting Program output automatically");
		startOutput();
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		refresh();
	}

private:
	void refresh()
	{
		if (output) {
			return;
		}
		properties = obs_get_output_properties(outputId);
		fillChoices(device, obs_properties_get(properties, DEVICE_HASH),
			    QString::fromUtf8(obs_data_get_string(settings, DEVICE_HASH)));
		refreshModes();
	}

	void setStatus(const char *key) { status.setText(QString::fromUtf8(obs_module_text(key))); }

	void refreshModes()
	{
		const auto hash = device.currentData().toString().toUtf8();
		const auto selectedMode = mode.currentData().isValid()
						  ? mode.currentData()
						  : QVariant::fromValue<qlonglong>(obs_data_get_int(settings, MODE_ID));
		if (auto *deviceProperty = obs_properties_get(properties, DEVICE_HASH);
		    deviceProperty && !hash.isEmpty()) {
			const OBSDataAutoRelease selection = obs_data_create();
			obs_data_set_string(selection, DEVICE_HASH, hash.constData());
			obs_property_modified(deviceProperty, selection);
		}
		fillChoices(mode, hash.isEmpty() ? nullptr : obs_properties_get(properties, MODE_ID), selectedMode);
		updateControls();
	}

	void updateControls()
	{
		const bool idle = !output;
		device.setEnabled(idle && device.count() > 0);
		mode.setEnabled(idle && mode.count() > 0);
		startButton.setEnabled(idle && device.currentIndex() >= 0 && mode.currentIndex() >= 0);
		stopButton.setEnabled(!idle);
		if (!idle) {
			setStatus("Running");
		} else if (!properties) {
			setStatus("ModuleUnavailable");
		} else if (device.count() == 0) {
			setStatus("NoDevice");
		} else if (mode.count() == 0) {
			setStatus("NoMode");
		} else {
			setStatus("Ready");
		}
	}

	void saveSettings()
	{
		if (device.currentIndex() >= 0 && mode.currentIndex() >= 0) {
			obs_data_set_string(settings, DEVICE_HASH,
					    device.currentData().toString().toUtf8().constData());
			obs_data_set_int(settings, MODE_ID, mode.currentData().toLongLong());
		}
		obs_data_set_bool(settings, autoStartKey, autoStart.isChecked());
		const BPtr<char> path = obs_module_config_path(settingsFile);
		if (path.Get() && QDir().mkpath(QFileInfo(QString::fromUtf8(path.Get())).absolutePath())) {
			if (!obs_data_save_json_safe(settings, path.Get(), "tmp", "bak")) {
				blog(LOG_WARNING, "Could not save DeckLink Low Latency output settings");
			}
		}
	}

	void startOutput()
	{
		saveSettings();
		output = obs_output_create(outputId, outputId, settings, nullptr);
		if (output) {
			stoppedSignal.Connect(obs_output_get_signal_handler(output), "stop", onOutputStopped, this);
			if (obs_output_start(output)) {
				updateControls();
				return;
			}
		}
		stoppedSignal.Disconnect();
		output = nullptr;
		setStatus("StartFailed");
	}

	static void onOutputStopped(void *data, calldata_t *event)
	{
		auto *panel = static_cast<OutputPanel *>(data);
		const bool success = calldata_int(event, "code") == OBS_OUTPUT_SUCCESS;
		QMetaObject::invokeMethod(
			panel,
			[panel, success] {
				panel->stoppedSignal.Disconnect();
				panel->output = nullptr;
				panel->refresh();
				if (!success) {
					panel->setStatus("OutputFailed");
				}
			},
			Qt::QueuedConnection);
	}

	QComboBox device;
	QComboBox mode;
	QCheckBox autoStart{QString::fromUtf8(obs_module_text("AutoStart"))};
	QLabel status;
	QPushButton startButton{QString::fromUtf8(obs_module_text("Start"))};
	QPushButton stopButton{QString::fromUtf8(obs_module_text("Stop"))};
	OBSDataAutoRelease settings;
	OBSProperties properties;
	OBSOutputAutoRelease output;
	OBSSignal stoppedSignal;
};

QPointer<QAction> menuAction;
QPointer<OutputPanel> outputPanel;

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (!outputPanel) {
		return;
	}
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		outputPanel->frontendReady();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		outputPanel->shutdown();
	}
}

} // namespace

bool obs_module_load(void)
{
	return true;
}

void obs_module_post_load(void)
{
	const char *title = obs_module_text("MenuTitle");
	auto *panel = new OutputPanel();
	if (!obs_frontend_add_dock_by_id(dockId, title, panel)) {
		delete panel;
		return;
	}
	outputPanel = panel;
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

	menuAction = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(title));
	if (menuAction) {
		QObject::connect(menuAction, &QAction::triggered, panel, [panel] {
			auto *dock = qobject_cast<QDockWidget *>(panel->parentWidget());
			if (dock) {
				dock->show();
				dock->raise();
			}
		});
	}
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	delete menuAction.data();
	if (outputPanel) {
		obs_frontend_remove_dock(dockId);
	}
}
