#ifndef POWER_H_
#define POWER_H_

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

esp_err_t power_init(void);
esp_err_t power_shutdown(void);

esp_err_t power_backlight_on(void);
esp_err_t power_backlight_off(void);

esp_err_t power_screen_on(void);
esp_err_t power_screen_off(void);

esp_err_t power_camera_on(void);
esp_err_t power_camera_off(void);

uint16_t power_get_vbat(void);
uint8_t power_get_battery_status(void);
bool power_get_battery_charging(void);
uint16_t power_get_ibat_charge(void);
uint16_t power_get_ibat_discharge(void);
uint16_t power_get_vusb(void);
uint16_t power_get_iusb(void);
uint16_t power_get_temp(void);

bool usb_connected(void);

#endif /* POWER_H_ */
