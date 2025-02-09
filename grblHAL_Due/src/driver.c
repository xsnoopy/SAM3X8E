/*

  driver.c - driver code for Atmel SAM3X8E ARM processor

  Part of grblHAL

  Copyright (c) 2019-2021 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "driver.h"
#include "serial.h"

#include "grbl/limits.h"
#include "grbl/crossbar.h"
#include "grbl/motor_pins.h"
#include "grbl/protocol.h"
#include "grbl/pin_bits_masks.h"

#if USB_SERIAL_CDC
#include "usb_serial.h"
#endif

#if SDCARD_ENABLE
#include "sdcard/sdcard.h"
#include "diskio.h"
#endif

#if EEPROM_ENABLE
#include "eeprom/eeprom.h"
#endif

#if BLUETOOTH_ENABLE
#include "bluetooth/bluetooth.h"
#endif

#if PLASMA_ENABLE
#include "plasma/thc.h"
#endif

#if I2C_ENABLE
#include "i2c.h"
#endif

#define DEBOUNCE_QUEUE 8 // Must be a power of 2

#ifndef OUTPUT
#define OUTPUT true
#endif
#ifndef INPUT
#define INPUT false
#endif

typedef struct { 
    volatile uint_fast8_t head;
    volatile uint_fast8_t tail;
    input_signal_t *signal[DEBOUNCE_QUEUE];
} debounce_queue_t;

static bool IOInitDone = false;
static uint32_t pulse_length, pulse_delay;
static const io_stream_t *serial_stream;
static axes_signals_t next_step_outbits;
static delay_t delay_ms = { .ms = 1, .callback = NULL }; // NOTE: initial ms set to 1 for "resetting" systick timer on startup
static debounce_queue_t debounce_queue = {0};
static input_signal_t a_signals[10] = {0}, b_signals[10] = {0}, c_signals[10] = {0}, d_signals[10] = {0};
static probe_state_t probe = {
    .connected = On
};
#ifdef SQUARING_ENABLED
static axes_signals_t motors_1 = {AXES_BITMASK}, motors_2 = {AXES_BITMASK};
#endif

#ifndef VFD_SPINDLE
static bool pwmEnabled = false;
static spindle_pwm_t spindle_pwm = {0};

static void spindle_set_speed (uint_fast16_t pwm_value);
#endif

static input_signal_t inputpin[] = {
  #ifdef PROBE_PIN
    { .id = Input_Probe,        .port = PROBE_PORT,        .pin = PROBE_PIN,         .group = PinGroup_Probe },
  #endif
  #ifdef RESET_PIN
    { .id = Input_Reset,        .port = RESET_PORT,        .pin = RESET_PIN,         .group = PinGroup_Control },
  #endif
  #ifdef FEED_HOLD_PIN
    { .id = Input_FeedHold,     .port = FEED_HOLD_PORT,    .pin = FEED_HOLD_PIN,     .group = PinGroup_Control },
  #endif
  #ifdef CYCLE_START_PIN
    { .id = Input_CycleStart,   .port = CYCLE_START_PORT,  .pin = CYCLE_START_PIN,   .group = PinGroup_Control },
  #endif
  #ifdef SAFETY_DOOR_PIN
    { .id = Input_SafetyDoor,   .port = SAFETY_DOOR_PORT,  .pin = SAFETY_DOOR_PIN,   .group = PinGroup_Control },
  #endif
    { .id = Input_LimitX,       .port = X_LIMIT_PORT,      .pin = X_LIMIT_PIN,       .group = PinGroup_Limit },
  #ifdef X2_LIMIT_PIN
    { .id = Input_LimitX_2,     .port = X2_LIMIT_PORT,     .pin = X2_LIMIT_PIN,      .group = PinGroup_Limit },
  #endif
  #ifdef X_LIMIT_PIN_MAX
    { .id = Input_LimitX_Max,   .port = X_LIMIT_PORT_MAX,  .pin = X_LIMIT_PIN_MAX,   .group = PinGroup_Limit },
  #endif
    { .id = Input_LimitY,       .port = Y_LIMIT_PORT,      .pin = Y_LIMIT_PIN,       .group = PinGroup_Limit },
  #ifdef Y2_LIMIT_PIN
   { .id = Input_LimitY_2,      .port = Y2_LIMIT_PORT,     .pin = Y2_LIMIT_PIN,      .group = PinGroup_Limit },
  #endif
  #ifdef Y_LIMIT_PIN_MAX
    { .id = Input_LimitY_Max,   .port = Y_LIMIT_PORT_MAX,  .pin = Y_LIMIT_PIN_MAX,   .group = PinGroup_Limit },
  #endif
    { .id = Input_LimitZ,       .port = Z_LIMIT_PORT,      .pin = Z_LIMIT_PIN,       .group = PinGroup_Limit },
  #ifdef Z2_LIMIT_PIN
    { .id = Input_LimitZ_2,     .port = Z2_LIMIT_PORT,     .pin = Z2_LIMIT_PIN,      .group = PinGroup_Limit },
  #endif
  #ifdef Z_LIMIT_PIN_MAX
    { .id = Input_LimitZ_Max,   .port = Z_LIMIT_PORT_MAX,  .pin = Z_LIMIT_PIN_MAX,   .group = PinGroup_Limit },
  #endif
  #ifdef A_LIMIT_PIN
    { .id = Input_LimitA,       .port = A_LIMIT_PORT,      .pin = A_LIMIT_PIN,       .group = PinGroup_Limit },
  #endif
  #ifdef A_LIMIT_PIN_MAX
    { .id = Input_LimitA_Max,   .port = A_LIMIT_PORT_MAX,  .pin = A_LIMIT_PIN_MAX,   .group = PinGroup_Limit },
  #endif
  #ifdef B_LIMIT_PIN
    { .id = Input_LimitB,       .port = B_LIMIT_PORT,      .pin = B_LIMIT_PIN,       .group = PinGroup_Limit },
  #endif
  #ifdef B_LIMIT_PIN_MAX
    { .id = Input_LimitB_Max,   .port = B_LIMIT_PORT_MAX,  .pin = B_LIMIT_PIN_MAX,   .group = PinGroup_Limit },
  #endif
  #ifdef C_LIMIT_PIN
    { .id = Input_LimitC,       .port = C_LIMIT_PORT,      .pin = C_LIMIT_PIN,       .group = PinGroup_Limit },
  #endif
  #ifdef C_LIMIT_PIN_MAX
    { .id = Input_LimitC_Max,   .port = C_LIMIT_PORT_MAX,  .pin = C_LIMIT_PIN_MAX,   .group = PinGroup_Limit },
  #endif
  #if KEYPAD_ENABLE
    { .id = Input_KeypadStrobe, .port = KEYPAD_PORT,       .pin = KEYPAD_PIN,        .group = PinGroup_Keypad },
  #endif
    // Aux input pins must be consecutive in this array
#ifdef AUXINPUT0_PIN
    { .id = Input_Aux0,         .port = AUXINPUT0_PORT,    .pin = AUXINPUT0_PIN,     .group = PinGroup_AuxInput },
#endif
#ifdef AUXINPUT1_PIN
    { .id = Input_Aux1,         .port = AUXINPUT1_PORT,    .pin = AUXINPUT1_PIN,     .group = PinGroup_AuxInput },
#endif
#ifdef AUXINPUT2_PIN
    { .id = Input_Aux2,         .port = AUXINPUT2_PORT,    .pin = AUXINPUT2_PIN,     .group = PinGroup_AuxInput }
#endif
};

static output_signal_t outputpin[] = {
    { .id = Output_StepX,           .port = X_STEP_PORT,                .pin = X_STEP_PIN,              .group = PinGroup_StepperStep },
#ifdef X2_STEP_PORT
    { .id = Output_StepX,           .port = X2_STEP_PORT,               .pin = X2_STEP_PIN,             .group = PinGroup_StepperStep },
#endif
    { .id = Output_StepY,           .port = Y_STEP_PORT,                .pin = Y_STEP_PIN,              .group = PinGroup_StepperStep },
#ifdef Y2_STEP_PORT
    { .id = Output_StepY,           .port = Y2_STEP_PORT,               .pin = Y2_STEP_PIN,             .group = PinGroup_StepperStep },
#endif
    { .id = Output_StepZ,           .port = Z_STEP_PORT,                .pin = Z_STEP_PIN,              .group = PinGroup_StepperStep },
#ifdef Z2_STEP_PORT
    { .id = Output_StepZ,           .port = Z2_STEP_PORT,               .pin = Z2_STEP_PIN,             .group = PinGroup_StepperStep },
#endif
#ifdef A_AXIS
    { .id = Output_StepA,           .port = A_STEP_PORT,                .pin = A_STEP_PIN,              .group = PinGroup_StepperStep },
#endif
#ifdef B_AXIS
    { .id = Output_StepB,           .port = B_STEP_PORT,                .pin = B_STEP_PIN,              .group = PinGroup_StepperStep },
#endif
#ifdef C_AXIS
    { .id = Output_StepC,           .port = C_STEP_PORT,                .pin = C_STEP_PIN,              .group = PinGroup_StepperStep },
#endif
    { .id = Output_DirX,            .port = X_DIRECTION_PORT,           .pin = X_DIRECTION_PIN,         .group = PinGroup_StepperDir },
#ifdef X2_DIRECTION_PIN
    { .id = Output_DirX_2,          .port = X2_DIRECTION_PORT,          .pin = X2_DIRECTION_PIN,        .group = PinGroup_StepperDir },
#endif
    { .id = Output_DirY,            .port = Y_DIRECTION_PORT,           .pin = Y_DIRECTION_PIN,         .group = PinGroup_StepperDir },
#ifdef Y2_DIRECTION_PIN2
    { .id = Output_DirY_2,          .port = Y2_DIRECTION_PORT,          .pin = Y2_DIRECTION_PIN,        .group = PinGroup_StepperDir },
#endif
    { .id = Output_DirZ,            .port = Z_DIRECTION_PORT,           .pin = Z_DIRECTION_PIN,         .group = PinGroup_StepperDir },
#ifdef Z2_DIRECTION_PIN
    { .id = Output_DirZ_2,          .port = Z2_DIRECTION_PORT,          .pin = X2_DIRECTION_PIN,        .group = PinGroup_StepperDir },
#endif
#ifdef A_AXIS
    { .id = Output_DirA,            .port = A_DIRECTION_PORT,           .pin = A_DIRECTION_PIN,         .group = PinGroup_StepperDir },
#endif
#ifdef B_AXIS
    { .id = Output_DirB,            .port = B_DIRECTION_PORT,           .pin = B_DIRECTION_PIN,         .group = PinGroup_StepperDir },
#endif
#ifdef C_AXIS
    { .id = Output_DirC,            .port = C_DIRECTION_PORT,           .pin = C_DIRECTION_PIN,         .group = PinGroup_StepperDir },
#endif
#if !TRINAMIC_ENABLE
#ifdef STEPPERS_ENABLE_PORT
    { .id = Output_StepperEnable,   .port = STEPPERS_ENABLE_PORT,       .pin = STEPPERS_ENABLE_PIN,     .group = PinGroup_StepperEnable },
#endif
#ifdef X_ENABLE_PORT
    { .id = Output_StepperEnableX,  .port = X_ENABLE_PORT,              .pin = X_ENABLE_PIN,            .group = PinGroup_StepperEnable },
#endif
#ifdef X2_ENABLE_PORT
    { .id = Output_StepperEnableX,  .port = X2_ENABLE_PORT,             .pin = X2_ENABLE_PIN,           .group = PinGroup_StepperEnable },
#endif
#ifdef Y_ENABLE_PORT
    { .id = Output_StepperEnableY,  .port = Y_ENABLE_PORT,              .pin = Y_ENABLE_PIN,            .group = PinGroup_StepperEnable },
#endif
#ifdef Y2_ENABLE_PORT
    { .id = Output_StepperEnableY,  .port = Y2_ENABLE_PORT,             .pin = Y2_ENABLE_PIN,           .group = PinGroup_StepperEnable },
#endif
#ifdef Z_ENABLE_PORT
    { .id = Output_StepperEnableZ,  .port = Z_ENABLE_PORT,              .pin = Z_ENABLE_PIN,            .group = PinGroup_StepperEnable },
#endif
#ifdef Z2_ENABLE_PORT
    { .id = Output_StepperEnableZ,  .port = Z2_ENABLE_PORT,             .pin = Z2_ENABLE_PIN,           .group = PinGroup_StepperEnable },
#endif
#ifdef A_ENABLE_PORT
    { .id = Output_StepperEnableA,  .port = A_ENABLE_PORT,              .pin = A_ENABLE_PIN,            .group = PinGroup_StepperEnable },
#endif
#ifdef B_ENABLE_PORT
    { .id = Output_StepperEnableB,  .port = B_ENABLE_PORT,              .pin = B_ENABLE_PIN,            .group = PinGroup_StepperEnable },
#endif
#ifdef C_ENABLE_PORT
    { .id = Output_StepperEnableC,  .port = C_ENABLE_PORT,              .pin = C_ENABLE_PIN,            .group = PinGroup_StepperEnable },
#endif
#endif
#if !VFD_SPINDLE
#ifdef SPINDLE_ENABLE_PIN
    { .id = Output_SpindleOn,       .port = SPINDLE_ENABLE_PORT,        .pin = SPINDLE_ENABLE_PIN,      .group = PinGroup_SpindleControl },
#endif
#ifdef SPINDLE_DIRECTION_PIN
    { .id = Output_SpindleDir,      .port = SPINDLE_DIRECTION_PORT,     .pin = SPINDLE_DIRECTION_PIN,   .group = PinGroup_SpindleControl },
#endif
#endif
#ifdef COOLANT_FLOOD_PIN
    { .id = Output_CoolantFlood,    .port = COOLANT_FLOOD_PORT,         .pin = COOLANT_FLOOD_PIN,       .group = PinGroup_Coolant },
#endif
#ifdef COOLANT_MIST_PIN
    { .id = Output_CoolantMist,     .port = COOLANT_MIST_PORT,          .pin = COOLANT_MIST_PIN,        .group = PinGroup_Coolant },
#endif
#ifdef SD_CS_PORT
    { .id = Output_SdCardCS,        .port = SD_CS_PORT,                 .pin = SD_CS_PIN,               .group = PinGroup_SdCard },
#endif
#ifdef AUXOUTPUT0_PORT
    { .id = Output_Aux0,            .port = AUXOUTPUT0_PORT,            .pin = AUXOUTPUT0_PIN,          .group = PinGroup_AuxOutput },
#endif
#ifdef AUXOUTPUT1_PORT
    { .id = Output_Aux1,            .port = AUXOUTPUT1_PORT,            .pin = AUXOUTPUT1_PIN,          .group = PinGroup_AuxOutput },
#endif
#ifdef AUXOUTPUT2_PORT
    { .id = Output_Aux2,            .port = AUXOUTPUT2_PORT,            .pin = AUXOUTPUT2_PIN,          .group = PinGroup_AuxOutput }
#endif
};

static uint32_t vectorTable[sizeof(DeviceVectors) / sizeof(uint32_t)] __attribute__(( aligned (0x100ul) ));

static void spindle_set_speed (uint_fast16_t pwm_value);

static void SysTick_IRQHandler (void);
static void STEPPER_IRQHandler (void);
static void STEP_IRQHandler (void);
static void STEPDELAY_IRQHandler (void);
static void PIOA_IRQHandler (void);
static void PIOB_IRQHandler (void);
static void PIOC_IRQHandler (void);
static void PIOD_IRQHandler (void);
static void DEBOUNCE_IRQHandler (void);

extern void Dummy_Handler(void);

inline __attribute__((always_inline)) void IRQRegister(int32_t IRQnum, void (*IRQhandler)(void))
{
    vectorTable[IRQnum + 16] = (uint32_t)IRQhandler;
}

void IRQUnRegister(int32_t IRQnum)
{
    vectorTable[IRQnum + 16] = (uint32_t)Dummy_Handler;
}

static void driver_delay_ms (uint32_t ms, void (*callback)(void))
{
    if((delay_ms.ms = ms) > 0) {
        SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
        if(!(delay_ms.callback = callback))
            while(delay_ms.ms);
    } else if(callback)
        callback();
}

static bool selectStream (const io_stream_t *stream)
{
    static stream_type_t active_stream = StreamType_Serial;

    if(!stream)
        stream = serial_stream;

    if(hal.stream.read != stream->read)
        stream->reset_read_buffer();

    memcpy(&hal.stream, stream, sizeof(io_stream_t));

    if(!hal.stream.write_all)
        hal.stream.write_all = hal.stream.write;

    hal.stream.set_enqueue_rt_handler(protocol_enqueue_realtime_command);

    active_stream = hal.stream.type;

    return stream->type == hal.stream.type;
}

#if PLASMA_ENABLE

static axes_signals_t pulse_output = {0};

void stepperOutputStep (axes_signals_t step_outbits, axes_signals_t dir_outbits)
{
    pulse_output = step_outbits;
    dir_outbits.value ^= settings.steppers.dir_invert.mask;

    BITBAND_PERI(Z_DIRECTION_PORT->PIO_ODSR, Z_DIRECTION_PIN) = dir_outbits.z;
}

#endif

// Set stepper pulse output pins

#ifdef SQUARING_ENABLED

inline static __attribute__((always_inline)) void set_step_outputs (axes_signals_t step_outbits_1)
{
    axes_signals_t step_outbits_2;

#if PLASMA_ENABLE
    step_outbits_1.value |= pulse_output.value;
    pulse_output.value = 0;
#endif

    step_outbits_2.mask = (step_outbits_1.mask & motors_2.mask) ^ settings.steppers.step_invert.mask;
    step_outbits_1.mask = (step_outbits_1.mask & motors_1.mask) ^ settings.steppers.step_invert.mask;

    BITBAND_PERI(X_STEP_PORT->PIO_ODSR, X_STEP_PIN) = step_outbits_1.x;
  #ifdef X2_STEP_PIN
    BITBAND_PERI(X2_STEP_PORT->PIO_ODSR, X2_STEP_PIN) = step_outbits_2.x;
  #endif

    BITBAND_PERI(Y_STEP_PORT->PIO_ODSR, Y_STEP_PIN) = step_outbits_1.y;
  #ifdef Y2_STEP_PIN
    BITBAND_PERI(Y2_STEP_PORT->PIO_ODSR, Y2_STEP_PIN) = step_outbits_2.y;
  #endif
 
    BITBAND_PERI(Z_STEP_PORT->PIO_ODSR, Z_STEP_PIN) = step_outbits_1.z;
  #ifdef Z2_STEP_PIN
    BITBAND_PERI(Z2_STEP_PORT->PIO_ODSR, Z2_STEP_PIN) = step_outbits_2.z;
  #endif

  #ifdef A_STEP_PIN
    BITBAND_PERI(A_STEP_PORT->PIO_ODSR, A_STEP_PIN) = step_outbits_1.a;
  #endif
  #ifdef B_STEP_PIN
    BITBAND_PERI(B_STEP_PORT->PIO_ODSR, B_STEP_PIN) = step_outbits_1.b;
  #endif
  #ifdef C_STEP_PIN
    BITBAND_PERI(C_STEP_PORT->PIO_ODSR, C_STEP_PIN) = step_outbits_1.c;
  #endif
}

static axes_signals_t getAutoSquaredAxes (void)
{
    axes_signals_t ganged = {0};

#if X_AUTO_SQUARE
    ganged.x = On;
#endif
#if Y_AUTO_SQUARE
    ganged.y = On;
#endif
#if Z_AUTO_SQUARE
    ganged.z = On;
#endif

    return ganged;
}

// Enable/disable motors for auto squaring of ganged axes
static void StepperDisableMotors (axes_signals_t axes, squaring_mode_t mode)
{
    motors_1.mask = (mode == SquaringMode_A || mode == SquaringMode_Both ? axes.mask : 0) ^ AXES_BITMASK;
    motors_2.mask = (mode == SquaringMode_B || mode == SquaringMode_Both ? axes.mask : 0) ^ AXES_BITMASK;
}

#else // SQUARING DISABLED

inline static void __attribute__((always_inline)) set_step_outputs (axes_signals_t step_outbits)
{
#if PLASMA_ENABLE
    step_outbits.value |= pulse_output.value;
    pulse_output.value = 0;
#endif

    step_outbits.value ^= settings.steppers.step_invert.mask;

    BITBAND_PERI(X_STEP_PORT->PIO_ODSR, X_STEP_PIN) = step_outbits.x;
  #ifdef X2_STEP_PIN
    BITBAND_PERI(X2_STEP_PORT->PIO_ODSR, X2_STEP_PIN) = step_outbits.x;
  #endif
    
    BITBAND_PERI(Y_STEP_PORT->PIO_ODSR, Y_STEP_PIN) = step_outbits.y;
  #ifdef Y2_STEP_PIN
    BITBAND_PERI(Y2_STEP_PORT->PIO_ODSR, Y2_STEP_PIN) = step_outbits.y;
  #endif

    BITBAND_PERI(Z_STEP_PORT->PIO_ODSR, Z_STEP_PIN) = step_outbits.z;
  #ifdef Z2_STEP_PIN
    BITBAND_PERI(Z2_STEP_PORT->PIO_ODSR, Z2_STEP_PIN) = step_outbits.z;
  #endif 

  #ifdef A_STEP_PIN
    BITBAND_PERI(A_STEP_PORT->PIO_ODSR, A_STEP_PIN) = step_outbits.a;
  #endif
  #ifdef B_STEP_PIN
    BITBAND_PERI(B_STEP_PORT->PIO_ODSR, B_STEP_PIN) = step_outbits.b;
  #endif
  #ifdef C_STEP_PIN
    BITBAND_PERI(C_STEP_PORT->PIO_ODSR, C_STEP_PIN) = step_outbits.c;
  #endif
}

#endif

// Set stepper direction output pins
inline static __attribute__((always_inline)) void set_dir_outputs (axes_signals_t dir_outbits)
{
    dir_outbits.value ^= settings.steppers.dir_invert.mask;

    BITBAND_PERI(X_DIRECTION_PORT->PIO_ODSR, X_DIRECTION_PIN) = dir_outbits.x;
  #ifdef X2_DIRECTION_PIN
    BITBAND_PERI(X2_DIRECTION_PORT->PIO_ODSR, X2_DIRECTION_PIN) = dir_outbits.x;
  #endif

    BITBAND_PERI(Y_DIRECTION_PORT->PIO_ODSR, Y_DIRECTION_PIN) = dir_outbits.y;
  #ifdef Y2_DIRECTION_PIN
    BITBAND_PERI(Y2_DIRECTION_PORT->PIO_ODSR, Y2_DIRECTION_PIN) = dir_outbits.y;
  #endif

    BITBAND_PERI(Z_DIRECTION_PORT->PIO_ODSR, Z_DIRECTION_PIN) = dir_outbits.z;
  #ifdef Z2_DIRECTION_PIN
    BITBAND_PERI(Z2_DIRECTION_PORT->PIO_ODSR, Z2_DIRECTION_PIN) = dir_outbits.z;
  #endif

  #ifdef A_STEP_PIN
    BITBAND_PERI(A_DIRECTION_PORT->PIO_ODSR, A_DIRECTION_PIN) = dir_outbits.a;
  #endif
  #ifdef B_STEP_PIN
    BITBAND_PERI(B_DIRECTION_PORT->PIO_ODSR, B_DIRECTION_PIN) = dir_outbits.b;
  #endif
  #ifdef C_STEP_PIN
    BITBAND_PERI(C_DIRECTION_PORT->PIO_ODSR, C_DIRECTION_PIN) = dir_outbits.c;
  #endif
}

// Enable/disable stepper motors
static void stepperEnable (axes_signals_t enable)
{
    enable.value ^= settings.steppers.enable_invert.mask;
#if TRINAMIC_ENABLE == 2130 && TRINAMIC_I2C
    trinamic_stepper_enable(enable);
#else
    BITBAND_PERI(X_ENABLE_PORT->PIO_ODSR, X_ENABLE_PIN) = enable.x;
  #ifdef X2_ENABLE_PIN
    BITBAND_PERI(X2_ENABLE_PORT->PIO_ODSR, X2_ENABLE_PIN) = enable.x;
  #endif
  #ifdef Y_ENABLE_PIN
    BITBAND_PERI(Y_ENABLE_PORT->PIO_ODSR, Y_ENABLE_PIN) = enable.y;
  #endif
  #ifdef Y2_ENABLE_PIN
    BITBAND_PERI(Y2_ENABLE_PORT->PIO_ODSR, Y2_ENABLE_PIN) = enable.y;
  #endif
  #ifdef Z_ENABLE_PIN
    BITBAND_PERI(Z_ENABLE_PORT->PIO_ODSR, Z_ENABLE_PIN) = enable.z;
  #endif
  #ifdef Z2_ENABLE_PIN
    BITBAND_PERI(Z2_ENABLE_PORT->PIO_ODSR, Z2_ENABLE_PIN) = enable.z;
  #endif
  #ifdef A_ENABLE_PIN
    BITBAND_PERI(A_ENABLE_PORT->PIO_ODSR, A_ENABLE_PIN) = enable.a;
  #endif
  #ifdef B_ENABLE_PIN
    BITBAND_PERI(B_ENABLE_PORT->PIO_ODSR, B_ENABLE_PIN) = enable.b;
  #endif
  #ifdef C_ENABLE_PIN
    BITBAND_PERI(C_ENABLE_PORT->PIO_ODSR, C_ENABLE_PIN) = enable.c;
  #endif
#endif
}

// Resets and enables stepper driver ISR timer
static void stepperWakeUp (void)
{
    stepperEnable((axes_signals_t){AXES_BITMASK});

    STEPPER_TIMER.TC_RC = 1000;
    STEPPER_TIMER.TC_CCR = TC_CCR_CLKEN|TC_CCR_SWTRG;
}

// Disables stepper driver interrupts
static void stepperGoIdle (bool clear_signals)
{
    STEPPER_TIMER.TC_CCR = TC_CCR_CLKDIS;

    if(clear_signals) {
        set_step_outputs((axes_signals_t){0});
        set_dir_outputs((axes_signals_t){0});
    }
}

// Sets up stepper driver interrupt timeout, AMASS version
static void stepperCyclesPerTick (uint32_t cycles_per_tick)
{
    STEPPER_TIMER.TC_CCR = TC_CCR_CLKDIS;
// Limit min steps/s to about 2 (hal.f_step_timer @ 20MHz)
#ifdef ADAPTIVE_MULTI_AXIS_STEP_SMOOTHING
    STEPPER_TIMER.TC_RC = cycles_per_tick < (1UL << 18) ? cycles_per_tick : (1UL << 18) - 1UL;
#else
    STEPPER_TIMER.TC_RC = cycles_per_tick < (1UL << 23) ? cycles_per_tick : (1UL << 23) - 1UL;
#endif
    STEPPER_TIMER.TC_CCR = TC_CCR_CLKEN|TC_CCR_SWTRG;
}

// Sets stepper direction and pulse pins and starts a step pulse.
static void stepperPulseStart (stepper_t *stepper)
{
    if(stepper->dir_change)
        set_dir_outputs(stepper->dir_outbits);

    if(stepper->step_outbits.value) {
        set_step_outputs(stepper->step_outbits);
        STEP_TIMER.TC_CCR = TC_CCR_SWTRG;
    }
}

// Start a stepper pulse, delay version.
// Note: delay is only added when there is a direction change and a pulse to be output.
static void stepperPulseStartDelayed (stepper_t *stepper)
{
    if(stepper->dir_change) {

        set_dir_outputs(stepper->dir_outbits);

        if(stepper->step_outbits.value) {

            next_step_outbits = stepper->step_outbits; // Store out_bits

            IRQRegister(STEP_TIMER_IRQn, STEPDELAY_IRQHandler);

            STEP_TIMER.TC_RC = pulse_delay;
            STEP_TIMER.TC_CCR = TC_CCR_SWTRG;
        }

        return;
    }

    if(stepper->step_outbits.value) {
        set_step_outputs(stepper->step_outbits);
        STEP_TIMER.TC_CCR = TC_CCR_SWTRG;
    }
}

// Returns limit state as an limit_signals_t variable.
// Each bitfield bit indicates an axis limit, where triggered is 1 and not triggered is 0.
inline static limit_signals_t limitsGetState (void)
{
    limit_signals_t signals = {0};

    signals.min.mask = signals.max.mask = settings.limits.invert.mask;
#ifdef DUAL_LIMIT_SWITCHES
    signals.min2.mask = settings.limits.invert.mask;
#endif
#ifdef MAX_LIMIT_SWITCHES
    signals.max.mask = settings.limits.invert.mask;
#endif
    
    signals.min.x = BITBAND_PERI(X_LIMIT_PORT->PIO_PDSR, X_LIMIT_PIN);
    signals.min.y = BITBAND_PERI(Y_LIMIT_PORT->PIO_PDSR, Y_LIMIT_PIN);
    signals.min.z = BITBAND_PERI(Z_LIMIT_PORT->PIO_PDSR, Z_LIMIT_PIN);
#ifdef A_LIMIT_PIN
    signals.min.a = BITBAND_PERI(A_LIMIT_PORT->PIO_PDSR, A_LIMIT_PIN);
#endif
#ifdef B_LIMIT_PIN
    signals.min.b = BITBAND_PERI(B_LIMIT_PORT->PIO_PDSR, B_LIMIT_PIN);
#endif
#ifdef C_LIMIT_PIN
    signals.min.c = BITBAND_PERI(C_LIMIT_PORT->PIO_PDSR, C_LIMIT_PIN);
#endif

#ifdef X2_LIMIT_PIN
    signals.min2.x = BITBAND_PERI(X2_LIMIT_PORT->PIO_PDSR, X2_LIMIT_PIN);
#endif
#ifdef Y2_LIMIT_PIN
    signals.min2.y = BITBAND_PERI(Y2_LIMIT_PORT->PIO_PDSR, Y2_LIMIT_PIN);
#endif
#ifdef Z2_LIMIT_PIN
    signals.min2.x = BITBAND_PERI(Z2_LIMIT_PORT->PIO_PDSR, Z2_LIMIT_PIN);
#endif

#ifdef X_LIMIT_PIN_MAX
    signals.max.x = BITBAND_PERI(X_LIMIT_PORT_MAX->PIO_PDSR, X_LIMIT_PIN_MAX);
#endif
#ifdef Y_LIMIT_PIN_MAX
    signals.max.y = BITBAND_PERI(Y_LIMIT_PORT_MAX->PIO_PDSR, Y_LIMIT_PIN_MAX);
#endif
#ifdef Z_LIMIT_PIN_MAX
    signals.max.z = BITBAND_PERI(Z_LIMIT_PORT_MAX->PIO_PDSR, Z_LIMIT_PIN_MAX);
#endif
#ifdef A_LIMIT_PIN_MAX
    signals.max.a = BITBAND_PERI(A_LIMIT_PORT_MAX->PIO_PDSR, A_LIMIT_PIN_MAX);
#endif
#ifdef B_LIMIT_PIN_MAX
    signals.max.b = BITBAND_PERI(B_LIMIT_PORT_MAX->PIO_PDSR, B_LIMIT_PIN_MAX);
#endif
#ifdef C_LIMIT_PIN_MAX
    signals.max.c = BITBAND_PERI(C_LIMIT_PORT_MAX->PIO_PDSR, C_LIMIT_PIN_MAX);
#endif

    if (settings.limits.invert.mask) {
        signals.min.value ^= settings.limits.invert.mask;
#ifdef DUAL_LIMIT_SWITCHES
        signals.min2.mask ^= settings.limits.invert.mask;
#endif
#ifdef MAX_LIMIT_SWITCHES
        signals.max.value ^= settings.limits.invert.mask;
#endif
    }

    return signals;
}

// Enable/disable limit pins interrupt
static void limitsEnable (bool on, bool homing)
{
    uint32_t i = sizeof(inputpin) / sizeof(input_signal_t);

    on = on && settings.limits.flags.hard_enabled;

    do {
        if(inputpin[--i].group == PinGroup_Limit) {
            if(on)
                inputpin[i].port->PIO_IER = inputpin[i].bit;
            else
                inputpin[i].port->PIO_IDR = inputpin[i].bit;    
        }
    } while(i);

  #if TRINAMIC_ENABLE == 2130
    trinamic_homing(homing);
  #endif
}

// Returns system state as a control_signals_t variable.
// Each bitfield bit indicates a control signal, where triggered is 1 and not triggered is 0.
static control_signals_t systemGetState (void)
{
    control_signals_t signals = {0};

    signals.value = settings.control_invert.mask;

    #ifdef RESET_PIN
    signals.reset = BITBAND_PERI(RESET_PORT->PIO_PDSR, RESET_PIN);
  #endif
  #ifdef FEED_HOLD_PIN
    signals.feed_hold = BITBAND_PERI(FEED_HOLD_PORT->PIO_PDSR, FEED_HOLD_PIN);
  #endif
  #ifdef CYCLE_START_PIN
    signals.cycle_start = BITBAND_PERI(CYCLE_START_PORT->PIO_PDSR, CYCLE_START_PIN);
  #endif
  #ifdef SAFETY_DOOR_PIN
    signals.safety_door_ajar = BITBAND_PERI(SAFETY_DOOR_PORT->PIO_PDSR, SAFETY_DOOR_PIN);
  #endif

    if(settings.control_invert.mask)
        signals.value ^= settings.control_invert.mask;

    return signals;
}

#ifdef PROBE_PIN

// Sets up the probe pin invert mask to
// appropriately set the pin logic according to setting for normal-high/normal-low operation
// and the probing cycle modes for toward-workpiece/away-from-workpiece.
static void probeConfigure (bool is_probe_away, bool probing)
{
    probe.triggered = Off;
    probe.is_probing = probing;
    probe.inverted = is_probe_away ? !settings.probe.invert_probe_pin : settings.probe.invert_probe_pin;
}


// Returns the probe connected and triggered pin states.
probe_state_t probeGetState (void)
{
    probe_state_t state = {0};

    state.connected = probe.connected;
    state.triggered = BITBAND_PERI(PROBE_PORT->PIO_PDSR, PROBE_PIN) ^ probe.inverted;

    return state;
}

#endif

#ifndef VFD_SPINDLE

// Static spindle (off, on cw & on ccw)

inline static void spindle_off (void)
{
    BITBAND_PERI(SPINDLE_ENABLE_PORT->PIO_ODSR, SPINDLE_ENABLE_PIN) = settings.spindle.invert.on;
}

inline static void spindle_on (void)
{
    BITBAND_PERI(SPINDLE_ENABLE_PORT->PIO_ODSR, SPINDLE_ENABLE_PIN) = !settings.spindle.invert.on;
}

inline static void spindle_dir (bool ccw)
{
  #ifdef SPINDLE_DIRECTION_PIN
    if(hal.driver_cap.spindle_dir)
        BITBAND_PERI(SPINDLE_DIRECTION_PORT->PIO_ODSR, SPINDLE_DIRECTION_PIN) = (ccw ^ settings.spindle.invert.ccw);
  #endif
}

// Start or stop spindle
static void spindleSetState (spindle_state_t state, float rpm)
{
    (void)rpm;

    if (!state.on)
        spindle_off();
    else {
        spindle_dir(state.ccw);
        spindle_on();
    }
}

// Variable spindle control functions

// Sets spindle speed
static void spindle_set_speed (uint_fast16_t pwm_value)
{
    if (pwm_value == spindle_pwm.off_value) {
        pwmEnabled = false;
        if(settings.spindle.flags.pwm_action == SpindleAction_DisableWithZeroSPeed)
            spindle_off();
        if(spindle_pwm.always_on) {
            SPINDLE_PWM_TIMER.TC_RA = spindle_pwm.period - spindle_pwm.off_value;
            SPINDLE_PWM_TIMER.TC_CMR &= ~TC_CMR_CPCSTOP;
            SPINDLE_PWM_TIMER.TC_CCR = TC_CCR_CLKEN|TC_CCR_SWTRG;
        } else
            SPINDLE_PWM_TIMER.TC_CMR |= TC_CMR_CPCSTOP; // Ensure output is low, by setting timer to stop at TCC match
    } else {
        SPINDLE_PWM_TIMER.TC_RA = spindle_pwm.period == pwm_value ? 1 : spindle_pwm.period - pwm_value;
        if(!pwmEnabled) {
            spindle_on();
            pwmEnabled = true;
            SPINDLE_PWM_TIMER.TC_CMR &= ~TC_CMR_CPCSTOP;
            SPINDLE_PWM_TIMER.TC_CCR = TC_CCR_CLKEN|TC_CCR_SWTRG;
        }
    }
}

#ifdef SPINDLE_PWM_DIRECT

static uint_fast16_t spindleGetPWM (float rpm)
{
    return spindle_compute_pwm_value(&spindle_pwm, rpm, false);
}

#else

static void spindleUpdateRPM (float rpm)
{
    spindle_set_speed(spindle_compute_pwm_value(&spindle_pwm, rpm, false));
}

#endif

// Start or stop spindle
static void spindleSetStateVariable (spindle_state_t state, float rpm)
{
    if (!state.on || rpm == 0.0f) {
        spindle_set_speed(spindle_pwm.off_value);
        spindle_off();
    } else {
        spindle_dir(state.ccw);
        spindle_set_speed(spindle_compute_pwm_value(&spindle_pwm, rpm, false));
    }
}

// Returns spindle state in a spindle_state_t variable
static spindle_state_t spindleGetState (void)
{
    spindle_state_t state = {settings.spindle.invert.mask};

    state.on = BITBAND_PERI(SPINDLE_ENABLE_PORT->PIO_ODSR, SPINDLE_ENABLE_PIN) != 0;
  #ifdef SPINDLE_DIRECTION_PIN
    state.ccw = hal.driver_cap.spindle_dir && BITBAND_PERI(SPINDLE_DIRECTION_PORT->PIO_ODSR, SPINDLE_DIRECTION_PIN) != 0;
  #endif

    state.value ^= settings.spindle.invert.mask;
    if(pwmEnabled)
        state.on = On;

    return state;
}

// end spindle code

#endif

#ifdef DEBUGOUT
void debug_out (bool on)
{
    BITBAND_PERI(COOLANT_MIST_PIN, on);
}
#endif

// Start/stop coolant (and mist if enabled)
static void coolantSetState (coolant_state_t mode)
{
    mode.value ^= settings.coolant_invert.mask;

  #ifdef COOLANT_FLOOD_PIN
    BITBAND_PERI(COOLANT_FLOOD_PORT->PIO_ODSR, COOLANT_FLOOD_PIN) = mode.flood;
  #endif
  #ifdef COOLANT_MIST_PIN
    BITBAND_PERI(COOLANT_MIST_PORT->PIO_ODSR, COOLANT_MIST_PIN) = mode.mist;
  #endif
}

// Returns coolant state in a coolant_state_t variable
static coolant_state_t coolantGetState (void)
{
    coolant_state_t state = {0};

  #ifdef COOLANT_FLOOD_PIN
    state.flood = BITBAND_PERI(COOLANT_FLOOD_PORT->PIO_ODSR, COOLANT_FLOOD_PIN);
  #endif
  #ifdef COOLANT_MIST_PIN
    state.mist  = BITBAND_PERI(COOLANT_MIST_PORT->PIO_ODSR, COOLANT_MIST_PIN);
  #endif

    state.value ^= settings.coolant_invert.mask;

    return state;
}

// Helper functions for setting/clearing/inverting individual bits atomically (uninterruptable)
static void bitsSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    __disable_irq();
    *ptr |= bits;
    __enable_irq();
}

static uint_fast16_t bitsClearAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    __disable_irq();
    uint_fast16_t prev = *ptr;
    *ptr &= ~bits;
    __enable_irq();

    return prev;
}

static uint_fast16_t valueSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t value)
{
    __disable_irq();
    uint_fast16_t prev = *ptr;
    *ptr = value;
    __enable_irq();

    return prev;
}

static void PIO_Mode (Pio *port, uint32_t bit, bool mode)
{
    port->PIO_WPMR = PIO_WPMR_WPKEY(0x50494F);

    port->PIO_ABSR &= ~bit;

    if(mode == OUTPUT)
        port->PIO_OER = bit;
    else
        port->PIO_ODR = bit;

    port->PIO_PER = bit;
    port->PIO_WPMR = PIO_WPMR_WPKEY(0x50494F)|PIO_WPMR_WPEN;
}

void PIO_EnableInterrupt (const input_signal_t *input, pin_irq_mode_t irq_mode)
{
    input->port->PIO_IDR = input->bit;

    switch(irq_mode) {

        case IRQ_Mode_Falling:
            input->port->PIO_AIMER = input->bit;
            input->port->PIO_ESR = input->bit;
            input->port->PIO_FELLSR = input->bit;
            break;

        case IRQ_Mode_Rising:
            input->port->PIO_AIMER = input->bit;
            input->port->PIO_ESR = input->bit;
            input->port->PIO_REHLSR = input->bit;
            break;

        case IRQ_Mode_Change:
            input->port->PIO_AIMDR = input->bit;
            input->port->PIO_ESR = input->bit;
            break;

        default:
            break;
    }

    input->port->PIO_ISR; // Clear interrupts

    if(!(irq_mode == IRQ_Mode_None || input->group == PinGroup_Limit))
        input->port->PIO_IER = input->bit;
    else
        input->port->PIO_IDR = input->bit;
} 

static inline void PIO_InputMode (Pio *port, uint32_t bit, bool no_pullup)
{
    port->PIO_WPMR = PIO_WPMR_WPKEY(0x50494F);

    port->PIO_PER = bit;
    port->PIO_ODR = bit;

    if(no_pullup)
        port->PIO_PUDR = bit;
    else
        port->PIO_PUER = bit;

    port->PIO_WPMR = PIO_WPMR_WPKEY(0x50494F)|PIO_WPMR_WPEN;
}

// Configures perhipherals when settings are initialized or changed
void settings_changed (settings_t *settings)
{
    if(IOInitDone) {

        stepperEnable(settings->steppers.deenergize);

#ifdef SQUARING_ENABLED
        hal.stepper.disable_motors((axes_signals_t){0}, SquaringMode_Both);
#endif

      #ifndef VFD_SPINDLE
        if(hal.driver_cap.variable_spindle && spindle_precompute_pwm_values(&spindle_pwm, hal.f_step_timer)) {
            SPINDLE_PWM_TIMER.TC_RC = spindle_pwm.period;
            hal.spindle.set_state = spindleSetStateVariable;
        } else
            hal.spindle.set_state = spindleSetState;
      #endif

        pulse_length = (uint32_t)(42.0f * (settings->steppers.pulse_microseconds - STEP_PULSE_LATENCY)) - 1;

        if(hal.driver_cap.step_pulse_delay && settings->steppers.pulse_delay_microseconds > 0.0f) {
            int32_t delay = (uint32_t)(42.0f * (settings->steppers.pulse_delay_microseconds - 0.6f));
            pulse_delay = delay < 2 ? 2 : delay;
            hal.stepper.pulse_start = stepperPulseStartDelayed;
        } else
            hal.stepper.pulse_start = stepperPulseStart;

        IRQRegister(STEP_TIMER_IRQn, STEP_IRQHandler);
        STEP_TIMER.TC_RC = pulse_length;
        STEP_TIMER.TC_IER = TC_IER_CPCS; // Enable step end interrupt

        /****************************************
         *  Control, limit & probe pins config  *
         ****************************************/

        bool pullup;
        input_signal_t *input;

        control_signals_t control_fei;
        control_fei.mask = settings->control_disable_pullup.mask ^ settings->control_invert.mask;

        axes_signals_t limit_fei;
        limit_fei.mask = settings->limits.disable_pullup.mask ^ settings->limits.invert.mask;

        NVIC_DisableIRQ(PIOA_IRQn);
        NVIC_DisableIRQ(PIOB_IRQn);
        NVIC_DisableIRQ(PIOC_IRQn);
        NVIC_DisableIRQ(PIOD_IRQn);

        uint32_t i = sizeof(inputpin) / sizeof(input_signal_t), a = 0, b = 0, c = 0, d = 0;

        do {

            input = &inputpin[--i];

            pullup = false;
            input->bit = 1U << input->pin;
            input->irq_mode = IRQ_Mode_None;

            if(input->port != NULL) {

                switch(input->id) {

                    case Input_Reset:
                        pullup = !settings->control_disable_pullup.reset;
                        input->irq_mode = control_fei.reset ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;

                    case Input_FeedHold:
                        pullup = !settings->control_disable_pullup.feed_hold;
                        input->irq_mode = control_fei.feed_hold ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;

                    case Input_CycleStart:
                        pullup = !settings->control_disable_pullup.cycle_start;
                        input->irq_mode = control_fei.cycle_start ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;

                    case Input_SafetyDoor:     
                        pullup = !settings->control_disable_pullup.safety_door_ajar;
                        input->irq_mode = control_fei.safety_door_ajar ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;

                    case Input_Probe:
                        pullup = hal.driver_cap.probe_pull_up;
                        break;

                    case Input_LimitX:
                    case Input_LimitX_2:
                    case Input_LimitX_Max:
                        pullup = !settings->limits.disable_pullup.x;
                        input->irq_mode = limit_fei.x ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;

                    case Input_LimitY:
                    case Input_LimitY_2:
                    case Input_LimitY_Max:
                        pullup = !settings->limits.disable_pullup.y;
                        input->irq_mode = limit_fei.y ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;

                    case Input_LimitZ:
                    case Input_LimitZ_2:
                    case Input_LimitZ_Max:
                        pullup = !settings->limits.disable_pullup.z;
                        input->irq_mode = limit_fei.z ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;


                    case Input_LimitA:
                    case Input_LimitA_Max:
                        pullup = !settings->limits.disable_pullup.a;
                        input->irq_mode = limit_fei.a ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;

                    case Input_LimitB:
                    case Input_LimitB_Max:
                        pullup = !settings->limits.disable_pullup.b;
                        input->irq_mode = limit_fei.b ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;

                    case Input_LimitC:
                    case Input_LimitC_Max:
                        pullup = !settings->limits.disable_pullup.c;
                        input->irq_mode = limit_fei.c ? IRQ_Mode_Falling : IRQ_Mode_Rising;
                        break;

                    case Input_KeypadStrobe:
                        pullup = true;
                        input->irq_mode = IRQ_Mode_Change;
                        break;

                    default:
                        break;
                }

                if(input->group == PinGroup_AuxInput) {
                    pullup = true;
                    input->cap.pull_mode = (PullMode_Up|PullMode_Down);
                    input->cap.irq_mode = (IRQ_Mode_Rising|IRQ_Mode_Falling|IRQ_Mode_Change);
                }

                PIO_Mode(input->port, input->bit, INPUT);
                PIO_InputMode(input->port, input->bit, !pullup);
                PIO_EnableInterrupt(input, input->irq_mode);

                input->debounce = hal.driver_cap.software_debounce && !(input->group == PinGroup_Probe || input->group == PinGroup_Keypad || input->group == PinGroup_AuxInput);

                if(input->port == PIOA)
                    memcpy(&a_signals[a++], &inputpin[i], sizeof(input_signal_t));
                else if(input->port == PIOB)
                    memcpy(&b_signals[b++], &inputpin[i], sizeof(input_signal_t));
                else if(input->port == PIOC)
                    memcpy(&c_signals[c++], &inputpin[i], sizeof(input_signal_t));
                else if(input->port == PIOD)
                    memcpy(&d_signals[d++], &inputpin[i], sizeof(input_signal_t));
            }
        } while(i);

        // Clear and enable PIO interrupts
        if(a) {
            PIOA->PIO_ISR;
            IRQRegister(PIOA_IRQn, PIOA_IRQHandler);
            NVIC_ClearPendingIRQ(PIOA_IRQn);
            NVIC_EnableIRQ(PIOA_IRQn);
        }

        if(b) {
            PIOB->PIO_ISR;
            IRQRegister(PIOB_IRQn, PIOB_IRQHandler);
            NVIC_ClearPendingIRQ(PIOB_IRQn);
            NVIC_EnableIRQ(PIOB_IRQn);
        }

        if(c) {
            PIOC->PIO_ISR;
            IRQRegister(PIOC_IRQn, PIOC_IRQHandler);
            NVIC_ClearPendingIRQ(PIOC_IRQn);
            NVIC_EnableIRQ(PIOC_IRQn);
        }

        if(d) {
            PIOD->PIO_ISR;
            IRQRegister(PIOD_IRQn, PIOD_IRQHandler);
            NVIC_ClearPendingIRQ(PIOD_IRQn);
            NVIC_EnableIRQ(PIOD_IRQn);
        }

        if(settings->limits.flags.hard_enabled)
            hal.limits.enable(true, false);
    }
}

static char *port2char(Pio *port)
{
    static char s[2] = "?.";

    s[0] = 'A' + ((uint32_t)port - (uint32_t)PIOA) / ((uint32_t)PIOB - (uint32_t)PIOA);

    return s;
}

static void enumeratePins (bool low_level, pin_info_ptr pin_info)
{
    static xbar_t pin = {0};
    uint32_t i = sizeof(inputpin) / sizeof(input_signal_t);

    pin.mode.input = On;

    for(i = 0; i < sizeof(inputpin) / sizeof(input_signal_t); i++) {
        pin.pin = inputpin[i].pin;
        pin.function = inputpin[i].id;
        pin.group = inputpin[i].group;
        pin.port = low_level ? (void *)inputpin[i].port : (void *)port2char(inputpin[i].port);
        pin.description = inputpin[i].description;
        pin.mode.pwm = pin.group == PinGroup_SpindlePWM;

        pin_info(&pin);
    };

    pin.mode.mask = 0;
    pin.mode.output = On;

    for(i = 0; i < sizeof(outputpin) / sizeof(output_signal_t); i++) {
        pin.pin = outputpin[i].pin;
        pin.function = outputpin[i].id;
        pin.group = outputpin[i].group;
        pin.port = low_level ? (void *)outputpin[i].port : (void *)port2char(outputpin[i].port);
        pin.description = outputpin[i].description;

        pin_info(&pin);
    };
}

// Initializes MCU peripherals for Grbl use
static bool driver_setup (settings_t *settings)
{
    pmc_enable_periph_clk(ID_PIOA);
    pmc_enable_periph_clk(ID_PIOB);
    pmc_enable_periph_clk(ID_PIOC);
    pmc_enable_periph_clk(ID_PIOD);
    pmc_enable_periph_clk(ID_TC0);
    pmc_enable_periph_clk(ID_TC1);
    pmc_enable_periph_clk(ID_TC2);
    pmc_enable_periph_clk(ID_TC3);
    pmc_enable_periph_clk(ID_TC6);
    pmc_enable_periph_clk(ID_TC7);
    pmc_enable_periph_clk(ID_TC8);

    /*************************
     *  Output signals init  *
     *************************/

    uint32_t i;
    for(i = 0 ; i < sizeof(outputpin) / sizeof(output_signal_t); i++) {
        outputpin[i].bit = 1U << outputpin[i].pin;
        PIO_Mode(outputpin[i].port, outputpin[i].bit, OUTPUT);
    }

 // Stepper init

    TC0->TC_WPMR = TC_WPMR_WPKEY(0x54494D); //|TC_WPMR_WPEN;
    TC2->TC_WPMR = TC_WPMR_WPKEY(0x54494D); //|TC_WPMR_WPEN;

    STEPPER_TIMER.TC_CCR = TC_CCR_CLKDIS;
    STEPPER_TIMER.TC_CMR = TC_CMR_WAVE|TC_CMR_WAVSEL_UP_RC;
    STEPPER_TIMER.TC_IER = TC_IER_CPCS;
    STEPPER_TIMER.TC_CCR = TC_CCR_CLKEN;

    IRQRegister(STEPPER_TIMER_IRQn, STEPPER_IRQHandler);
    NVIC_EnableIRQ(STEPPER_TIMER_IRQn); // Enable stepper interrupt
    NVIC_SetPriority(STEPPER_TIMER_IRQn, 2);

    STEP_TIMER.TC_CCR = TC_CCR_CLKDIS;
    STEP_TIMER.TC_CMR = TC_CMR_WAVE|TC_CMR_WAVSEL_UP|TC_CMR_CPCSTOP;
    STEP_TIMER.TC_IER = TC_IER_CPCS;
    STEP_TIMER.TC_CCR = TC_CCR_CLKEN;

    IRQRegister(STEP_TIMER_IRQn, STEP_IRQHandler);
    NVIC_EnableIRQ(STEP_TIMER_IRQn);    // Enable stepper interrupts
    NVIC_SetPriority(STEP_TIMER_IRQn, 0);

  // Software debounce init

    if(hal.driver_cap.software_debounce) {

        DEBOUNCE_TIMER.TC_CCR = TC_CCR_CLKDIS;
        DEBOUNCE_TIMER.TC_CMR = TC_CMR_WAVE|TC_CMR_WAVSEL_UP|TC_CMR_CPCSTOP;
        DEBOUNCE_TIMER.TC_IER = TC_IER_CPCS;
        DEBOUNCE_TIMER.TC_RC  = 40000 * 42; // 40ms
        DEBOUNCE_TIMER.TC_CCR = TC_CCR_CLKEN;

        IRQRegister(DEBOUNCE_TIMER_IRQn, DEBOUNCE_IRQHandler);
        NVIC_SetPriority(STEP_TIMER_IRQn, 4);
        NVIC_EnableIRQ(DEBOUNCE_TIMER_IRQn);    // Enable debounce interrupt
    }

#ifndef VFD_SPINDLE

 // Spindle init

    SPINDLE_PWM_PORT->PIO_WPMR = PIO_WPMR_WPKEY(0x50494F);
    SPINDLE_PWM_PORT->PIO_ABSR |= (1 << SPINDLE_PWM_PIN);
    SPINDLE_PWM_PORT->PIO_PDR = (1 << SPINDLE_PWM_PIN);

#ifdef SPINDLE_PWM_CHANNEL
#error "Spindle PWM to be completed for this board!"
//    PWM->PWM_CLK = ;
//    PWM->PWM_ENA |= (1 << SPINDLE_PWM_CHANNEL);
//    PWM->PWM_CH_NUM[SPINDLE_PWM_CHANNEL].PWM_CPRD = ;
#else
    SPINDLE_PWM_TIMER.TC_CCR = TC_CCR_CLKDIS;
    SPINDLE_PWM_TIMER.TC_CMR = TC_CMR_WAVE|TC_CMR_WAVSEL_UP_RC|TC_CMR_ASWTRG_CLEAR|TC_CMR_ACPA_SET|TC_CMR_ACPC_CLEAR; //|TC_CMR_EEVT_XC0;
#endif

#endif

#if TRINAMIC_ENABLE == 2130
    trinamic_start(true);
#endif

 // Set defaults

#if N_AXIS > 3
    IOInitDone = settings->version == 20;
#else
    IOInitDone = settings->version == 19;
#endif

    hal.settings_changed(settings);
    hal.stepper.go_idle(true);
    hal.spindle.set_state((spindle_state_t){0}, 0.0f);
    hal.coolant.set_state((coolant_state_t){0});

#if SDCARD_ENABLE
    pinMode(SD_CD_PIN, INPUT_PULLUP);

// This does not work, the card detect pin is not interrupt capable(!) and inserting a card causes a hard reset...
// The bootloader needs modifying for it to work? Or perhaps the schematic is plain wrong?
// attachInterrupt(SD_CD_PIN, SD_IRQHandler, CHANGE);

    if(BITBAND_PERI(SD_CD_PIN) == 0)
        power_on();

    sdcard_init();
#endif

    return IOInitDone;
}

// EEPROM emulation - stores settings in flash
// Note: settings will not survive a reflash unless protected

typedef struct {
    void *addr;
    uint16_t page;
    uint16_t pages;
} nvs_storage_t;

static nvs_storage_t grblNVS;

bool nvsRead (uint8_t *dest)
{
    if(grblNVS.addr != NULL)
        memcpy(dest, grblNVS.addr, hal.nvs.size);

    return grblNVS.addr != NULL;
}

bool nvsWrite (uint8_t *source)
{
    uint16_t page = grblNVS.page;
    uint32_t size = grblNVS.pages;
    uint32_t *dest = (uint32_t *)grblNVS.addr, *src = (uint32_t *)source, words;

    while(!(EFC1->EEFC_FSR & EEFC_FSR_FRDY));

    while(size) {

        // Fill page buffer
        words = IFLASH1_PAGE_SIZE / sizeof(uint32_t);
        do {
            *dest++ = *src++;
        } while(--words);

        // Write page buffer to flash
        EFC1->EEFC_FCR = EEFC_FCR_FKEY(0x5A)|EEFC_FCR_FARG(page)|EEFC_FCR_FCMD(0x03);
        while(!(EFC1->EEFC_FSR & EEFC_FSR_FRDY));
        
        if(EFC1->EEFC_FSR & EEFC_FSR_FLOCKE)
            break;

        size--;
        page++;
    }

    return size == 0;
}

bool nvsInit (void)
{
    if(NVS_SIZE & ~(IFLASH1_PAGE_SIZE - 1))
        grblNVS.pages = (NVS_SIZE & ~(IFLASH1_PAGE_SIZE - 1)) / IFLASH1_PAGE_SIZE + 1;
    else
        grblNVS.pages = NVS_SIZE / IFLASH1_PAGE_SIZE;

//  grblNVS.row_size = IFLASH1_LOCK_REGION_SIZE; // 16K
    grblNVS.page = IFLASH1_NB_OF_PAGES - grblNVS.pages;
    grblNVS.addr = (void *)(IFLASH1_ADDR + IFLASH1_PAGE_SIZE * grblNVS.page);

    return true;
}

// End EEPROM emulation

#if USB_SERIAL_CDC
static void execute_realtime (uint_fast16_t state)
{
#if USB_SERIAL_CDC
    usb_execute_realtime(state);
#endif
}
#endif

// Initialize HAL pointers, setup serial comms and enable EEPROM
// NOTE: Grbl is not yet configured (from EEPROM data), driver_setup() will be called when done
bool driver_init (void)
{
    WDT_Disable (WDT);

    // Enable EEPROM and serial port here for Grbl to be able to configure itself and report any errors

    // Copy vector table to RAM so we can override the default Arduino IRQ assignments

    __disable_irq();

    memcpy(&vectorTable, (void *)SCB->VTOR, sizeof(vectorTable));

    SCB->VTOR = ((uint32_t)&vectorTable & SCB_VTOR_TBLOFF_Msk) | SCB_VTOR_TBLBASE_Msk;
    __DSB();
    __enable_irq();

    // End vector table copy

    SysTick->LOAD = (SystemCoreClock / 1000) - 1;
    SysTick->VAL = 0;
    SysTick->CTRL |= SysTick_CTRL_CLKSOURCE_Msk|SysTick_CTRL_TICKINT_Msk;

    IRQRegister(SysTick_IRQn, SysTick_IRQHandler);

//    NVIC_SetPriority(SysTick_IRQn, (1 << __NVIC_PRIO_BITS) - 1);
    NVIC_EnableIRQ(SysTick_IRQn);

    hal.info = "SAM3X8E";
	hal.driver_version = "210817";
#ifdef BOARD_NAME
    hal.board = BOARD_NAME;
#endif
    hal.driver_setup = driver_setup;
    hal.f_step_timer = SystemCoreClock / 2; // 42 MHz
    hal.rx_buffer_size = RX_BUFFER_SIZE;
    hal.delay_ms = driver_delay_ms;
    hal.settings_changed = settings_changed;

    hal.stepper.wake_up = stepperWakeUp;
    hal.stepper.go_idle = stepperGoIdle;
    hal.stepper.enable = stepperEnable;
    hal.stepper.cycles_per_tick = stepperCyclesPerTick;
    hal.stepper.pulse_start = stepperPulseStart;
#ifdef SQUARING_ENABLED
    hal.stepper.get_auto_squared = getAutoSquaredAxes;
    hal.stepper.disable_motors = StepperDisableMotors;
#endif

    hal.limits.enable = limitsEnable;
    hal.limits.get_state = limitsGetState;

    hal.coolant.set_state = coolantSetState;
    hal.coolant.get_state = coolantGetState;

#ifdef PROBE_PIN
    hal.probe.configure = probeConfigure;
    hal.probe.get_state = probeGetState;
#endif

#ifndef VFD_SPINDLE
    hal.spindle.set_state = spindleSetState;
    hal.spindle.get_state = spindleGetState;
  #ifdef SPINDLE_PWM_DIRECT
    hal.spindle.get_pwm = spindleGetPWM;
    hal.spindle.update_pwm = spindle_set_speed;
  #else
    hal.spindle.update_rpm = spindleUpdateRPM;
  #endif
#endif

    hal.control.get_state = systemGetState;

#if USB_SERIAL_CDC
    serial_stream = usb_serialInit();
    grbl.on_execute_realtime = execute_realtime;
#else
    serial_stream = serialInit(115200);
#endif

    hal.stream_select = selectStream;
    hal.stream_select(serial_stream);

#ifdef DEBUGOUT
    hal.debug_out = debug_out;
#endif

#if I2C_ENABLE
    i2c_init();
#endif

#if EEPROM_ENABLE
    i2c_eeprom_init();
#else
    if(nvsInit()) {
        hal.nvs.type = NVS_Flash;
        hal.nvs.memcpy_from_flash = nvsRead;
        hal.nvs.memcpy_to_flash = nvsWrite;
    } else
        hal.nvs.type = NVS_None;
#endif

    hal.irq_enable = __enable_irq;
    hal.irq_disable = __disable_irq;
    hal.set_bits_atomic = bitsSetAtomic;
    hal.clear_bits_atomic = bitsClearAtomic;
    hal.set_value_atomic = valueSetAtomic;
    hal.get_elapsed_ticks = millis;
    hal.enumerate_pins = enumeratePins;

 // driver capabilities, used for announcing and negotiating (with Grbl) driver functionality

#ifdef SAFETY_DOOR_PIN
    hal.signals_cap.safety_door_ajar = On;
#endif

#ifndef VFD_SPINDLE
  #ifdef SPINDLE_DIRECTION_PIN
    hal.driver_cap.spindle_dir = On;
  #endif
    hal.driver_cap.variable_spindle = On;
#endif
#ifdef COOLANT_MIST_PIN
    hal.driver_cap.mist_control = On;
#endif
    hal.driver_cap.software_debounce = On;
    hal.driver_cap.step_pulse_delay = On;
    hal.driver_cap.amass_level = 3;
    hal.driver_cap.control_pull_up = On;
    hal.driver_cap.limits_pull_up = On;
    hal.driver_cap.probe_pull_up = On;

#ifdef HAS_IOPORTS
    uint32_t i;
    input_signal_t *input;
    static pin_group_pins_t aux_inputs = {0}, aux_outputs = {0};

    for(i = 0 ; i < sizeof(inputpin) / sizeof(input_signal_t); i++) {
        input = &inputpin[i];
        if(input->group == PinGroup_AuxInput) {
            if(aux_inputs.pins.inputs == NULL)
                aux_inputs.pins.inputs = input;
            aux_inputs.n_pins++;
        }
    }

    output_signal_t *output;
    for(i = 0 ; i < sizeof(outputpin) / sizeof(output_signal_t); i++) {
        output = &outputpin[i];
        if(output->group == PinGroup_AuxOutput) {
            if(aux_outputs.pins.outputs == NULL)
                aux_outputs.pins.outputs = output;
            aux_outputs.n_pins++;
        }
    }

    ioports_init(&aux_inputs, &aux_outputs);
#endif

#if SPINDLE_HUANYANG
    huanyang_init(modbus_init(serial2Init(19200), NULL));
#endif

#if BLUETOOTH_ENABLE
 #if BLUETOOTH_ENABLE == 2
    bluetooth_init(serial2Init(115200));
 #else
  #if USB_SERIAL_CDC
    bluetooth_init(serialInit(115200));
  #else
    bluetooth_init(serial_stream);
  #endif
 #endif
#endif

#if PLASMA_ENABLE
    hal.stepper.output_step = stepperOutputStep;
    plasma_init();
#endif

#include "grbl/plugins_init.h"

    // No need to move version check before init.
    // Compiler will fail any signature mismatch for existing entries.
    return hal.version == 8;
}

/* interrupt handlers */

// Main stepper driver
static void STEPPER_IRQHandler (void)
{
    if(STEPPER_TIMER.TC_SR & STEPPER_TIMER.TC_IMR)
        hal.stepper.interrupt_callback();
}

// Step output off
static void STEP_IRQHandler (void)
{
    if(STEP_TIMER.TC_SR & STEP_TIMER.TC_IMR)
        set_step_outputs((axes_signals_t){0});  
}

// Step output on/off
static void STEPDELAY_IRQHandler (void)
{
    if(STEP_TIMER.TC_SR & STEP_TIMER.TC_IMR) {

        set_step_outputs(next_step_outbits);

        IRQRegister(STEP_TIMER_IRQn, STEP_IRQHandler);

        STEP_TIMER.TC_RC = pulse_length;
        STEP_TIMER.TC_CCR = TC_CCR_SWTRG;
    }
}

inline static bool enqueue_debounce (input_signal_t *signal)
{
    bool ok;
    uint_fast8_t bptr = (debounce_queue.head + 1) & (DEBOUNCE_QUEUE - 1);

    if((ok = bptr != debounce_queue.tail)) {
        debounce_queue.signal[debounce_queue.head] = signal;
        debounce_queue.head = bptr;
    }

    return ok;
}

// Returns NULL if no debounce checks enqueued
inline static input_signal_t *get_debounce (void)
{
    input_signal_t *signal = NULL;
    uint_fast8_t bptr = debounce_queue.tail;

    if(bptr != debounce_queue.head) {
        signal = debounce_queue.signal[bptr++];
        debounce_queue.tail = bptr & (DEBOUNCE_QUEUE - 1);
    }

    return signal;
}

static void DEBOUNCE_IRQHandler (void)
{
    input_signal_t *signal;

    DEBOUNCE_TIMER.TC_SR;

    while((signal = get_debounce())) {

        signal->port->PIO_ISR;
        signal->port->PIO_IER = signal->bit;

        if((signal->port->PIO_PDSR & signal->bit) == (signal->irq_mode == IRQ_Mode_Falling ? 0 : signal->bit))
          switch(signal->group) {

            case PinGroup_Limit:
                hal.limits.interrupt_callback(limitsGetState());
                break;

            case PinGroup_Control:
                hal.control.interrupt_callback(systemGetState());
                break;
        }
    }
}

inline static void PIO_IRQHandler (input_signal_t *signals, uint32_t isr)
{
    bool debounce = false;
    uint32_t grp = 0, i = 0;

    while(signals[i].port != NULL) {
        if(isr & signals[i].bit) {
            if(signals[i].debounce && enqueue_debounce(&signals[i])) {
                signals[i].port->PIO_IDR = signals[i].bit;
                debounce = true;
            } else switch(signals[i].group) {

                case PinGroup_AuxInput:
                    ioports_event(&signals[i]);
                    break;
#if KEYPAD_ENABLE
                case PinGroup_Keypad:
                    keypad_keyclick_handler(!BITBAND_PERI(KEYPAD_PORT->PIO_PDSR, KEYPAD_PIN));
                    break;
#endif
                default:
                    grp |= signals[i].group;
                    break;
            }
        }
        i++;
    }

    if(debounce)
        DEBOUNCE_TIMER.TC_CCR = TC_CCR_SWTRG;

    if(grp & PinGroup_Limit) {
        limit_signals_t state = limitsGetState();
        if(limit_signals_merge(state).value) //TODO: add check for limit switches having same state as when limit_isr were invoked?
            hal.limits.interrupt_callback(state);
    }

    if(grp & PinGroup_Control)
        hal.control.interrupt_callback(systemGetState());
}

static void PIOA_IRQHandler (void)
{
    PIO_IRQHandler(a_signals, PIOA->PIO_ISR);
}

static void PIOB_IRQHandler (void)
{
    PIO_IRQHandler(b_signals, PIOB->PIO_ISR);
}

static void PIOC_IRQHandler (void)
{
    PIO_IRQHandler(c_signals, PIOC->PIO_ISR);
}

static void PIOD_IRQHandler (void)
{
    PIO_IRQHandler(d_signals, PIOD->PIO_ISR);
}

// Interrupt handler for 1 ms interval timer
static void SysTick_IRQHandler (void)
{
    SysTick_Handler();

#if SDCARD_ENABLE
    static uint32_t fatfs_ticks = 10;
    if(!(--fatfs_ticks)) {
        disk_timerproc();
        fatfs_ticks = 10;
    }
#endif

    if(delay_ms.ms && !(--delay_ms.ms)) {
        if(delay_ms.callback) {
            delay_ms.callback();
            delay_ms.callback = NULL;
        }
    }
}
