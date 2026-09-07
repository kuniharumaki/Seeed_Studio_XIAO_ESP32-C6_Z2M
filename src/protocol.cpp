#include "protocol.h"
#include "transport.h"
#include "zb_ncp.h"
#include "utils.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "PROTOCOL";

protocol& protocol::instance() {
    static protocol s_protocol;
    return s_protocol;
}

protocol::protocol() {
}

protocol::~protocol() {
}

uint8_t protocol::next_seq(uint8_t seq) {
    return (seq >= 3) ? 1 : (seq + 1);
}

uint8_t* protocol::find_signature(uint8_t* s, const uint8_t* end) {
    if (!s || (end - s) < 2) return nullptr;
    const uint8_t* search_end = end - 1;
    while (s < search_end) {
        s = static_cast<uint8_t*>(memchr(s, 0xde, search_end - s));
        if (!s) return nullptr;
        if (s[1] == 0xad) {
            return s;
        }
        s++;
    }
    return nullptr;
}

void protocol::send_ack(const zb_transport_header_t& hdr) {
    auto rsp = static_cast<zb_transport_header_t*>(malloc(sizeof(zb_transport_header_t)));
    if (!rsp) {
        ESP_LOGE(TAG, "send_ack: failed to allocate %d bytes", sizeof(zb_transport_header_t));
        return;
    }
    rsp->signature[0] = 0xde;
    rsp->signature[1] = 0xad;
    rsp->packet_len = 5;
    rsp->packet_type = ZBOSS_NCP_API_HL;
    rsp->is_ack = 1;
    rsp->is_nack = 0;
    rsp->packet_seq = 0;
    rsp->ack_seq = hdr.packet_seq;
    rsp->first_fragment = 0;
    rsp->last_fragment = 0;
    rsp->header_crc = utils::crc8(&rsp->packet_len, 4);
    ESP_LOGI(TAG, "Sending immediate ACK for seq %d (control=0x%02X)", hdr.packet_seq, (hdr.packet_seq << 4) | 0x01);
    esp_err_t ret = transport::process_input(rsp, sizeof(zb_transport_header_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send immediate ACK, error: %s", esp_err_to_name(ret));
    }
}

void protocol::send_nack(const zb_transport_header_t& hdr) {
    auto rsp = static_cast<zb_transport_header_t*>(malloc(sizeof(zb_transport_header_t)));
    if (!rsp) {
        ESP_LOGE(TAG, "send_nack: failed to allocate %d bytes", sizeof(zb_transport_header_t));
        return;
    }
    rsp->signature[0] = 0xde;
    rsp->signature[1] = 0xad;
    rsp->packet_len = 5;
    rsp->packet_type = ZBOSS_NCP_API_HL;
    rsp->is_ack = 1;
    rsp->is_nack = 1;
    rsp->packet_seq = 0;
    rsp->ack_seq = hdr.packet_seq;
    rsp->first_fragment = 0;
    rsp->last_fragment = 0;
    rsp->header_crc = utils::crc8(&rsp->packet_len, 4);
    ESP_LOGI(TAG, "Sending immediate NACK for seq %d", hdr.packet_seq);
    esp_err_t ret = transport::process_input(rsp, sizeof(zb_transport_header_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send immediate NACK, error: %s", esp_err_to_name(ret));
    }
}

void protocol::on_frame(const zb_transport_header_t& hdr, const void* data, uint16_t size) {
    ESP_LOGI(TAG, "on_frame: is_ack=%d, is_nack=%d, type=%d, size=%d, seq=%d, ack_seq=%d",
        hdr.is_ack, hdr.is_nack, hdr.packet_type, size, hdr.packet_seq, hdr.ack_seq);
    if (hdr.is_nack) {
        ESP_LOGE(TAG, "NACK received. Retransmit is not supported.");
        return;
    }
    m_last_rx_seq = hdr.packet_seq;
    if (!data || size == 0) {
        ESP_LOGI(TAG, "ACK received for seq %d.", hdr.ack_seq);
        return;
    }
    if (hdr.packet_type != ZBOSS_NCP_API_HL) {
        ESP_LOGE(TAG, "Invalid packet type: %02x", int(hdr.packet_type));
        send_nack(hdr);
        return;
    }
    if (!hdr.is_ack) {
        send_ack(hdr);
    }
    zb_ncp::on_rx_data(data, size);
}

esp_err_t protocol::on_receive_data_impl(const uint8_t* data, uint16_t size) {
    if (unlikely(m_rx_buffer_pos + size > RINGBUF_SIZE)) {
        ESP_LOGE(TAG, "RX Buffer overflow, dropping old data");
        m_rx_buffer_pos = 0;
    }
    if (likely(size > 0)) {
        memcpy(&m_rx_buffer[m_rx_buffer_pos], data, size);
        m_rx_buffer_pos += size;
    }
    uint8_t* curr = m_rx_buffer;
    uint8_t* end = m_rx_buffer + m_rx_buffer_pos;
    while (true) {
        uint16_t remaining = end - curr;
        if (remaining < sizeof(zb_transport_header_t)) break;
        uint8_t* sig = find_signature(curr, end);
        if (!sig) {
            curr = (end > m_rx_buffer) ? end - 1 : end;
            break;
        }
        curr = sig;
        remaining = end - curr;
        if (remaining < sizeof(zb_transport_header_t)) break;
        auto hdr = reinterpret_cast<const zb_transport_header_t*>(curr);
        if (utils::crc8(&hdr->packet_len, 4) != hdr->header_crc) {
            ESP_LOGW(TAG, "CRC8 mismatch! calculated=%02X, header=%02X", utils::crc8(&hdr->packet_len, 4), hdr->header_crc);
            curr += 2;
            continue;
        }
        uint16_t full_packet_size = hdr->packet_len + 2; // 2 bytes signature
        if (full_packet_size > RINGBUF_SIZE || full_packet_size < sizeof(zb_transport_header_t)) {
            ESP_LOGW(TAG, "Invalid packet length: %d", hdr->packet_len);
            curr += 2;
            continue;
        }
        if (remaining < full_packet_size) break;
        int data_len = full_packet_size - sizeof(zb_transport_header_t) - sizeof(uint16_t); // sizeof(uint16_t) => CRC_16
        if (hdr->packet_len == 5) {
            on_frame(*hdr, nullptr, 0);
        } else if (data_len >= 0) {
            uint16_t crc_received;
            memcpy(&crc_received, curr + sizeof(zb_transport_header_t), sizeof(crc_received));
            const uint8_t* data_ptr = curr + sizeof(zb_transport_header_t) + sizeof(crc_received);
            uint16_t d_crc = utils::crc16(data_ptr, static_cast<uint16_t>(data_len));
            if (data_len == 0) {
                on_frame(*hdr, nullptr, 0);
            } else if (d_crc != crc_received) {
                ESP_LOGE(TAG, "Data CRC_16 mismatch: %04x != %04x", d_crc, crc_received);
                send_nack(*hdr);
            } else {
                if (data_len >= sizeof(zb_ncp::cmd_t)) {
                    ESP_LOGI(TAG, "Valid DATA frame received! data_len=%d", data_len);
                    on_frame(*hdr, data_ptr, static_cast<uint16_t>(data_len));
                } else {
                    curr += 2;
                    continue;
                }
            }
        } else {
            curr += 2;
            continue;
        }
        curr += full_packet_size;
    }
    m_rx_buffer_pos = end - curr;
    if (m_rx_buffer_pos > 0 && curr != m_rx_buffer) {
        memmove(m_rx_buffer, curr, m_rx_buffer_pos);
    }
    return ESP_OK;
}

esp_err_t protocol::send_data_impl(const void* data, uint16_t size) {
    if (!data || size == 0) {
        ESP_LOGE(TAG, "Attempt to send ZERO data!");
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t total_size = sizeof(zb_transport_header_t) + sizeof(uint16_t) + size;  // sizeof(uint16_t) => CRC_16
    uint8_t* tx_buf = static_cast<uint8_t*>(malloc(total_size));
    if (!tx_buf) {
        ESP_LOGE(TAG, "send_data: failed to allocate %d bytes", total_size);
        return ESP_ERR_NO_MEM;
    }
    uint8_t current_seq = m_tx_seq.load();
    while (!m_tx_seq.compare_exchange_weak(current_seq, next_seq(current_seq)));
    auto hdr = reinterpret_cast<zb_transport_header_t*>(tx_buf);
    hdr->signature[0] = 0xde;
    hdr->signature[1] = 0xad;
    hdr->packet_len = sizeof(zb_transport_header_t) + size;
    hdr->packet_type = ZBOSS_NCP_API_HL;
    hdr->is_ack = 0;
    hdr->is_nack = 0;
    hdr->packet_seq = current_seq;
    hdr->ack_seq = 0;
    hdr->first_fragment = 1;
    hdr->last_fragment = 1;
    hdr->header_crc = utils::crc8(&hdr->packet_len, 4);
    uint16_t crc_16 = utils::crc16(data, size);
    memcpy(tx_buf + sizeof(zb_transport_header_t), &crc_16, sizeof(crc_16));
    memcpy(tx_buf + sizeof(zb_transport_header_t) + sizeof(crc_16), data, size);
    ESP_LOGI(TAG, "send_data: seq=%d (control=0x%02X), data_len=%d, total=%d",
        current_seq, 0xC0 | (current_seq << 2), size, total_size);
    return transport::process_input(tx_buf, total_size);
}

void protocol::reset_impl() {
    m_rx_buffer_pos = 0;
    m_tx_seq = 0;
    m_last_rx_seq = 0;
    ESP_LOGI(TAG, "Protocol state reset (seq=0)");
}

esp_err_t protocol::run() {
    reset_impl();
    return ESP_OK;
}

esp_err_t protocol::start() {
    return instance().run();
}
