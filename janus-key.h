#include "libevdev/libevdev-uinput.h"
#include <sys/types.h>
#include <time.h>

/// Keys to which a secondary function has been assigned are called JANUS KEYS.

/// For `struct input_event' (defined in <linux/input.h>):
///
/// - `time' is the timestamp, it returns the time at which the event happened.
///
/// - `type' is for example EV_REL for relative moment, EV_KEY for a keypress or release.
///   More types are defined in include/linux/input-event-codes.h.
///
/// - `code' is event code, for example REL_X or KEY_BACKSPACE,
///   again a complete list is in include/linux/input-event-codes.h.
///
/// - `value' is the value the event carries.
///   Either a relative change for EV_REL, absolute new value for EV_ABS (joysticks ...),
///   or 0 for EV_KEY for release, 1 for keypress and 2 for autorepeat.

typedef struct
{
  /// `key', `primary_function' and `secondary_function' are all key constants or `0'.
  unsigned int key;
  unsigned int primary_function;
  unsigned int secondary_function;

  /// Physical state of `key`. (= the last value received)
  /// This field stores the `value' of a `struct input_event' object.
  unsigned int state;
  unsigned int last_secondary_function_value_sent;

  struct timespec last_time_down;

  /// time at which delayed remapping should happen
  struct timespec send_down_at;
  /// whether delayed remapping should happen
  unsigned int delayed_down;
}
  mod_key;

static inline unsigned int
mod_key_primary_function (mod_key *self)
{
  return self->primary_function > 0 ? self->primary_function : self->key;
}

static inline int
mod_key_secondary_held (mod_key *self)
{
  //return (self->state == 1 || self->state == 2) && self->secondary_function > 0;
  return self->state == 1 && self->secondary_function > 0;
}
