# Bluetooth pairing persistence

The tested YDP02X mounts `/var` as tmpfs (`/etc/init.d/S21mountall.sh`). BlueZ writes
adapter settings, link keys and device information to `/var/lib/bluetooth`. Neither
the firmware init scripts nor `runDictPen` restore this database, so an application
restart retains pairings but a device reboot loses them.

`helper/BluetoothStorage` installs `/etc/init.d/S22PenModsBluetooth` atomically when
PenMods initializes the UI. This runs after S21 mounts `/userdata` and before
S36/S40/S50 can start the Bluetooth stack. It bind-mounts
`/userdata/PenMods/bluetooth` over BlueZ's default directory. Changes, including
forgetting devices, go straight to the persistent database; no periodic backup can
resurrect deleted pairings.

On first installation, the current database is copied to a private staging directory
and renamed into place only after a successful copy, then mounted immediately.
Existing persistent databases are never overwritten by later RAM-backed databases.
The database directory has mode 0700 and copied file modes are preserved. A missing
`/userdata` mount fails setup instead of placing pairing keys in volatile storage.
Already persistent BlueZ directories are left alone. The mount remains during
shutdown, and subsequent application starts do not stack bind mounts. QEMU skips
the installer.

The initial migration occurs while bluetoothd may be running: avoid pairing or
forgetting a device during this first installation. If the init script was removed
by a firmware reflash, let PenMods reinstall it, then reboot so bluetoothd loads the
saved database from the beginning. A firmware reflash may also require reinstalling
PenMods. Removing PenMods alone leaves the early-boot storage script in place so
saved pairings remain available.

Verification (does not print pairing keys):

```sh
adb push scripts/check_bluetooth_persistence.sh /tmp/penmods-check-bluetooth.sh
adb shell 'sh /tmp/penmods-check-bluetooth.sh'
# Before the fix: FAIL: BlueZ records are on volatile tmpfs
# After the fix: PASS: BlueZ uses persistent storage ...
adb shell 'find /var/lib/bluetooth -type f -name info -exec sha256sum {} \;'
```

Compare `info` hashes before and after a full reboot, and verify the script again.
Confirm `/var/lib/bluetooth` and `/userdata/PenMods/bluetooth` have the same device
and inode, and that the mount occurs only once. Do not print or commit `info`
contents: these contain link keys.
A separate real accessory check is needed to verify reconnection; durable pairing
records alone do not guarantee auto-connect.

## Device verification for this fix

On the connected YDP02X, the check failed on tmpfs before deployment and passed on
the `/userdata` ext4 bind mount afterward. The main library built successfully,
and its SHA-256 matched the deployed library. After a full device reboot, the
existing paired device's `info` SHA-256 was unchanged and BlueZ's D-Bus
`org.bluez.Device1.Paired` property returned `true`. Two additional invocations of
the init script left exactly one mount at `/var/lib/bluetooth`. The application
remained running with no matching startup binding/symbol errors. The accessory
was disconnected (`Connected=false`) during that check. After a second reboot by
the maintainer, REDMI Buds 8 Pro was still present with `Paired=true` and
`Connected=true`; `ServicesResolved=false`, so audio playback remains unverified.

## Headphone battery display

`helper/BluetoothBattery` is exposed as the `bluetoothBattery` QML context object
after Qt application initialization. It uses QtDBus on the system bus, with no
shell polling, daemon restart, pairing changes or Bluetooth configuration writes.
Its `percentages` map is keyed by uppercase device address and contains only
connected devices with a measured integer in 0–100. A missing entry means unknown;
it must not be converted to 0%.

The preferred source is BlueZ's
[`org.bluez.Battery1.Percentage`](https://github.com/bluez/bluez/blob/5.50/doc/battery-api.txt).
An asynchronous `GetManagedObjects` discovers devices already connected at app
startup, then `InterfacesAdded`, `InterfacesRemoved` and `PropertiesChanged`
maintain the state. Invalidated properties are reread asynchronously. Snapshot
replies replay signals received during loading, and per-request generations stop
late replies from restoring removed/disconnected devices. Loss/replacement of
the `org.bluez` owner clears the map and discovers the new daemon's objects.

There is also a standard
[`GattCharacteristic1`](https://github.com/bluez/bluez/blob/5.50/doc/gatt-api.txt)
fallback: Battery Service UUID `0000180f-0000-1000-8000-00805f9b34fb`, Battery
Level characteristic UUID `00002a19-0000-1000-8000-00805f9b34fb`. Both the
characteristic's `Service` and the remote service's `Device` reference must match
the connected device tree. Read-capable characteristics use asynchronous
`ReadValue`; notify/indicate-capable ones use `StartNotify` and live `Value`
changes. Only one byte in 0–100 is accepted. Disconnect clears the value and
outstanding requests; reconnect rereads instead of showing an old cached value.
There is no invented percentage if discovery, reading or notification is not
supported. Read-only devices can provide an initial/reconnect measurement, but
cannot provide live changes without their own notifications.

This fallback matters on the captured YDP02X firmware: the saved Bluetooth daemon
log identifies BlueZ 5.50 and explicitly says `not load battery plugin`. The
captured `libDeviceIo.so` starts it with
`/usr/libexec/bluetooth/bluetoothd -C -n -d -E &`, without a battery exclusion
argument; the skip message is absent from upstream 5.50 `src/plugin.c`, so it is
a vendor change rather than a proven configuration switch. The vendor HFP
`rk_bt_hfp_report_battery` path sends `AT+IPHONEACCEV` as a hands-free client
(the pen's own battery), not a measured headset battery. PenMods therefore does
not reinterpret that value or replace/restart the vendor Bluetooth stack.
The GATT fallback works without the disabled battery plugin when a connected
accessory exposes the standard Battery Service. Classic headphones that expose
neither Battery1 nor that service remain explicitly unknown; private HFP/vendor
battery protocols are not inferred from the headset's name.

The Bluetooth settings device row adds `已连接 · N%` (or `电量未知`) without
changing its height. The existing status-bar headphone icon gains a narrow
vertical level bar; unknown is `?`, not an empty 0% bar. It uses the host Bluetooth
manager's active device address rather than an arbitrary connected device and
does not attach Bluetooth battery information to the wired-headset indicator.
The shared indicator also appears in quick settings.

Run `python tools/test_bluetooth_battery.py` for the host regression test (requires
Qt5Core/Qt5DBus development files, Qt5 moc, g++ and dbus-run-session). It compiles
the real Qt5 C++ class against a mock `org.bluez` D-Bus
object manager, covering startup, real zero, invalid values, live changes,
disconnect/reconnect, removals, daemon owner changes, late read replies and
signals racing asynchronous snapshots/property reads. A Qt6 offscreen harness
loaded the actual settings page/delegate, `YConnectionIndicators` and
`YMainTitleBar`, with only device contexts/model data/icon provider substituted,
and rendered 320×170 known/unknown/zero/wired/disconnected states. The indicator
row measured 65 px with battery, 58 px without; the actual 320 px title bar fit all
connection icons, 23:59, 100% and the charging icon without overlapping the
portrait. No physical accessory battery
measurement or deployment was performed for this change; the existing REDMI
connection record above does not prove that headset exposes a battery source.
