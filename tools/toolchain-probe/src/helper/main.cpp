// Segundo app (env helper): esboço do adaptador USB-CAN. Fontes diferentes do vguard,
// mas usa a mesma biblioteca de lib/. Usa o driver TWAI novo do ESP-IDF 6.x
// (esp_twai.h + esp_twai_onchip.h), para provar que ele compila em C++ no esp32 clássico.
#include <cstdio>

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "crc16.h"

namespace {

struct RxFrame {
    uint32_t id;
    uint8_t len;
    uint8_t data[8];
};

QueueHandle_t rx_queue;

// O driver novo entrega a recepção num callback em contexto de ISR.
bool on_rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t*, void*) {
    uint8_t buf[8];
    twai_frame_t frame = {};
    frame.buffer = buf;
    frame.buffer_len = sizeof(buf);
    if (twai_node_receive_from_isr(node, &frame) != ESP_OK) return false;

    RxFrame out = {};
    out.id = frame.header.id;
    out.len = static_cast<uint8_t>(frame.header.dlc > 8 ? 8 : frame.header.dlc);
    for (int i = 0; i < out.len; ++i) out.data[i] = buf[i];

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(rx_queue, &out, &woken);
    return woken == pdTRUE;
}

void bridge_task(void*) {
    twai_onchip_node_config_t cfg = {};
    cfg.io_cfg.tx = GPIO_NUM_21;
    cfg.io_cfg.rx = GPIO_NUM_22;
    cfg.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    cfg.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    cfg.bit_timing.bitrate = 500000;
    cfg.tx_queue_depth = 8;

    twai_node_handle_t node = nullptr;
    twai_event_callbacks_t cbs = {};
    cbs.on_rx_done = on_rx_done;

    if (twai_new_node_onchip(&cfg, &node) == ESP_OK &&
        twai_node_register_event_callbacks(node, &cbs, nullptr) == ESP_OK &&
        twai_node_enable(node) == ESP_OK) {
        uint8_t req[8] = {0x02, 0x01, 0x0C, 0x55, 0x55, 0x55, 0x55, 0x55};
        twai_frame_t tx = {};
        tx.header.id = 0x7DF;
        tx.header.dlc = 8;
        tx.buffer = req;
        tx.buffer_len = sizeof(req);
        twai_node_transmit(node, &tx, 10);
    }

    RxFrame f;
    for (;;) {
        if (xQueueReceive(rx_queue, &f, pdMS_TO_TICKS(1000)) == pdTRUE) {
            std::printf("t%03lX%u\n", static_cast<unsigned long>(f.id), f.len);  // estilo slcan
        }
        std::printf("helper probe, crc16=0x%04X\n", probe::crc16("123456789"));
    }
}

}  // namespace

extern "C" void app_main(void) {
    rx_queue = xQueueCreate(32, sizeof(RxFrame));
    xTaskCreate(bridge_task, "bridge", 4096, nullptr, 5, nullptr);
}
