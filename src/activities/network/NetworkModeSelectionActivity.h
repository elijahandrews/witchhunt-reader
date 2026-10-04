#pragma once

#include "activities/UiListActivity.h"

// USB_SERIAL and USB_DRIVE are mutually exclusive in the menu: a board that can
// present itself as a USB mass-storage device has no use for the serial
// file-transfer protocol, so it offers the drive instead (see the menu tables
// in the .cpp). Both enumerators exist in every build; only the listing differs.
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_SERIAL, USB_DRIVE };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "USB Drive" or "USB Transfer" - whichever USB transfer this board has, opened directly
 *   (no result comes back)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * Adapted from upstream crosspoint-reader's NetworkModeSelectionActivity (develop @ cdac66ffe,
 * src/activities/network/NetworkModeSelectionActivity.cpp). Ours lists the fourth row on every
 * board (upstream only with USB mass storage) and keeps the serial transfer, the direct USB
 * hand-offs and usesWifi().
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  // Every board lists four rows; the fourth is its USB transfer method.
  static constexpr int MENU_ITEM_COUNT = 4;

  bool usesWifi() const override { return true; }

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override { return MENU_ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  const char* headerTitle() const override;

  // Label, subtitle and icon never change, so the rows are built once, in the constructor.
  freeink::ui::ListItem rowItems[MENU_ITEM_COUNT]{};
};
