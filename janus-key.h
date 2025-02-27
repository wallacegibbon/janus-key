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
  long key;
  long primary_function;
  long secondary_function;

  long value;
  long last_secondary_function_value;

  struct timespec last_time_down;
}
  mod_key;

static inline long
mod_key_primary_function (mod_key *self)
{
  return self->primary_function > 0 ? self->primary_function : self->key;
}
