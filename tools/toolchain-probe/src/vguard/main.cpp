// App principal (env vguard): inclui o driver TWAI e cria uma tarefa FreeRTOS.
// Só prova que compila e liga; não precisa de transceptor para o build.
#include <cstdio>

#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "crc16.h"

static const char* TAG = "vguard";

static void can_task(void*) {
    const twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_21, GPIO_NUM_22, TWAI_MODE_NORMAL);
    const twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    const twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK && twai_start() == ESP_OK) {
        // Requisição OBD modo 01, PID 0x0C (RPM), endereço funcional 0x7DF.
        twai_message_t msg = {};
        msg.identifier = 0x7DF;
        msg.data_length_code = 8;
        const uint8_t req[8] = {0x02, 0x01, 0x0C, 0x55, 0x55, 0x55, 0x55, 0x55};
        for (int i = 0; i < 8; ++i) msg.data[i] = req[i];
        twai_transmit(&msg, pdMS_TO_TICKS(10));
    }

    for (;;) {
        ESP_LOGI(TAG, "crc16=0x%04X", probe::crc16("123456789"));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main(void) {
    std::printf("vguard probe, __cplusplus=%ld\n", static_cast<long>(__cplusplus));
    xTaskCreate(can_task, "can", 4096, nullptr, 5, nullptr);
}
