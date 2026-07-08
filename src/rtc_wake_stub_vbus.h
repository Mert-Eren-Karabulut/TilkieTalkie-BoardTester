#ifndef RTC_WAKE_STUB_VBUS_H
#define RTC_WAKE_STUB_VBUS_H

#ifdef __cplusplus
extern "C" {
#endif

// Arms the VBUS-detect deep-sleep wake stub: a ~20s timer wake runs a few-ms stub
// (from RTC memory, before the bootloader) that bit-bangs one I2C read of the
// BQ25792's VBUS-present bit and either falls through to a full boot (USB appeared —
// begin() will assert CE and charging starts) or goes straight back to sleep.
// Average cost ~µA. Call ONLY before a BATTERY-powered deep sleep: docked sleeps arm
// the global pad hold, which freezes GPIO1/2 and would break the stub's I2C (and
// charging already continues there without it).
void wakeStubVbusArm(void);

#ifdef __cplusplus
}
#endif

#endif // RTC_WAKE_STUB_VBUS_H
