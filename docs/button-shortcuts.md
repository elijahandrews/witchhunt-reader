# Button shortcuts

Under **Settings → Controls → Button Actions**, each configurable press can select
**Toggle Device Settings**, **Toggle Status Bar**, or, on boards with USB mass storage, **USB Drive**.
The first action opens the full device Settings menu. Press it again in Settings
or a settings submenu to save and return; when opened from a book, it returns to
that book. Back from the Settings tabs uses the same return destination.

For example, assign Power → Double Press to Toggle Device Settings and
Previous Page → Double Press to USB Drive. These are optional bindings; existing
button preferences and firmware defaults are retained. On X4 Pro the two physical
page keys appear as Up and Down in Button Actions. Power hold remains Sleep.

A button with a double-press action waits up to 300 ms after its first release
to distinguish a single press. A double press fires on the second release.
USB Drive closes the reader before handing the SD card to the computer; eject
the drive on the computer to return to normal operation.

**Next Page → Double Press → Toggle Status Bar** hides or restores the reader's
status items and progress bars without changing their individual preferences.
It reclaims their space and keeps a source-text anchor through repeated toggles
in EPUB, TXT, and Markdown. Fixed-image XTC pages contain their own baked-in
footer, which this action cannot remove. Visibility is saved globally and can
also be changed under **Customise Status Bar → Show Status Bar**. The action is
reader-only, so ordinary list navigation outside a book has no double-click wait.

On ESP32-S3 builds using hardware USB Serial/JTAG, USB Drive startup disconnects
the old serial interface for 250 ms before starting mass storage. This allows
the host to enumerate the new device while the cable stays connected. It is a
one-time startup handoff and never disconnects an already active storage session.
The controllers share the internal USB PHY, as described in the
[Espressif USB PHY guide](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_phy.html).

Existing stored action IDs remain stable. The device and web pickers translate
their capability-filtered option indices to those IDs, so moving a card between
boards does not turn an existing light shortcut into another action.
