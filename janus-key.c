/// We can not change the events of an existing keyboard device.
///
/// What we do is creating a new virtual keyboard device (with `uinput')
/// and rebuilding the events in this virtual device
/// while blocking the original keyboard events.

#include "./config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <poll.h>

#define COUNTOF(x) (sizeof(x) / sizeof(*(x)))

/// Max delay set by user stored as a timespec struct
/// This time will be filled with `max_delay' defined in config.h
struct timespec delay_timespec;

static int
is_in_mod_map (unsigned int key)
{
  for (size_t i = 0; i < COUNTOF (mod_map); i++)
    {
      if (mod_map[i].key == key)
	return i;
    }
  return -1;
};

static int
is_janus (unsigned int key)
{
  int i = is_in_mod_map (key);
  if (i >= 0 && mod_map[i].secondary_function > 0)
    return i;
  else
    return -1;
}

static int
timespec_cmp (struct timespec *tp1, struct timespec *tp2)
{
  if (tp1->tv_sec > tp2->tv_sec)
    return 1;
  else if (tp1->tv_sec < tp2->tv_sec)
    return -1;
  else
    {
      if (tp1->tv_nsec > tp2->tv_nsec)
	return 1;
      else if (tp1->tv_nsec < tp2->tv_nsec)
	return -1;
      else
	return 0;
    }
}

static inline int
timespec_cmp_now (struct timespec *t)
{
  struct timespec now;
  clock_gettime (CLOCK_MONOTONIC, &now);
  return timespec_cmp (&now, t);
}

static void
timespec_add (struct timespec *a, struct timespec *b, struct timespec *c)
{
  c->tv_sec = a->tv_sec + b->tv_sec;
  c->tv_nsec = a->tv_nsec + b->tv_nsec;
  if (c->tv_nsec >= 1000000000)
    {
      c->tv_nsec -= 1000000000;
      c->tv_sec++;
    }
}

/// Assumes a >= b here.
static void
timespec_sub (struct timespec *a, struct timespec *b, struct timespec *c)
{
  c->tv_sec = a->tv_sec - b->tv_sec;
  c->tv_nsec = a->tv_nsec - b->tv_nsec;
  if (c->tv_nsec < 0)
    {
      c->tv_nsec += 1000000000;
      c->tv_sec--;
    }
}

static long
timespec_to_ms (struct timespec *ts)
{
  long milliseconds = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
  return milliseconds;
}

static void
ms_to_timespec (long ms, struct timespec *ts)
{
  ts->tv_sec = ms / 1000;
  ts->tv_nsec = (ms % 1000) * 1000000;
}

static void
send_key_ev_and_sync (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  int err;
  err = libevdev_uinput_write_event (uidev, EV_KEY, code, value);
  if (err != 0)
    {
      perror ("Error in writing EV_KEY event\n");
      exit (err);
    }

  err = libevdev_uinput_write_event (uidev, EV_SYN, SYN_REPORT, 0);
  if (err != 0)
    {
      perror ("Error in writing EV_SYN, SYN_REPORT, 0.\n");
      exit (err);
    }

  //printf("Sending %u %u\n", code, value);
}

static int
send_secondary_function_jk_once (struct libevdev_uinput *uidev, mod_key *m, int value)
{
  if (m->last_secondary_function_value_sent != value)
    {
      send_key_ev_and_sync (uidev, m->secondary_function, value);
      m->last_secondary_function_value_sent = value;
      return 1;
    }
  else
    return 0;
}

static void
send_secondary_function_all_jks (struct libevdev_uinput *uidev)
{
  for (size_t i = 0; i < COUNTOF (mod_map); i++)
    {
      mod_key *tmp = &mod_map[i];
      if (mod_key_secondary_held (tmp))
	{
	  tmp->delayed_down = 0;
	  send_secondary_function_jk_once (uidev, tmp, 1);
	}
    }
}

static void
send_primary_function_mod (struct libevdev_uinput *uidev, mod_key *m, int value)
{
  send_key_ev_and_sync (uidev, mod_key_primary_function (m), value);
}

static void
send_primary_function (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  int i = is_in_mod_map (code);
  if (i >= 0)
    send_primary_function_mod (uidev, &mod_map[i], value);
  else
    send_key_ev_and_sync (uidev, code, value);
}

static void
handle_ev_key_jk (struct libevdev_uinput *uidev, unsigned int code, int value, mod_key *jk)
{
  if (value == 0)
    {
      jk->state = 0;
      jk->delayed_down = 0;

      struct timespec trigger_time;
      timespec_add (&jk->last_time_down, &delay_timespec, &trigger_time);

      if (!send_secondary_function_jk_once (uidev, jk, 0))
	{ // state unchanged, which means second function was not triggered.
	  if (timespec_cmp_now (&trigger_time) < 0)
	    { // It's a tap
	      send_secondary_function_all_jks (uidev);
	      send_primary_function_mod (uidev, jk, 1);
	      send_primary_function_mod (uidev, jk, 0);
	    }
	}
    }
  else if (value == 1)
    {
      jk->state = 1;
      jk->delayed_down = 1;

      struct timespec trigger_time;
      clock_gettime (CLOCK_MONOTONIC, &jk->last_time_down);
      timespec_add (&jk->last_time_down, &delay_timespec, &trigger_time);

      jk->send_down_at = trigger_time;
    }
  else
    {
      /// Ignore
    }
}

static void
handle_ev_key_non_jk (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  if (value == 0)
    {
      send_primary_function (uidev, code, 0);
    }
  else if (value == 1)
    {
      send_secondary_function_all_jks (uidev);
      send_primary_function (uidev, code, 1);
    }
  else
    {
      /// Ignore
    }
}

static void
handle_ev_key (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  int jk_index = is_janus (code);
  if (jk_index >= 0)
    handle_ev_key_jk (uidev, code, value, &mod_map[jk_index]);
  else
    handle_ev_key_non_jk (uidev, code, value);
}

static void
handle_timeout (struct libevdev_uinput *uidev)
{
  for (size_t i = 0; i < COUNTOF (mod_map); i++)
    {
      mod_key *tmp = &mod_map[i];
      if (tmp->delayed_down && timespec_cmp_now (&tmp->send_down_at) >= 0)
	{ // The key has been held for more than `max_delay' milliseconds.
	  // It's secondary function anyway now.
	  send_secondary_function_jk_once (uidev, tmp, 1);
	  tmp->delayed_down = 0;
	}
    }
}

static int
evdev_read_skip_sync (struct libevdev *dev, struct input_event *event)
{
  int r = libevdev_next_event (dev,
			       LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING,
			       event);

  if (r == LIBEVDEV_READ_STATUS_SYNC)
    {
      printf ("janus_key: dropped\n");

      while (r == LIBEVDEV_READ_STATUS_SYNC)
	r = libevdev_next_event (dev, LIBEVDEV_READ_FLAG_SYNC, event);

      printf ("janus_key: re-synced\n");
    }

  return r;
}

int
main (int argc, char **argv)
{
  if (argc < 2)
    {
      fprintf (stderr, "Argument Error: Necessary argument is not given.\n");
      exit (1);
    }

  /// Prepare the delay_timespec that will be used in many places.
  ms_to_timespec (max_delay, &delay_timespec);

  /// let (KEY_ENTER), value 0 go through
  usleep (100000);

  int read_fd = open (argv[1], O_RDONLY);
  if (read_fd < 0)
    {
      perror ("Failed to open device\n");
      exit (1);
    }

  int ret;

  struct libevdev *dev = NULL;
  ret = libevdev_new_from_fd (read_fd, &dev);
  if (ret < 0)
    {
      fprintf (stderr, "Failed to init libevdev (%s)\n", strerror (-ret));
      exit (1);
    }

  int write_fd = open ("/dev/uinput", O_RDWR);
  if (write_fd < 0)
    {
      printf ("uifd < 0 (Do you have the right privileges?)\n");
      return -errno;
    }

  struct libevdev_uinput *uidev;

  /// IMPORTANT: Creating a new (e.g. /dev/input/event18) input device.
  ret = libevdev_uinput_create_from_device (dev, write_fd, &uidev);
  if (ret != 0)
    return ret;

  /// IMPORTANT: Blocking the events of the original keyboard device.
  ret = libevdev_grab (dev, LIBEVDEV_GRAB);
  if (ret < 0)
    {
      fprintf (stderr, "grab < 0\n");
      return -errno;
    }

  /// For event waiting (blocking)
  struct pollfd poll_fd;
  poll_fd.fd = read_fd;
  poll_fd.events = POLLIN;

  do
    {
      /// The documents of `libevdev' says:
      ///   "You do not need libevdev_has_event_pending() if you're using select(2) or poll(2)."
      /// But here we need to call `libevdev_has_event_pending' before `poll'
      /// to make it work.
      int has_pending_events = libevdev_has_event_pending (dev);
      if (has_pending_events < 0)
	{
	  perror ("libevdev check pending failed");
	  exit (1);
	}
      if (has_pending_events == 0)
	{
	  /// Block waiting for new events.
	  if (poll (&poll_fd, 1, -1) <= 0)
	    {
	      perror ("poll failed");
	      exit (1);
	    }
	}

      struct input_event event;
      ret = evdev_read_skip_sync (dev, &event);
      if (ret == LIBEVDEV_READ_STATUS_SUCCESS)
	{
	  if (event.type == EV_KEY)
	    handle_ev_key (uidev, event.code, event.value);
	}

      handle_timeout (uidev);
    }
  while (ret == LIBEVDEV_READ_STATUS_SYNC
	 || ret == LIBEVDEV_READ_STATUS_SUCCESS
	 || ret == -EAGAIN);

  if (ret != LIBEVDEV_READ_STATUS_SUCCESS
      && ret != -EAGAIN)
    fprintf (stderr, "Failed to handle events: %s\n", strerror (-ret));

  // No need to use free memory (e.g., using libevdev_free) if the
  // program is shutting down.
  return 0;
}
