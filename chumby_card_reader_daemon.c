/* Copyright (C) 2022 Doug Brown
 *
 * This file is part of chumby-utils.
 *
 * chumby-utils is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * chumby-utils is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <gpiod.h>

// This program monitors the "card inserted" GPIO pins and ensures the card reader
// isn't attached as a USB device if there is no card inserted. This gets rid of
// the empty sda device that you end up with if a card isn't inserted.

#define USB_STORAGE_QUIRKS_PATH "/sys/module/usb_storage/parameters/quirks"
#define USB_STORAGE_BIND_PATH "/sys/bus/usb/drivers/usb-storage/bind"
#define CARD_READER_QUIRK "058f:6366:i"
#define CARD_READER_USB_INTERFACE "1-1.4:1.0"
#define CARD_READER_AUTHORIZED_PATH "/sys/bus/usb/devices/1-1.4/authorized"

#define NUM_PRESENCE_GPIOS 4

#define SD_PRESENT_GPIO 100
#define XD_PRESENT_GPIO 101
#define MS_PRESENT_GPIO 102
#define CF_PRESENT_GPIO 103

#define SD_PRESENT (1 << 0)
#define XD_PRESENT (1 << 1)
#define MS_PRESENT (1 << 2)
#define CF_PRESENT (1 << 3)

static uint8_t debounced_presence_flags;
static struct gpiod_chip *gpio_chip;
static struct gpiod_line_bulk gpio_bulk;
static unsigned int gpio_offsets[NUM_PRESENCE_GPIOS] = {
	SD_PRESENT_GPIO,
	XD_PRESENT_GPIO,
	MS_PRESENT_GPIO,
	CF_PRESENT_GPIO
};

static uint8_t check_card_presence(void)
{
	uint8_t retval = 0;

	// Read all gpio states
	int values[NUM_PRESENCE_GPIOS];
	if (gpiod_line_get_value_bulk(&gpio_bulk, values) != 0) {
		return 0;
	}
	
	// Turn them into a bitmask. Note that we marked it as active low,
	// so if they are active they will be nonzero here
	for (int i = 0; i < NUM_PRESENCE_GPIOS; i++) {
		if (values[i]) {
			retval |= (1 << i);
		}
	}
	
	return retval;
}

static void wait_for_card_presence_change(void)
{
	uint8_t bouncy_state, new_bouncy_state;
	struct timespec timeout = { .tv_sec = 0, .tv_nsec = 500000000 };
	struct gpiod_line_bulk event_bulk;
	struct gpiod_line *line, **lineptr;
	struct gpiod_line_event event;
	int result;
	
	// Wait forever for card presence to change
	result = gpiod_line_event_wait_bulk(&gpio_bulk, NULL, &event_bulk);
	if (result <= 0) {
		// This shouldn't happen, but if it did, bail
		exit(4);
	}

	// Read out the event on any affected lines
	gpiod_line_bulk_foreach_line(&event_bulk, line, lineptr) {
		gpiod_line_event_read(line, &event);
	}
	
	// Save the new bouncy state
	bouncy_state = check_card_presence();

	// Now wait for it to stabilize
	while (1) {
		result = gpiod_line_event_wait_bulk(&gpio_bulk, &timeout, &event_bulk);
		if (result < 0) {
			// This shouldn't happen, but if it did, bail
			exit(5);
		} else if (result > 0) {
			// It changed again. First, read out all events
			gpiod_line_bulk_foreach_line(&event_bulk, line, lineptr) {
				gpiod_line_event_read(line, &event);
			}

			// Update bouncy state
			bouncy_state = check_card_presence();
		} else {
			// It timed out. If it hasn't changed still, we can break out of the loop!
			if (bouncy_state == check_card_presence()) {
				break;
			}
		}
	}
	
	// We have now debounced it; save the new debounced state
	debounced_presence_flags = bouncy_state;
}

static void remove_card_reader_quirk(void)
{
	// In case there are other quirks installed, only remove ours. Start
	// by reading the existing quirks...
	char *quirks = malloc(1024);
	FILE *f = fopen(USB_STORAGE_QUIRKS_PATH, "rb");
	size_t bytes_read;
	char *pos;
	bool comma_before = false;
	bool comma_after = false;
	size_t move_len;
	char *move_dest;
	char *move_src;

	if (!f) {
		goto return_free;
	}
	bytes_read = fread(quirks, 1, 1023, f);
	fclose(f);

	if (bytes_read == 0) {
		goto return_free;
	}
	
	// NULL-terminate the buffer
	if (quirks[bytes_read - 1] == '\n') {
		quirks[bytes_read - 1] = 0;
	} else {
		quirks[bytes_read] = 0;
		bytes_read++;
	}
	
	// Find our quirk in the buffer
	pos = strstr(quirks, CARD_READER_QUIRK);
	if (pos) {
		// If we found it, remove the quirk. Keep track of whether
		// we have a comma before and/or after.
		if (pos > 0 && *(pos - 1) == ',') {
			comma_before = true;
		}
		if (pos[strlen(CARD_READER_QUIRK)] == ',') {
			comma_after = true;
		}
		
		move_dest = pos;
		move_src = pos + strlen(CARD_READER_QUIRK);
		move_len = bytes_read - (pos - quirks) - strlen(CARD_READER_QUIRK);

		if (comma_before) {
			// Remove the comma before if there is one
			move_dest--;
		} else if (comma_after) {
			// Remove the comma after if there is one of those instead
			move_src++;
			move_len--;
		}
		
		// This memmove will strip out any commas needed
		memmove(move_dest, move_src, move_len);
		
		// Now write it back out
		f = fopen(USB_STORAGE_QUIRKS_PATH, "wb");
		if (f) {
			fwrite(quirks, 1, strlen(quirks), f);
			fclose(f);
		}
	}

return_free:
	free(quirks);
}

static void bind_card_reader(void)
{
	// Send stderr to /dev/null; we don't care if this fails
	int r = system("echo " CARD_READER_USB_INTERFACE " > " USB_STORAGE_BIND_PATH " 2>/dev/null");
	(void)r;
}

static void connect_card_reader(void)
{
	int r = system("echo 1 > " CARD_READER_AUTHORIZED_PATH);
	(void)r;
}

static void disconnect_card_reader(void)
{
	int r = system("echo 0 > " CARD_READER_AUTHORIZED_PATH);
	(void)r;
}

int main(int argc, char *argv[])
{
	uint8_t new_presence_flags;
	
	// Find the gpio chip
	gpio_chip = gpiod_chip_open_by_label("gpio-pxa");
	if (!gpio_chip) {
		return 1;
	}
	
	// Initialize the bulk object and request the presence GPIOs
	gpiod_line_bulk_init(&gpio_bulk);
	if (gpiod_chip_get_lines(gpio_chip, gpio_offsets, NUM_PRESENCE_GPIOS,
				  &gpio_bulk) != 0) {
		return 2;
	}
	// Register to be notified when it changes
	if (gpiod_line_request_bulk_both_edges_events_flags(&gpio_bulk, "Card reader",
							    GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW)
							    != 0) {
		return 3;
	}

	// First of all, remove the card reader quirk if the bootloader added it
	remove_card_reader_quirk();

	// Read initial presence state
	debounced_presence_flags = check_card_presence();
	if (debounced_presence_flags) {
		// Bind it (if it's not already bound because the quirk prevented it)
		bind_card_reader();
		// Connect, in case we started the daemon with it disconnected
		connect_card_reader();
	} else {
		// Disconnect card reader
		disconnect_card_reader();
	}
	
	// Now, loop forever, monitoring the card presence
	while (1) {
		wait_for_card_presence_change();
		if (debounced_presence_flags) {
			connect_card_reader();
		} else {
			disconnect_card_reader();
		}			
	}
	
	// This will never happen, but to be clean...
	gpiod_chip_close(gpio_chip);
	
	return 0;	
}

