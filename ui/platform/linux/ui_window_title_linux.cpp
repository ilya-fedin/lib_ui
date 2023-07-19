// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/platform/linux/ui_window_title_linux.h"

#include "base/platform/linux/base_linux_xdp_utilities.h"

#include "base/integration.h"

namespace Ui {
namespace Platform {
namespace {

class TitleControlsLayoutImpl : public TitleControlsLayout {
public:
	TitleControlsLayoutImpl();

private:
	using ButtonPlacement = std::tuple<
		std::vector<Glib::ustring>,
		std::vector<Glib::ustring>
	>;

	[[nodiscard]] static TitleControls::Layout Convert(
		const ButtonPlacement &placement);

	[[nodiscard]] static TitleControls::Layout Get();

	const base::Platform::XDP::SettingWatcher _settingWatcher;
};

TitleControlsLayoutImpl::TitleControlsLayoutImpl()
: TitleControlsLayout(Get())
, _settingWatcher(
	"org.freedesktop.appearance",
	"button-placement",
	[=](const ButtonPlacement &value) {
		base::Integration::Instance().enterFromEventLoop([&] {
			_variable = Convert(value);
		});
	}) {}

TitleControls::Layout TitleControlsLayoutImpl::Convert(
		const ButtonPlacement &placement) {
	const auto toControl = [](const Glib::ustring &keyword) {
		if (keyword == "minimize") {
			return TitleControls::Control::Minimize;
		} else if (keyword == "maximize") {
			return TitleControls::Control::Maximize;
		} else if (keyword == "close") {
			return TitleControls::Control::Close;
		}
		return TitleControls::Control::Unknown;
	};

	return TitleControls::Layout{
		.left = std::get<0>(
			placement
		) | ranges::view::transform(
			toControl
		) | ranges::to_vector,
		.right = std::get<1>(
			placement
		) | ranges::view::transform(
			toControl
		) | ranges::to_vector,
	};
}

TitleControls::Layout TitleControlsLayoutImpl::Get() {
	if (const auto result = base::Platform::XDP::ReadSetting<ButtonPlacement>(
			"org.freedesktop.appearance",
			"button-placement")) {
		return Convert(result);
	}

	return TitleControls::Layout{
		.right = {
			TitleControls::Control::Minimize,
			TitleControls::Control::Maximize,
			TitleControls::Control::Close,
		}
	};
}

} // namespace

std::shared_ptr<TitleControlsLayout> TitleControlsLayout::Create() {
	return std::make_shared<TitleControlsLayoutImpl>();
}

} // namespace Platform
} // namespace Ui
