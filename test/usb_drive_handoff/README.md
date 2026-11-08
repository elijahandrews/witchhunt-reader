# USB Drive handoff checks

Run `python3 test/usb_drive_handoff/run.py --out /tmp/usb-handoff-check`.
The sanitizer harness compiles the production HAL startup method against recorded hardware calls for S3 hardware serial, native USB, other boards, and no MSC capability. It checks ordering, active-session protection and failure paths. Physical enumeration with the cable already attached still requires a device test.

`input_loop.py --out /tmp/usb-input-check` compiles the production exclusive
main-loop prefix, mapped-input update method and USB activity loop. It exercises
Power/Back/Home/touch exits, connected-host input refusal and timeout boundaries.
The `--negative-control` variant reinserts the duplicate drain and must fail.
This proves input delivery, not USB enumeration or physical timeout recovery.
