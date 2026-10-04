#include "board.h"
#include <Wire.h>
#include <driver/i2c_master.h>
#include "esp32-hal-i2c.h"
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

// Appelé depuis plusieurs tâches
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
// Allumer puis éteindre par deux écritures I2C séparées rend la durée d'allumage dépendante de
// l'ordonnanceur : mesuré Wi-Fi allumé, 116 à 1800 us au lieu de 528, d'où un scintillement.
// Les deux écritures partent donc dans UNE transaction (START, allumé, START, éteint, STOP) :
// le CH422G applique chaque octet dès qu'il le reçoit (vérifié sur la carte en relisant ses
// sorties au milieu d'une transaction), et la durée d'allumage est alors fixée par l'horloge
// I2C, générée par le matériel. On règle la luminosité en changeant la fréquence de cette horloge.
#define BL_PWM_HZ      250
#define BL_PULSE_BITS  19.5f  // périodes d'horloge I2C entre les deux fronts de la sortie

static i2c_master_dev_handle_t pulse_dev;
static volatile uint8_t bl_percent = 100;

// Accès au CH422G sans adresse gérée par le pilote, à la fréquence qui donne la durée voulue
static void setPulseClock(uint32_t hz) {
  if (pulse_dev) i2c_master_bus_rm_device(pulse_dev);
  pulse_dev = nullptr;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = I2C_DEVICE_ADDRESS_NOT_USED;
  cfg.scl_speed_hz = hz;
  i2c_master_bus_add_device((i2c_master_bus_handle_t)i2cBusHandle(0), &cfg, &pulse_dev);
}

static void sendPulse() {
  xSemaphoreTake(exio_mutex, portMAX_DELAY);
  if (pulse_dev) {
    uint8_t on[2] = {CH422G_ADDR_OUT << 1, (uint8_t)(exio_state | 1 << EXIO_LCD_BL)};
    uint8_t off[2] = {CH422G_ADDR_OUT << 1, (uint8_t)(exio_state & ~(1 << EXIO_LCD_BL))};
    i2c_operation_job_t ops[5] = {};
    ops[0].command = I2C_MASTER_CMD_START;
    ops[1].command = I2C_MASTER_CMD_WRITE;
    ops[1].write.ack_check = true;
    ops[1].write.data = on;
    ops[1].write.total_bytes = sizeof(on);
    ops[2].command = I2C_MASTER_CMD_START;
    ops[3].command = I2C_MASTER_CMD_WRITE;
    ops[3].write.ack_check = true;
    ops[3].write.data = off;
    ops[3].write.total_bytes = sizeof(off);
    ops[4].command = I2C_MASTER_CMD_STOP;
    i2c_master_execute_defined_operations(pulse_dev, ops, 5, 50);
  }
  xSemaphoreGive(exio_mutex);
}

// Priorité au-dessus de la tâche Wi-Fi (23) : seul l'instant de l'impulsion dépend d'elle, pas sa durée
static void backlightTask(void *) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / BL_PWM_HZ));
    uint8_t p = bl_percent;
    if (p > 0 && p < 100) sendPulse();
  }
}

void backlightBegin() {
  xTaskCreatePinnedToCore(backlightTask, "backlight", 4096, nullptr, 24, nullptr, 0);
}

void backlightSet(uint8_t percent) {
  percent = min<uint8_t>(percent, 100);
  if (percent > 0 && percent < 100) {
    // 40 us par pour cent, plus 128 us : la durée d'allumage qu'avait l'ancien PWM logiciel
    float on_us = percent * 40.0f + 128.0f;
    xSemaphoreTake(exio_mutex, portMAX_DELAY);
    setPulseClock((uint32_t)(BL_PULSE_BITS * 1000000.0f / on_us));
    xSemaphoreGive(exio_mutex);
  }
  bl_percent = percent;
  if (percent == 0 || percent == 100) exioWrite(EXIO_LCD_BL, percent == 100);
}
