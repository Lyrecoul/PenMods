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
