#pragma once

namespace stubuddy {

// A release gate belongs to one continuous link. A fast drop/reconnect must
// invalidate it even if both reads of online() happen to return true.
constexpr bool NetworkRecordingAllowed(bool online, unsigned armed_epoch, unsigned current_epoch,
                                       bool notification_pending) {
    return online && armed_epoch == current_epoch && !notification_pending;
}
}  // namespace stubuddy
