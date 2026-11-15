#pragma once

#include <QString>

namespace blink {

// Creates or removes the start-at-login entry of the installed Blink. An existing entry is kept as it is, with any
// edits made to it since. Returns what went wrong, or an empty string.
QString applyAutostart(bool enabled);

} // namespace blink
