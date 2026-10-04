#include "board.h"
#include <Wire.h>
#include "board_pins.h"

// --- I2C ---
// L'interruption I2C est allouée sur le coeur qui appelle Wire.begin(). On le fait depuis le
// coeur 0 : sur le coeur 1, elle partage une ligne avec le LCD et les écritures sont parfois
// retardées de ~0,7 ms, ce qui fait varier la luminosité (PWM logicielle du rétroéclairage).
static void i2cInitTask(void *caller) {
  Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL, 400000);
  xTaskNotifyGive((TaskHandle_t)caller);
  vTaskDelete(nullptr);
}

void i2cBegin() {
  xTaskCreatePinnedToCore(i2cInitTask, "i2c_init", 4096, xTaskGetCurrentTaskHandle(), 5, nullptr, 0);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// --- CH422G : pas de registres, chaque fonction a sa propre adresse I2C ---
#define CH422G_ADDR_MODE  0x24
#define CH422G_ADDR_OUT   0x38
#define CH422G_MODE_IO_OUTPUT  0x01

static uint8_t exio_state = 0;
static SemaphoreHandle_t exio_mutex = xSemaphoreCreateMutex();

bool exioBegin() {
  Wire.beginTransmission(CH422G_ADDR_MODE);
  Wire.write(CH422G_MODE_IO_OUTPUT);
  return Wire.endTransmission() == 0;
}

// Appelé depuis loop() et depuis la tâche esp_timer (PWM du rétroéclairage)
void exioWrite(uint8_t pin, bool level) {
  xSemaphoreTake(exio_mutex, portMAX_DELAY);
  if (level) exio_state |= (1 << pin);
  else exio_state &= ~(1 << pin);
  Wire.beginTransmission(CH422G_ADDR_OUT);
  Wire.write(exio_state);
  Wire.endTransmission();
  xSemaphoreGive(exio_mutex);
}

// --- Rétroéclairage ---
// L'AP3032 n'a qu'une entrée CTRL, câblée sur l'expander I2C (EXIO2) : pas de PWM matérielle.
// On hache donc CTRL en logiciel, une écriture I2C par front, cadencée par esp_timer.
// Mesuré sur la carte : ~121 us par écriture avec l'I2C à 400 kHz, donc en dessous de ~5 %
// le temps d'allumage n'est plus tenu. Le bus I2C reçoit 2 écritures par période.
#define BL_PWM_HZ  250

static esp_timer_handle_t bl_period_timer, bl_off_timer;
static volatile uint8_t bl_percent = 100;

static void blPeriodCb(void *) {
  uint8_t p = bl_percent;
  if (p == 0) return;
  exioWrite(EXIO_LCD_BL, HIGH);
  if (p < 100) esp_timer_start_once(bl_off_timer, (1000000UL / BL_PWM_HZ) * p / 100);
}

static void blOffCb(void *) {
  exioWrite(EXIO_LCD_BL, LOW);
}

void backlightBegin() {
  esp_timer_create_args_t period = {.callback = blPeriodCb, .name = "bl_period"};
  esp_timer_create_args_t off = {.callback = blOffCb, .name = "bl_off"};
  esp_timer_create(&period, &bl_period_timer);
  esp_timer_create(&off, &bl_off_timer);
  esp_timer_start_periodic(bl_period_timer, 1000000UL / BL_PWM_HZ);
}

void backlightSet(uint8_t percent) {
  bl_percent = min<uint8_t>(percent, 100);
  if (bl_percent == 0) exioWrite(EXIO_LCD_BL, LOW);
}
