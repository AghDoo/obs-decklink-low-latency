#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <util/util.hpp>

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVariant>

#include "../decklink/const.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-decklink-low-latency-ui", "en-US")

namespace {

constexpr const char *outputId = OBS_DECKLINK_LOW_LATENCY_OUTPUT_ID;
constexpr const char *settingsFile = "program-output.json";

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

class OutputDialog final : public QDialog {
public:
	explicit OutputDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(QString::fromUtf8(obs_module_text("MenuTitle")));
		auto *form = new QFormLayout(this);
		form->addRow(QString::fromUtf8(obs_module_text("Device")), &device);
		form->addRow(QString::fromUtf8(obs_module_text("OutputMode")), &mode);
		form->addRow(QString::fromUtf8(obs_module_text("BufferCount")), &buffers);
		status.setWordWrap(true);
		form->addRow(&status);
		form->addRow(&startButton, &stopButton);

		const BPtr<char> path = obs_module_config_path(settingsFile);
		if (path) {
			settings = obs_data_create_from_json_file_safe(path, "bak");
		}
		if (!settings) {
			settings = obs_data_create();
		}

		connect(&device, &QComboBox::currentIndexChanged, this, [this] { refreshModes(); });
		connect(&startButton, &QPushButton::clicked, this, [this] { startOutput(); });
		connect(&stopButton, &QPushButton::clicked, this, [this] {
			stopButton.setEnabled(false);
			setStatus("Stopping");
			obs_output_stop(output);
		});
		connect(this, &QDialog::finished, this, [this] { saveSettings(); });
	}

	~OutputDialog() override
	{
		saveSettings();
		stoppedSignal.Disconnect();
		if (output) {
			obs_output_force_stop(output);
		}
	}

	void refresh()
	{
		if (output) {
			return;
		}
		properties = obs_get_output_properties(outputId);
		const OBSDataAutoRelease defaults = obs_output_defaults(outputId);
		fillChoices(device, obs_properties_get(properties, DEVICE_HASH),
			    QString::fromUtf8(obs_data_get_string(settings, DEVICE_HASH)));
		const long long bufferCount = obs_data_has_user_value(settings, BUFFER_COUNT)
						      ? obs_data_get_int(settings, BUFFER_COUNT)
						      : obs_data_get_int(defaults, BUFFER_COUNT);
		fillChoices(buffers, obs_properties_get(properties, BUFFER_COUNT),
			    QVariant::fromValue<qlonglong>(bufferCount));
		refreshModes();
	}

private:
	void setStatus(const char *key) { status.setText(QString::fromUtf8(obs_module_text(key))); }

	void refreshModes()
	{
		const auto hash = device.currentData().toString().toUtf8();
		const auto selectedMode = mode.currentData().isValid()
						  ? mode.currentData()
						  : QVariant::fromValue<qlonglong>(obs_data_get_int(settings, MODE_ID));
		obs_data_set_string(settings, DEVICE_HASH, hash.constData());
		if (auto *deviceProperty = obs_properties_get(properties, DEVICE_HASH);
		    deviceProperty && !hash.isEmpty()) {
			obs_property_modified(deviceProperty, settings);
		}
		fillChoices(mode, hash.isEmpty() ? nullptr : obs_properties_get(properties, MODE_ID), selectedMode);
		updateControls();
	}

	void updateControls()
	{
		const bool idle = !output;
		device.setEnabled(idle && device.count() > 0);
		mode.setEnabled(idle && mode.count() > 0);
		buffers.setEnabled(idle && buffers.count() > 0);
		startButton.setEnabled(idle && device.currentIndex() >= 0 && mode.currentIndex() >= 0 &&
				       buffers.currentIndex() >= 0);
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
		if (device.currentIndex() < 0 || mode.currentIndex() < 0 || buffers.currentIndex() < 0) {
			return;
		}
		obs_data_set_string(settings, DEVICE_HASH, device.currentData().toString().toUtf8().constData());
		obs_data_set_int(settings, MODE_ID, mode.currentData().toLongLong());
		obs_data_set_int(settings, BUFFER_COUNT, buffers.currentData().toLongLong());
		const BPtr<char> path = obs_module_config_path(settingsFile);
		if (path && QDir().mkpath(QFileInfo(QString::fromUtf8(path.Get())).absolutePath())) {
			if (!obs_data_save_json_safe(settings, path, "tmp", "bak")) {
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
		auto *dialog = static_cast<OutputDialog *>(data);
		const bool success = calldata_int(event, "code") == OBS_OUTPUT_SUCCESS;
		QMetaObject::invokeMethod(
			dialog,
			[dialog, success] {
				dialog->stoppedSignal.Disconnect();
				dialog->output = nullptr;
				dialog->refresh();
				if (!success) {
					dialog->setStatus("OutputFailed");
				}
			},
			Qt::QueuedConnection);
	}

	QComboBox device;
	QComboBox mode;
	QComboBox buffers;
	QLabel status;
	QPushButton startButton{QString::fromUtf8(obs_module_text("Start"))};
	QPushButton stopButton{QString::fromUtf8(obs_module_text("Stop"))};
	OBSDataAutoRelease settings;
	OBSProperties properties;
	OBSOutputAutoRelease output;
	OBSSignal stoppedSignal;
};

QPointer<QAction> menuAction;
QPointer<OutputDialog> outputDialog;

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		delete outputDialog.data();
	}
}

} // namespace

bool obs_module_load(void)
{
	return true;
}

void obs_module_post_load(void)
{
	menuAction = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("MenuTitle")));
	if (!menuAction) {
		return;
	}
	QObject::connect(menuAction, &QAction::triggered, menuAction, [] {
		if (!outputDialog) {
			outputDialog = new OutputDialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
		}
		if (!outputDialog->isVisible()) {
			outputDialog->refresh();
		}
		outputDialog->show();
		outputDialog->raise();
		outputDialog->activateWindow();
	});
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	delete outputDialog.data();
	delete menuAction.data();
}
