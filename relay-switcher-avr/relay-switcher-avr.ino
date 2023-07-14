// This is v1 for use with the black digital relay switches
// It adds memory for the most recent engage/bypass setting.

#include <avr/io.h>
#include <util/delay.h>
#include <avr/eeprom.h>

//------------------------------------------------
// BASIC BEHAVIOR SETTINGS:

// which uC
#define ATTINY85
//#define ATTINY13

// By default we assume normally open unless this is defined
//#define NORMALLY_CLOSED

// If you define SIMPLIFIED_DEBOUNCE it will trust the
// first instance of a button press, then ignore subsequent
// presses for the SIMPLIFIED_DEBOUNCE_DELAY.
// Otherwise if this is not defined it will do a more traditional
// debounce that looks for a stabilized change of state.
#define SIMPLIFIED_DEBOUNCE

// Define DISABLE_TEMPORARY_SWITCH to turn off the ability to hold
// the switch to temporarily engage/bypass.
//#define DISABLE_TEMPORARY_SWITCH

//------------------------------------------------



#ifdef ATTINY85
# define EEPROM_SIZE 512
#else
# define EEPROM_SIZE 64
#endif

#define EEPROM_CHUNK_SIZE 1 // we're choosing to use 1 byte
#define EEPROM_ADDR_FLAG_MASK 0b10000000  // chosen to indicate current addr
#define ADDR_BOOT_SETTING   0x0
#define ADDR_BYPASS_SETTING 0x1 // starting location to look
uint16_t eeprom_addr = ADDR_BYPASS_SETTING;

// These will also indicate the number of LED blinks
#define ON_BOOT_POS1     0x3
#define ON_BOOT_POS2     0x4
#define ON_BOOT_REMEMBER 0x5

#ifdef SIMPLIFIED_DEBOUNCE
# define DEBOUNCE_DELAY 250
#else
# define DEBOUNCE_DELAY 14
#endif

#define PIN_SW     PB4
#define PIN_LED1   PB2
#define PIN_LED2   PB3
#define PIN_POS1   PB1
#define PIN_POS2   PB0

// If you hold down the switch for at least this number of ms, then
// we assume you are holding it down to temporarily toggle the state,
// and want it to toggle back on release.
// i.e. it's not a quick press and release.
#define TEMPORARY_SWITCH_TIME 500


#define RELAY_SWITCH_TIME 40



/* TBD: If space requires we can consolidate these state vars into a single byte */
uint8_t position = 1;
uint8_t sw_state;     // its debounced state. i.e. what we assume is intended by player
uint8_t sw_last_loop; // state last loop

unsigned long sw_stable_since = 0;
unsigned long sw_pressed_at   = 0;


uint8_t use_eeprom = 0;

void setup() {
  DDRB &= ~(1 << PIN_SW); // input
  PORTB |= (1 << PIN_SW); // activate pull-up resistor
  DDRB |= (1 << PIN_LED1); // output
  DDRB |= (1 << PIN_LED2); // output
  DDRB |= (1 << PIN_POS1); // output
  DDRB |= (1 << PIN_POS2); // output


  // wait for the pullup to do its job,
  // otherwise PIN_SW can sometimes get false LOW readings
  _delay_ms(5);
  sw_last_loop = read_switch();
  sw_state = sw_last_loop;

  uint8_t blink_n_times = 0;

  if (eeprom_is_ready()) {
    uint8_t on_boot_setting = eeprom_read_byte((const uint8_t *)ADDR_BOOT_SETTING);

    if (on_boot_setting == ON_BOOT_POS1) {
      position = 1;

    } else if (on_boot_setting == ON_BOOT_POS2) {
      position = 2;

    // If this hasn't been set (probably 0xff), we'll always assume it's ON_BOOT_REMEMBER
    } else {
      on_boot_setting = ON_BOOT_REMEMBER;
      position = eeprom_read_position();
    }

    // If switch is held when powered on, toggle the on_boot setting
    if (sw_last_loop == 0) {
      if (on_boot_setting == ON_BOOT_REMEMBER) {
        on_boot_setting = ON_BOOT_POS1;
      } else if (on_boot_setting == ON_BOOT_POS1) {
        on_boot_setting = ON_BOOT_POS2;
      } else {
        on_boot_setting = ON_BOOT_REMEMBER;
        // More important than getting the position setting is
        // setting the eeprom_addr, which this call does.
        position = eeprom_read_position();
      }
      eeprom_update_byte((uint8_t *)ADDR_BOOT_SETTING, on_boot_setting);
      blink_n_times = on_boot_setting;
    }

    if (on_boot_setting == ON_BOOT_REMEMBER) {
      use_eeprom = 1;
    }
  }

  write_position(); // get the relay setup before blinking the LED for EEPROM
  _delay_ms(1);

  if (blink_n_times > 0) {
    // Blink LED. Ordinarily this should be done asynchronously,
    // but on initial boot it probably doesn't matter
    for (int i = 1; i < blink_n_times; i++) {
      PORTB |= (1 << PIN_LED1); //high
      PORTB |= (1 << PIN_LED2); //high
      _delay_ms(200);
      PORTB &= ~(1 << PIN_LED1); //low
      PORTB &= ~(1 << PIN_LED2); //low
      _delay_ms(200);
    }
    _delay_ms(1000);
    set_led(); // set the LED again
  }
}

void loop() {
  uint8_t sw_this_loop = read_switch();

  unsigned long now = millis();

#ifndef SIMPLIFIED_DEBOUNCE
  // If still unstable, reset the clock
  if (sw_this_loop != sw_last_loop) {
    sw_stable_since = now;
  }
#endif

  // has sw state has changed?
  // Is switch state stable?
  if ((sw_this_loop != sw_state) 
      && (now - sw_stable_since) > DEBOUNCE_DELAY)
  {
#ifdef SIMPLIFIED_DEBOUNCE
    sw_stable_since = now;
#endif
    sw_state = sw_this_loop;

    if (sw_state == 0) {// sw pressed
      sw_pressed_at = now;
      toggle_position();

    // Switch has just been released. Let's decide if it was a long press for temporary engagement/disengagement,
    // or a quick press to toggle its state.
    } else {
#ifndef DISABLE_TEMPORARY_SWITCH
      if ((now - sw_pressed_at) > TEMPORARY_SWITCH_TIME) {
        toggle_position();
      }
#endif
    }
  }

  sw_last_loop = sw_this_loop;
}

uint8_t eeprom_read_position() {
  // This may not find the flag, so these two defaults are important.
  eeprom_addr = ADDR_BYPASS_SETTING;
  uint8_t rv = 0;

  for (uint16_t i = 0; i < (EEPROM_SIZE / EEPROM_CHUNK_SIZE); i++) {
    uint16_t addr = i * EEPROM_CHUNK_SIZE;

    uint8_t b = eeprom_read_byte((const uint8_t *)addr);
    if (b & EEPROM_ADDR_FLAG_MASK) {
      eeprom_addr = addr;

      rv = b & 0x1; // remove the EEPROM_ADDR_FLAG_MASK bit
      break;
    }
  }
  return rv;
}

void eeprom_write_position(uint8_t setting) {
  // don't unnecessarily wear out the eeprom.
  if (use_eeprom == 0) {
    return;
  }

  eeprom_update_byte((uint8_t *)eeprom_addr, 0x0); // Clear flag on previous location.
                                        // This will also clean up any default data in
                                        // the eeprom that coincidentally had the flag bit set.

  if (eeprom_addr < (EEPROM_SIZE - EEPROM_CHUNK_SIZE)) {
    eeprom_addr += EEPROM_CHUNK_SIZE;
  } else {
    eeprom_addr = ADDR_BYPASS_SETTING;
  }
  eeprom_update_byte((uint8_t *)eeprom_addr, EEPROM_ADDR_FLAG_MASK | setting);
}

// This may block. See comments
void toggle_position() {
  if (position == 1) {
    position = 2;
  } else {
    position = 1;
  }
  write_position();
  eeprom_write_position(position);
}

void write_position() {
  set_led();
  PORTB &= ~(1 << PIN_POS1); // low
  PORTB &= ~(1 << PIN_POS2); // low
  // There's a non-zero chance the blocking we do here could
  // interfere with the switch debouncing.
  // But with the simplified debounce it's a non-issue.
  if (position == 1) {
    PORTB |= (1 << PIN_POS1); // high
    _delay_ms(RELAY_SWITCH_TIME);
    PORTB &= ~(1 << PIN_POS1); // low
  } else {
    PORTB |= (1 << PIN_POS2); // high
    _delay_ms(RELAY_SWITCH_TIME);
    PORTB &= ~(1 << PIN_POS2); // low
  }
}

void set_led() {
  if (position == 1) {
    PORTB |= (1 << PIN_LED1); // high
    PORTB &= ~(1 << PIN_LED2); // low
  } else {
    PORTB |= (1 << PIN_LED2); // high
    PORTB &= ~(1 << PIN_LED1); // low
  }
}

uint8_t read_switch() {
  uint8_t rv = PINB & (1 << PIN_SW);
#ifdef NORMALLY_CLOSED
  rv = ! rv;
#endif
  return rv;
}
