/// We can not change the events of an existing keyboard device.
///
/// What we do is creating a new virtual keyboard device (with `uinput')
/// and rebuilding the events in this virtual device
/// while blocking the original keyboard events.

#include "./config.h"
#include <assert.h>
#include <bits/time.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include "libevdev/libevdev.h"
#include "libevdev/libevdev-uinput.h"
#include "poll.h"

#define COUNTOF(x) (sizeof(x) / sizeof(*(x)))

/// Max delay set by user stored as a timespec struct
/// This time will be filled with `max_delay' defined in config.h
struct timespec delay_timespec;

/// If `key` is in the mod_map, then return its index. Otherwise return -1.
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

/// If `key` is a janus key, then return its index. Otherwise return -1.
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
send_secondary_function_once (struct libevdev_uinput *uidev, mod_key *m, int value)
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
send_down_or_held_jks_secondary_function (struct libevdev_uinput *uidev, int value)
{
  for (size_t i = 0; i < COUNTOF (mod_map); i++)
    {
      mod_key *tmp = &mod_map[i];
      if (mod_key_secondary_down_or_held (tmp))
	{
	  tmp->delayed_down = 0;
	  send_secondary_function_once (uidev, tmp, value);
	}
    }
}

static void
send_primary_function (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  int i = is_in_mod_map (code);
  if (i >= 0)
    {
      mod_key *tmp = &mod_map[i];
      send_key_ev_and_sync (uidev, mod_key_primary_function (tmp), value);
    }
  else
    send_key_ev_and_sync (uidev, code, value);
}

/// Those whose primary_functions are different from their secondary_functions are called `janus'.
static void
handle_ev_key_janus (struct libevdev_uinput *uidev, unsigned int code, int value, mod_key *jk)
{
  jk->state = value;
  if (value == 1)
    {
      struct timespec scheduled_delayed_down;
      clock_gettime (CLOCK_MONOTONIC, &jk->last_time_down);
      timespec_add (&jk->last_time_down, &delay_timespec, &scheduled_delayed_down);
      jk->send_down_at = scheduled_delayed_down;
      jk->delayed_down = 1;
    }
  else if (value == 2)
    {
      /// Nothing to do here, things had already been done in `value == 1' branch.
    }
  else
    {
      jk->delayed_down = 0;
      struct timespec sum;
      timespec_add (&jk->last_time_down, &delay_timespec, &sum);
      if (timespec_cmp_now (&sum) < 0)
	{ // Considered as tap (delayed click is not triggered)
	  if (!send_secondary_function_once (uidev, jk, 0))
	    { // last_send is zero, which means this janus key is acting its primary function.
	      send_down_or_held_jks_secondary_function (uidev, 1);
	      send_primary_function (uidev, jk->key, 1);
	      send_primary_function (uidev, jk->key, 0);
	    }
	}
      else
	{ // Not considered as tap (delayed click has already been sent)
	  send_secondary_function_once (uidev, jk, 0);
	}
    }
}

static void
handle_ev_key_normal_or_mapping (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  /// No special handling for key release.
  if (value == 0)
    {
      send_primary_function (uidev, code, 0);
      return;
    }

  /// For Key DOWN or HELD, send active janus keys' secondary function first.
  send_down_or_held_jks_secondary_function (uidev, 1);
  send_primary_function (uidev, code, value);
}

static void
handle_ev_key (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  int jk_index = is_janus (code);
  if (jk_index >= 0)
    handle_ev_key_janus (uidev, code, value, &mod_map[jk_index]);
  else
    handle_ev_key_normal_or_mapping (uidev, code, value);
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
	  send_secondary_function_once (uidev, tmp, 1);
	  tmp->delayed_down = 0;
	}
    }
}

static int
soonest_delayed_down ()
{
  struct timespec
    soonest_val = {.tv_sec = 0,.tv_nsec = 0 },
    *soonest = &soonest_val;

  int soonest_index = -1;

  for (size_t i = 1; i < COUNTOF (mod_map); i++)
    {
      mod_key *tmp = &mod_map[i];
      if (tmp->delayed_down && timespec_cmp (&tmp->send_down_at, soonest) < 0)
	{
	  soonest = &tmp->send_down_at;
	  soonest_index = i;
	}
    }

  return soonest_index;
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
  ms_to_timespec (max_delay, &delay_timespec);
  struct libevdev *dev = NULL;
  const char *file;
  int read_fd;
  int rc = 1;
  if (argc < 2)
    exit (1);

  // let (KEY_ENTER), value 0 go through
  usleep (100000);

  file = argv[1];
  read_fd = open (file, O_RDONLY);
  if (read_fd < 0)
    {
      perror ("Failed to open device\n");
      exit (1);
    }

  struct pollfd poll_fd;
  poll_fd.fd = read_fd;
  poll_fd.events = POLLIN;

  rc = libevdev_new_from_fd (read_fd, &dev);
  if (rc < 0)
    {
      fprintf (stderr, "Failed to init libevdev (%s)\n", strerror (-rc));
      exit (1);
    }

  int err;
  int write_fd;
  struct libevdev_uinput *uidev;

  write_fd = open ("/dev/uinput", O_RDWR);
  if (write_fd < 0)
    {
      printf ("uifd < 0 (Do you have the right privileges?)\n");
      return -errno;
    }

  /// IMPORTANT: Creating a new (e.g. /dev/input/event18) input device.
  err = libevdev_uinput_create_from_device (dev, write_fd, &uidev);
  if (err != 0)
    return err;

  /// IMPORTANT: Blocking the events of the original keyboard device.
  int grab = libevdev_grab (dev, LIBEVDEV_GRAB);
  if (grab < 0)
    {
      printf ("grab < 0\n");
      return -errno;
    }

  /// variables to manage timeout
  struct input_event event;
  // if true then we have got an event, otherwise we have timed out
  int got_event = 0;

  while (rc == LIBEVDEV_READ_STATUS_SYNC
	 || rc == LIBEVDEV_READ_STATUS_SUCCESS
	 || rc == -EAGAIN)
    {
      int has_pending_events = libevdev_has_event_pending (dev);
      got_event = 0;

      if (has_pending_events < 0)
	{
	  perror ("pending");
	  exit (1);
	}

      if (has_pending_events == 0)
	{
	  if (poll (&poll_fd, 1, -1) <= 0)
	    {
	      perror ("poll failed");
	      exit (1);
	    }
	}

      got_event = 1;
      rc = evdev_read_skip_sync (dev, &event);

      handle_timeout (uidev);

      // handle new event if we have one
      if (got_event && rc == LIBEVDEV_READ_STATUS_SUCCESS)
	{
	  if (event.type == EV_KEY)
	    handle_ev_key (uidev, event.code, event.value);
	}

    }

  if (rc != LIBEVDEV_READ_STATUS_SUCCESS && rc != -EAGAIN)
    fprintf (stderr, "Failed to handle events: %s\n", strerror (-rc));

  // No need to use free memory (e.g., using libevdev_free) if the
  // program is shutting down.
  return 0;
}
