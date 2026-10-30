// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

namespace mod::filemanager {

// Write original and translated text on adjacent lines sharing the same LRC timestamp.
QString bilingualLrc(const QString& original, const QString& translation, bool& hasTranslation);

} // namespace mod::filemanager
