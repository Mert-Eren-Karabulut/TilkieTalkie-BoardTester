// VBUS-detect deep-sleep wake stub.
//
// Problem this solves: CE (charger enable) is firmware-controlled with a pull-up
// default of "charging DISABLED" (deliberate design choice — no CE pulldown on the
// board), and there is no hardware wake signal for USB plug-in (BQ25792 INT#
// unrouted, QON unwired). A device deep-sleeping on battery would therefore
// silently NOT charge when docked until a button press woke it.
//
// This stub lets the sleeping device notice USB by itself at ~µA average cost: a
// timer wake every WAKE period lands here (executing from RTC memory, before the
// bootloader), we bit-bang a single I2C read of the BQ25792 Charger Status 0
// register on the battery bus, and either fall through to a full boot (VBUS present:
// battery.begin() asserts CE, charging starts, and the next inactivity sleep takes
// the docked path whose global pad hold keeps charging through sleep) or re-enter
// deep sleep within a few milliseconds. Button/pogo (ext1) wakes pass straight
// through to a normal boot. ext1 arming persists across esp_wake_stub_sleep() — it
// only re-sets the stub entry and the sleep-enable bit (verified in
// esp_hw_support/sleep_wake_stub.c).
//
// Wake-stub rules honored (see IDF examples/system/deep_sleep_wake_stub): this whole
// file is placed in RTC memory by the linker's *rtc_wake_stub* filename rule
// (esp_system/ld/esp32s3/sections.ld.in), so code, rodata and statics are all
// RTC-resident; the only external calls are ROM (esp_rom_delay_us) or RTC-resident
// IDF wake-stub APIs. No flash access, no drivers, no logging (the USB-CDC console
// cannot run here anyway). The bootloader-armed RTC watchdog allows ~9s; a full
// stub pass takes ~2-3ms.
//
// Bus notes: battery I2C (SDA=GPIO1, SCL=GPIO2) has external 4.7k pullups on the
// always-on 3.3V rail, so the bus is alive during deep sleep. After a deep-sleep
// wake the digital GPIO/IO_MUX blocks are at reset defaults (out=0, outputs
// disabled, simple-GPIO routing), which is exactly what open-drain bit-banging
// needs: latch OUT at 0 and toggle only the output-enable bit.

#include <stdint.h>
#include <stdbool.h>

#include "esp_sleep.h"
#include "esp_wake_stub.h"
#include "esp_rom_sys.h"
#include "soc/rtc.h"          // RTC_TIMER_TRIG_EN
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/soc.h"

#include "rtc_wake_stub_vbus.h"

#define STUB_SDA_GPIO            1
#define STUB_SCL_GPIO            2
#define STUB_PIN_MASK            ((1UL << STUB_SDA_GPIO) | (1UL << STUB_SCL_GPIO))
#define STUB_BQ25792_ADDR        0x6B
#define STUB_REG_CHARGER_STATUS0 0x1B   // bit0 = VBUS_PRESENT_STAT
#define STUB_VBUS_PRESENT_BIT    0x01
#define STUB_HALF_PERIOD_US      5      // ~100kHz open-drain bit-bang
#define STUB_RECHECK_US          (20ULL * 1000ULL * 1000ULL)

// Open-drain emulation: OUT stays latched 0; "release" = output disabled (external
// pullup raises the line), "drive low" = output enabled.
static void stub_release(uint32_t gpio) { REG_WRITE(GPIO_ENABLE_W1TC_REG, 1UL << gpio); }
static void stub_drive_low(uint32_t gpio) { REG_WRITE(GPIO_ENABLE_W1TS_REG, 1UL << gpio); }
static uint32_t stub_level(uint32_t gpio) { return (REG_READ(GPIO_IN_REG) >> gpio) & 1UL; }
static void stub_dly(void) { esp_rom_delay_us(STUB_HALF_PERIOD_US); }

// One I2C clock pulse with the given SDA state; returns SDA sampled while SCL high.
// No clock-stretch handling: the BQ25792 does not stretch.
static uint32_t stub_clock_bit(uint32_t sda_high)
{
    if (sda_high) {
        stub_release(STUB_SDA_GPIO);
    } else {
        stub_drive_low(STUB_SDA_GPIO);
    }
    stub_dly();
    stub_release(STUB_SCL_GPIO);
    stub_dly();
    uint32_t bit = stub_level(STUB_SDA_GPIO);
    stub_drive_low(STUB_SCL_GPIO);
    return bit;
}

// Works both as START (bus idle) and repeated START (mid-transaction, SCL low).
static void stub_start(void)
{
    stub_release(STUB_SDA_GPIO);
    stub_dly();
    stub_release(STUB_SCL_GPIO);
    stub_dly();
    stub_drive_low(STUB_SDA_GPIO);
    stub_dly();
    stub_drive_low(STUB_SCL_GPIO);
    stub_dly();
}

static void stub_stop(void)
{
    stub_drive_low(STUB_SDA_GPIO);
    stub_dly();
    stub_release(STUB_SCL_GPIO);
    stub_dly();
    stub_release(STUB_SDA_GPIO);
    stub_dly();
}

// Returns true when the slave ACKed.
static bool stub_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        (void)stub_clock_bit((byte >> i) & 1U);
    }
    return stub_clock_bit(1) == 0; // SDA released for the slave's ACK
}

static uint8_t stub_read_byte_nack(void)
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++) {
        value = (uint8_t)((value << 1) | stub_clock_bit(1));
    }
    (void)stub_clock_bit(1); // NACK: single-byte read
    return value;
}

static bool stub_read_charger_status0(uint8_t *value)
{
    // Route both pads as simple GPIO with input buffers on; OUT latched low so the
    // output-enable bit alone implements open-drain.
    PIN_FUNC_SELECT(IO_MUX_GPIO1_REG, PIN_FUNC_GPIO);
    PIN_FUNC_SELECT(IO_MUX_GPIO2_REG, PIN_FUNC_GPIO);
    PIN_INPUT_ENABLE(IO_MUX_GPIO1_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO2_REG);
    REG_WRITE(GPIO_OUT_W1TC_REG, STUB_PIN_MASK);
    REG_WRITE(GPIO_ENABLE_W1TC_REG, STUB_PIN_MASK); // both released = bus idle
    stub_dly();

    // If a slave is holding SDA low (aborted pre-sleep transaction), clock it out.
    for (int i = 0; i < 9 && stub_level(STUB_SDA_GPIO) == 0; i++) {
        (void)stub_clock_bit(1);
    }
    if (stub_level(STUB_SDA_GPIO) == 0) {
        return false;
    }

    bool ok = false;
    stub_start();
    if (stub_write_byte((uint8_t)(STUB_BQ25792_ADDR << 1))) {
        if (stub_write_byte(STUB_REG_CHARGER_STATUS0)) {
            stub_start(); // repeated start
            if (stub_write_byte((uint8_t)((STUB_BQ25792_ADDR << 1) | 1U))) {
                *value = stub_read_byte_nack();
                ok = true;
            }
        }
    }
    stub_stop();
    REG_WRITE(GPIO_ENABLE_W1TC_REG, STUB_PIN_MASK); // leave both released
    return ok;
}

static void wake_stub_vbus_check(void)
{
    // Buttons/pogo (ext1) or anything that is not our poll timer: normal full boot.
    if ((esp_wake_stub_get_wakeup_cause() & RTC_TIMER_TRIG_EN) == 0) {
        esp_default_wake_deep_sleep();
        return;
    }

    uint8_t status0 = 0;
    if (stub_read_charger_status0(&status0) && (status0 & STUB_VBUS_PRESENT_BIT)) {
        // USB appeared while sleeping on battery: continue into a full boot.
        esp_default_wake_deep_sleep();
        return;
    }

    // Still on battery (or the read failed — stay conservative): back to sleep.
    esp_wake_stub_set_wakeup_time(STUB_RECHECK_US);
    esp_wake_stub_sleep(&wake_stub_vbus_check);
}

void wakeStubVbusArm(void)
{
    esp_sleep_enable_timer_wakeup(STUB_RECHECK_US);
    esp_set_deep_sleep_wake_stub(&wake_stub_vbus_check);
}
