/*

  janus-key. Give keys a double function.

  Copyright (C) 2021, 2022, 2023  Giulio Pietroiusti

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

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

int last_input_was_special_combination = 0;

/// For calculating delay
struct timespec now;
struct timespec tp_sum;

// If any of the janus keys is down or held return the index of the
// first one of them in the mod_map. Otherwise, return -1.
static int
some_jk_are_down_or_held ()
{
  for (size_t i = 0; i < COUNTOF (mod_map); i++)
    {
      if ((mod_map[i].state == 1 || mod_map[i].state == 2)
	  && mod_map[i].secondary_function > 0)
	{
	  return i;
	}
    }
  return -1;
}

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
  if (i >= 0)
    {
      if (mod_map[i].secondary_function > 0)
	return i;
    }
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

static void
send_down_or_held_jks_secondary_function (struct libevdev_uinput *uidev, int value)
{
  for (size_t i = 0; i < COUNTOF (mod_map); i++)
    {
      if ((mod_map[i].state == 1 || mod_map[i].state == 2)
	  && mod_map[i].secondary_function > 0)
	{
	  mod_map[i].delayed_down = 0;
	  if (mod_map[i].last_secondary_function_value_sent != value)
	    {
	      send_key_ev_and_sync (uidev, mod_map[i].secondary_function, value);
	      mod_map[i].last_secondary_function_value_sent = value;
	    }
	}
    }
}

static void
send_primary_function (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  int i = is_in_mod_map (code);
  if (i >= 0)
    send_key_ev_and_sync (uidev, mod_key_primary_function (&mod_map[i]), value);
  else
    send_key_ev_and_sync (uidev, code, value);
}

static void
handle_ev_key_janus (struct libevdev_uinput *uidev, unsigned int code, int value, mod_key *jk)
{
  if (value == 1)
    {
      struct timespec scheduled_delayed_down;
      jk->state = 1;
      last_input_was_special_combination = 0;

      clock_gettime (CLOCK_MONOTONIC, &jk->last_time_down);
      timespec_add (&jk->last_time_down, &delay_timespec, &scheduled_delayed_down);
      jk->send_down_at = scheduled_delayed_down;
      jk->delayed_down = 1;
    }
  else if (value == 2)
    {
      jk->state = 2;
      last_input_was_special_combination = 0;
    }
  else
    {
      jk->delayed_down = 0;
      jk->state = 0;
      clock_gettime (CLOCK_MONOTONIC, &now);
      //timespec_add (&jk->last_time_down, &tp_max_delay, &tp_sum);
      timespec_add (&jk->last_time_down, &delay_timespec, &tp_sum);
      if (timespec_cmp (&now, &tp_sum) < 0)
	{ // is considered as tap
	  if (last_input_was_special_combination)
	    {
	      if (jk->last_secondary_function_value_sent != 0)
		send_key_ev_and_sync (uidev, jk->secondary_function, 0);

	      jk->last_secondary_function_value_sent = 0;
	    }
	  else
	    {
	      if (some_jk_are_down_or_held () >= 0)
		{
		  last_input_was_special_combination = 1;
		  send_down_or_held_jks_secondary_function (uidev, 1);
		}
	      else
		send_down_or_held_jks_secondary_function (uidev, 0);

	      send_primary_function (uidev, jk->key, 1);
	      send_primary_function (uidev, jk->key, 0);
	    }
	}
      else
	{ // is not considered as tap
	  if (jk->last_secondary_function_value_sent != 0)
	    send_key_ev_and_sync (uidev, jk->secondary_function, 0);

	  jk->last_secondary_function_value_sent = 0;
	}
    }
}

static void
handle_ev_key_normal (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  /// No special handling for key release.
  if (value == 0)
    {
      send_primary_function (uidev, code, 0);
      return;
    }

  /// For Key DOWN or HELD, send active janus keys' secondary function first.
  if (some_jk_are_down_or_held () >= 0)
    {
      last_input_was_special_combination = 1;
      send_down_or_held_jks_secondary_function (uidev, 1);
    }
  else
    last_input_was_special_combination = 0;

  send_primary_function (uidev, code, value);
}

static void
handle_ev_key (struct libevdev_uinput *uidev, unsigned int code, int value)
{
  int jk_index = is_janus (code);
  if (jk_index >= 0)
    handle_ev_key_janus (uidev, code, value, &mod_map[jk_index]);
  else
    handle_ev_key_normal (uidev, code, value);
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

  // variables to manage timeout
  struct input_event event;

  // if true then we have got an event, otherwise we have timed out
  unsigned got_event = 0;

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

  /// We can not change the events of an existing keyboard device.
  ///
  /// What we do is creating a new virtual keyboard device (with `uinput')
  /// and rebuilding the events in this virtual device
  /// while blocking the original keyboard events.

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

  while (rc == LIBEVDEV_READ_STATUS_SYNC
	 || rc == LIBEVDEV_READ_STATUS_SUCCESS
	 || rc == -EAGAIN)
    {
      int has_pending_events = libevdev_has_event_pending (dev);
      got_event = 0;
      struct timespec timeout;

      if (has_pending_events < 0)
	{
	  perror ("pending");
	  exit (1);
	}

      if (has_pending_events == 1)
	{
	  got_event = 1;
	  rc = libevdev_next_event (dev,
				    LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING,
				    &event);

	  if (rc == LIBEVDEV_READ_STATUS_SYNC)
	    {
	      printf ("janus_key: dropped\n");
	      while (rc == LIBEVDEV_READ_STATUS_SYNC)
		rc = libevdev_next_event (dev, LIBEVDEV_READ_FLAG_SYNC, &event);

	      printf ("janus_key: re-synced\n");
	    }
	}
      else
	{
	  // find that mod_key whose `delayed_down` is true and has the soonest `send_down_at`, if any.
	  // there might not be a mod_key that satisfies those conditions
	  int soonest_index = -1;
	  struct timespec soonest_val = {.tv_sec = 0,.tv_nsec = 0 };
	  for (size_t i = 1; i < COUNTOF (mod_map); i++)
	    {
	      if (mod_map[i].delayed_down
		  && timespec_cmp (&mod_map[i].send_down_at, &soonest_val) < 0)
		{
		  soonest_val = mod_map[i].send_down_at;
		  soonest_index = i;
		}
	    }

	  // decide whether to poll and calculate timeout
	  long poll_timeout = -1;
	  int should_poll = 0;
	  if (soonest_index == -1)
	    {
	      // we should poll until a new event (second arg to poll will be -1)
	      should_poll = 1;
	    }
	  else
	    {
	      clock_gettime (CLOCK_MONOTONIC, &now);
	      if (timespec_cmp (&now, &mod_map[soonest_index].send_down_at) < 0)
		{
		  should_poll = 1;
		  timespec_sub (&now, &mod_map[soonest_index].send_down_at, &timeout);
		}
	    }
	  if (should_poll && poll (&poll_fd, 1, poll_timeout))
	    {
	      got_event = 1;
	      rc = libevdev_next_event (dev,
					LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING,
					&event);

	      if (rc == LIBEVDEV_READ_STATUS_SYNC)
		{
		  printf ("janus_key: dropped (after poll returned true)\n");
		  while (rc == LIBEVDEV_READ_STATUS_SYNC)
		    rc = libevdev_next_event (dev, LIBEVDEV_READ_FLAG_SYNC, &event);

		  printf ("janus_key: re-synced (after poll returned true)\n");
		}
	    }
	}

      // handle timers
      for (size_t i = 0; i < COUNTOF (mod_map); i++)
	{
	  clock_gettime (CLOCK_MONOTONIC, &now);
	  timespec_sub (&now, &mod_map[i].send_down_at, &timeout);

	  /// The key has been held for more than `max_delay' milliseconds.
	  /// It's secondary function anyway now.
	  if (mod_map[i].delayed_down
	      && timespec_cmp (&now, &mod_map[i].send_down_at) >= 0)
	    {
	      if (mod_map[i].last_secondary_function_value_sent != 1)
		send_key_ev_and_sync (uidev, mod_map[i].secondary_function, 1);

	      mod_map[i].last_secondary_function_value_sent = 1;
	      mod_map[i].delayed_down = 0;
	    }
	}

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
