// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "common/service/Logger.h"
#include "common/service/Singleton.h"

namespace mod {

class BluetoothStorage : public Singleton<BluetoothStorage>, private Logger {
    friend Singleton<BluetoothStorage>;
    BluetoothStorage();
    void install();
};

} // namespace mod
