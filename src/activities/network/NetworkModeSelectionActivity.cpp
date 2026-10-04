#include "NetworkModeSelectionActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/icons/networkModeIcons.h"

namespace fui = freeink::ui;

namespace {
// The fourth row is the USB transfer method this board actually has. Boards
// that can act as a USB mass-storage device (FREEINK_CAP_USB_MSC) show "USB
// Drive"; the rest keep the serial protocol.
#if FREEINK_CAP_USB_MSC
constexpr NetworkMode USB_MODE = NetworkMode::USB_DRIVE;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_DRIVE;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_DRIVE_DESC;
constexpr const uint8_t* USB_MODE_ICON = networkModeIconUsb;
#else
constexpr NetworkMode USB_MODE = NetworkMode::USB_SERIAL;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_TRANSFER;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_TRANSFER_DESC;
constexpr const uint8_t* USB_MODE_ICON = networkModeIconTransfer;
#endif

constexpr int ROWS = NetworkModeSelectionActivity::MENU_ITEM_COUNT;
constexpr NetworkMode menuModes[ROWS] = {NetworkMode::JOIN_NETWORK, NetworkMode::CONNECT_CALIBRE,
                                         NetworkMode::CREATE_HOTSPOT, USB_MODE};
constexpr StrId menuItems[ROWS] = {StrId::STR_JOIN_NETWORK, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CREATE_HOTSPOT,
                                   USB_MODE_LABEL};
constexpr StrId menuDescs[ROWS] = {StrId::STR_JOIN_DESC, StrId::STR_CALIBRE_DESC, StrId::STR_HOTSPOT_DESC,
                                   USB_MODE_DESC};
constexpr const uint8_t* menuIcons[ROWS] = {networkModeIconWifi, networkModeIconLibrary, networkModeIconHotspot,
                                            USB_MODE_ICON};

// A 32 px icon from networkModeIcons.h as the bitmap a list row draws (Mask1: bit 0 = ink).
fui::BitmapRef rowIcon(const uint8_t* bits) {
  fui::BitmapRef icon;
  icon.data = bits;
  icon.width = 32;
  icon.height = 32;
  icon.format = fui::BitmapFormat::Mask1;
  return icon;
}
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i]);
    item.subtitle = I18N.get(menuDescs[i]);
    item.icon = rowIcon(menuIcons[i]);
    item.actionValue = static_cast<int16_t>(i);
    rowItems[i] = item;
  }
}

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Every choice leaves this screen; a lingering tap flash would gray an unrelated element on the
  // next screen's first render.
  app.clearTapFlash();
  nav.selected = index;
  const NetworkMode mode = menuModes[index];

  // Neither USB mode needs WiFi or the web server, so hand off directly here
  // instead of routing the result back through the WiFi-centric
  // CrossPointWebServerActivity. The WiFi modes still return to that owner.
  if (mode == NetworkMode::USB_SERIAL) {
    activityManager.goToSerialTransfer();
    return;
  }
  if (mode == NetworkMode::USB_DRIVE) {
    activityManager.goToUsbDrive();
    return;
  }
  onModeSelected(mode);
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);
  auto props = listProps(screen);
  props.items = rowItems;
  props.count = static_cast<uint16_t>(MENU_ITEM_COUNT);
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  addList(screen, props, /*hasSubtitle=*/true);
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
