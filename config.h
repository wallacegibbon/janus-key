#include "./janus-key.h"

mod_key mod_map[] =
  {
    /// The most ergonomic idea for a qwerty keyboard.
    {KEY_SPACE, 0, KEY_LEFTCTRL},

    /// CAPSLOCK into ESC
    {KEY_CAPSLOCK, KEY_ESC},
  };

// If a key is held down for a time greater than max_delay, then,
// when released, it will not send its primary function.
unsigned int max_delay = 300;
