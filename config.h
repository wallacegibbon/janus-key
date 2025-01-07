#include "./janus-key.h"

// KEY | 1st FUNCTION | 2nd FUNCTION
mod_key mod_map[] = {
  {KEY_SPACE, 0, KEY_LEFTCTRL},
  {KEY_CAPSLOCK, KEY_ESC, KEY_LEFTALT},
};

// Delay in milliseconds.
// If a key is held down for a time greater than max_delay, then,
// when released, it will not send its primary function.
unsigned int max_delay = 200;
