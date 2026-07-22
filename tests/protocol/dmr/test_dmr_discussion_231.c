// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Compact protocol fixture derived from the 12.5 kHz WAV attached to
 * GitHub Discussion #231. The source recording is intentionally not vendored.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_text.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

typedef struct {
    uint8_t dibits[98];
    uint8_t reliability[98];
} discussion_231_r34_block;

static const discussion_231_r34_block k_blocks[5] = {
    {
        {
            0U, 2U, 0U, 1U, 0U, 1U, 0U, 2U, 0U, 3U, 3U, 2U, 0U, 1U, 0U, 2U, 0U, 2U, 0U, 2U, 3U, 1U, 0U, 2U, 3U,
            2U, 0U, 2U, 0U, 3U, 3U, 3U, 3U, 1U, 0U, 2U, 1U, 3U, 0U, 0U, 0U, 2U, 0U, 2U, 2U, 2U, 3U, 2U, 3U, 1U,
            3U, 2U, 0U, 2U, 2U, 2U, 2U, 3U, 0U, 2U, 0U, 2U, 2U, 2U, 1U, 3U, 0U, 2U, 3U, 3U, 0U, 2U, 2U, 3U, 3U,
            0U, 0U, 2U, 0U, 2U, 1U, 1U, 2U, 3U, 0U, 2U, 1U, 3U, 3U, 3U, 2U, 3U, 0U, 2U, 0U, 2U, 0U, 0U,
        },
        {
            174U, 111U, 194U, 203U, 160U, 141U, 156U, 184U, 186U, 126U, 200U, 120U, 171U, 169U, 131U, 154U, 169U,
            198U, 131U, 148U, 200U, 121U, 167U, 190U, 168U, 116U, 198U, 180U, 181U, 132U, 192U, 189U, 180U, 77U,
            73U,  105U, 203U, 176U, 173U, 136U, 139U, 153U, 182U, 188U, 151U, 193U, 184U, 149U, 146U, 203U, 165U,
            125U, 171U, 200U, 196U, 138U, 167U, 203U, 185U, 143U, 174U, 128U, 144U, 58U,  203U, 196U, 184U, 167U,
            103U, 203U, 164U, 200U, 196U, 176U, 192U, 87U,  137U, 153U, 188U, 164U, 169U, 193U, 104U, 203U, 200U,
            116U, 203U, 184U, 133U, 203U, 160U, 203U, 192U, 157U, 199U, 171U, 179U, 185U,
        },
    },
    {
        {
            0U, 2U, 0U, 2U, 0U, 1U, 0U, 2U, 0U, 2U, 3U, 1U, 0U, 1U, 0U, 2U, 1U, 3U, 0U, 2U, 0U, 1U, 0U, 2U, 2U,
            2U, 0U, 2U, 3U, 3U, 0U, 0U, 0U, 2U, 0U, 2U, 0U, 3U, 3U, 0U, 3U, 1U, 0U, 2U, 2U, 2U, 0U, 0U, 3U, 1U,
            2U, 3U, 0U, 2U, 3U, 3U, 1U, 3U, 0U, 2U, 0U, 2U, 2U, 2U, 1U, 0U, 0U, 2U, 3U, 3U, 2U, 2U, 0U, 2U, 3U,
            2U, 0U, 2U, 2U, 2U, 3U, 3U, 2U, 3U, 0U, 2U, 2U, 2U, 3U, 3U, 2U, 3U, 0U, 2U, 1U, 3U, 2U, 3U,
        },
        {
            193U, 157U, 124U, 196U, 127U, 160U, 166U, 189U, 124U, 119U, 203U, 127U, 172U, 157U, 102U, 98U,  203U,
            185U, 164U, 172U, 178U, 172U, 142U, 164U, 185U, 151U, 178U, 200U, 141U, 203U, 170U, 153U, 137U, 133U,
            200U, 176U, 164U, 107U, 203U, 184U, 172U, 137U, 167U, 176U, 125U, 202U, 126U, 180U, 134U, 203U, 150U,
            203U, 161U, 172U, 95U,  203U, 203U, 196U, 201U, 129U, 168U, 131U, 164U, 96U,  199U, 199U, 184U, 188U,
            141U, 203U, 166U, 160U, 173U, 184U, 171U, 114U, 184U, 202U, 180U, 158U, 130U, 203U, 195U, 195U, 139U,
            197U, 189U, 151U, 121U, 203U, 178U, 203U, 188U, 94U,  203U, 203U, 186U, 203U,
        },
    },
    {
        {
            0U, 2U, 2U, 2U, 3U, 1U, 0U, 2U, 0U, 2U, 0U, 2U, 0U, 1U, 0U, 2U, 1U, 2U, 2U, 2U, 0U, 1U, 0U, 2U, 0U,
            2U, 3U, 1U, 3U, 3U, 2U, 0U, 0U, 2U, 0U, 2U, 2U, 2U, 3U, 0U, 3U, 1U, 0U, 2U, 0U, 2U, 1U, 2U, 0U, 2U,
            3U, 2U, 3U, 3U, 1U, 2U, 1U, 3U, 0U, 2U, 3U, 3U, 0U, 3U, 0U, 2U, 0U, 2U, 0U, 2U, 3U, 2U, 1U, 3U, 2U,
            0U, 0U, 2U, 0U, 2U, 3U, 3U, 2U, 3U, 0U, 2U, 0U, 2U, 1U, 0U, 3U, 2U, 0U, 2U, 2U, 2U, 3U, 3U,
        },
        {
            156U, 86U,  176U, 168U, 203U, 74U,  114U, 175U, 192U, 188U, 174U, 173U, 173U, 158U, 102U, 112U, 194U,
            203U, 194U, 139U, 161U, 193U, 173U, 196U, 97U,  108U, 203U, 162U, 123U, 203U, 125U, 160U, 175U, 184U,
            149U, 167U, 123U, 158U, 203U, 192U, 178U, 126U, 122U, 157U, 201U, 125U, 203U, 139U, 157U, 169U, 164U,
            95U,  93U,  203U, 203U, 124U, 203U, 203U, 202U, 141U, 86U,  203U, 170U, 200U, 183U, 153U, 183U, 149U,
            186U, 172U, 122U, 50U,  203U, 163U, 157U, 117U, 145U, 149U, 189U, 188U, 125U, 203U, 184U, 203U, 189U,
            151U, 188U, 148U, 172U, 160U, 139U, 97U,  183U, 195U, 166U, 179U, 147U, 203U,
        },
    },
    {
        {
            0U, 2U, 3U, 3U, 0U, 1U, 0U, 2U, 0U, 3U, 3U, 2U, 0U, 1U, 0U, 2U, 0U, 2U, 3U, 2U, 0U, 1U, 0U, 2U, 1U,
            2U, 3U, 1U, 2U, 2U, 3U, 0U, 3U, 1U, 0U, 2U, 3U, 3U, 3U, 0U, 0U, 2U, 0U, 2U, 0U, 2U, 2U, 1U, 3U, 1U,
            3U, 1U, 0U, 2U, 0U, 3U, 2U, 3U, 0U, 2U, 0U, 2U, 0U, 3U, 1U, 3U, 0U, 2U, 3U, 3U, 1U, 3U, 0U, 2U, 3U,
            1U, 0U, 2U, 0U, 2U, 1U, 1U, 1U, 0U, 0U, 2U, 0U, 2U, 3U, 3U, 2U, 3U, 0U, 2U, 2U, 2U, 1U, 0U,
        },
        {
            160U, 106U, 75U,  203U, 90U,  165U, 139U, 166U, 198U, 136U, 203U, 121U, 162U, 193U, 157U, 177U, 141U,
            168U, 179U, 120U, 165U, 184U, 145U, 167U, 129U, 92U,  203U, 171U, 168U, 132U, 203U, 173U, 203U, 168U,
            198U, 142U, 203U, 169U, 203U, 98U,  137U, 166U, 174U, 194U, 131U, 153U, 113U, 183U, 121U, 203U, 196U,
            102U, 110U, 144U, 193U, 182U, 199U, 203U, 188U, 105U, 121U, 66U,  97U,  203U, 203U, 203U, 184U, 128U,
            70U,  203U, 199U, 149U, 123U, 168U, 178U, 78U,  88U,  139U, 187U, 164U, 165U, 188U, 203U, 142U, 164U,
            150U, 188U, 192U, 129U, 203U, 195U, 203U, 169U, 177U, 184U, 107U, 191U, 188U,
        },
    },
    {
        {
            0U, 2U, 3U, 1U, 0U, 1U, 0U, 2U, 3U, 2U, 1U, 0U, 0U, 1U, 0U, 2U, 1U, 3U, 0U, 2U, 1U, 2U, 1U, 0U, 2U,
            3U, 3U, 2U, 1U, 2U, 0U, 3U, 3U, 1U, 0U, 2U, 0U, 3U, 2U, 1U, 0U, 2U, 0U, 2U, 3U, 1U, 2U, 0U, 3U, 0U,
            2U, 2U, 0U, 2U, 1U, 2U, 2U, 3U, 0U, 2U, 3U, 3U, 1U, 3U, 2U, 0U, 0U, 2U, 3U, 1U, 0U, 0U, 2U, 0U, 1U,
            0U, 0U, 2U, 1U, 3U, 0U, 0U, 2U, 3U, 0U, 2U, 2U, 2U, 3U, 3U, 0U, 2U, 3U, 1U, 1U, 0U, 1U, 1U,
        },
        {
            109U, 41U,  155U, 168U, 166U, 187U, 186U, 149U, 181U, 128U, 163U, 181U, 192U, 141U, 69U,  55U,  203U,
            203U, 179U, 87U,  203U, 129U, 203U, 179U, 137U, 203U, 153U, 66U,  203U, 191U, 184U, 141U, 203U, 90U,
            94U,  149U, 185U, 145U, 118U, 152U, 145U, 202U, 108U, 98U,  203U, 180U, 178U, 200U, 155U, 192U, 163U,
            98U,  166U, 117U, 203U, 168U, 154U, 203U, 196U, 156U, 94U,  203U, 192U, 140U, 135U, 166U, 183U, 144U,
            203U, 156U, 196U, 183U, 189U, 107U, 203U, 144U, 123U, 80U,  203U, 196U, 173U, 146U, 121U, 203U, 157U,
            200U, 188U, 164U, 147U, 203U, 111U, 141U, 199U, 94U,  203U, 191U, 153U, 203U,
        },
    },
};

static const uint8_t k_assembled_pdu[80] = {
    0x00U, 0x68U, 0x00U, 0x65U, 0x00U, 0x6CU, 0x00U, 0x6FU, 0x00U, 0x20U, 0x00U, 0x69U, 0x00U, 0x20U, 0x00U, 0x61U,
    0x00U, 0x6DU, 0x00U, 0x20U, 0x00U, 0x6AU, 0x00U, 0x75U, 0x00U, 0x6EU, 0x00U, 0x69U, 0x00U, 0x6FU, 0x00U, 0x72U,
    0x00U, 0x2CU, 0x00U, 0x20U, 0x00U, 0x69U, 0x00U, 0x74U, 0x00U, 0x73U, 0x00U, 0x20U, 0x00U, 0x61U, 0x00U, 0x20U,
    0x00U, 0x74U, 0x00U, 0x65U, 0x00U, 0x78U, 0x00U, 0x74U, 0x00U, 0x20U, 0x00U, 0x6DU, 0x00U, 0x65U, 0x00U, 0x73U,
    0x00U, 0x73U, 0x00U, 0x61U, 0x00U, 0x67U, 0x00U, 0x65U, 0x00U, 0x2EU, 0x00U, 0x00U, 0x7BU, 0x17U, 0x9AU, 0x5FU,
};

static void
build_info_bits(const uint8_t dibits[98], uint8_t info[196]) {
    for (size_t i = 0U; i < 98U; i++) {
        info[i * 2U] = (uint8_t)((dibits[i] >> 1U) & 1U);
        info[(i * 2U) + 1U] = (uint8_t)(dibits[i] & 1U);
    }
}

static void
pack_crc32_bits(const uint8_t bytes[80], uint8_t bits[640]) {
    for (size_t i = 0U, j = 0U; i < 80U; i += 2U, j += 16U) {
        for (size_t bit = 0U; bit < 8U; bit++) {
            bits[j + bit] = (uint8_t)((bytes[i + 1U] >> (7U - bit)) & 1U);
            bits[j + 8U + bit] = (uint8_t)((bytes[i] >> (7U - bit)) & 1U);
        }
    }
}

static void
test_capture_blocks_through_live_decoder(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t info[196];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(history, 0, sizeof(history));
    state.event_history_s = history;
    state.currentslot = 0U;
    state.data_header_valid[0] = 1U;
    state.data_header_format[0] = 13U;
    state.data_header_sap[0] = 10U;
    state.data_header_blocks[0] = 5U;
    state.data_block_counter[0] = 1U;
    state.data_conf_data[0] = 1U;
    state.data_header_dd_format[0] = 0x16U;
    state.data_header_bit_padding[0] = 16U;
    state.dmr_lrrp_source[0] = 31U;
    state.dmr_lrrp_target[0] = 2515U;
    opts.aggressive_framesync = 1;

    for (size_t block = 0U; block < 5U; block++) {
        build_info_bits(k_blocks[block].dibits, info);
        dmr_data_burst_handler(&opts, &state, info, 0x08U, k_blocks[block].reliability);
        assert(state.data_block_crc_valid[0][block + 1U] == 1U);
        if (block < 4U) {
            assert(memcmp(state.dmr_pdu_sf[0], k_assembled_pdu, (block + 1U) * 16U) == 0);
        }
    }

    assert(strcmp(history[0].Event_History_Items[0].text_message, "helo i am junior, its a text message.") == 0);
    assert(strstr(history[0].Event_History_Items[1].event_string, "declared UTF-32; decoded UTF-16BE compatibility")
           != NULL);
    assert(history[0].Event_History_Items[1].source_id == 31U);
    assert(history[0].Event_History_Items[1].target_id == 2515U);
    assert(state.data_header_valid[0] == 0U);
    assert(state.data_header_dd_format[0] == 0U);
    assert(state.data_header_bit_padding[0] == 0U);
}

static void
test_capture_packet_crc_padding_and_text(void) {
    uint8_t crc_bits[640];
    DSD_MEMSET(crc_bits, 0, sizeof(crc_bits));
    pack_crc32_bits(k_assembled_pdu, crc_bits);
    uint32_t extracted = ((uint32_t)k_assembled_pdu[76] << 24U) | ((uint32_t)k_assembled_pdu[77] << 16U)
                         | ((uint32_t)k_assembled_pdu[78] << 8U) | (uint32_t)k_assembled_pdu[79];
    assert(ComputeCrc32Bit(crc_bits, 608U) == extracted);

    size_t payload_bytes = 0U;
    assert(dmr_short_data_payload_bytes((sizeof k_assembled_pdu - 4U) * 8U, 16U, &payload_bytes) == 0);
    assert(payload_bytes == 74U);
    dmr_text_result result;
    assert(dmr_decode_defined_short_data(0x16U, k_assembled_pdu, payload_bytes, 1, &result) == 0);
    assert(result.compatibility == 1U);
    assert(strcmp(result.text, "helo i am junior, its a text message.") == 0);
}

int
main(void) {
    test_capture_blocks_through_live_decoder();
    test_capture_packet_crc_padding_and_text();
    puts("DMR_DISCUSSION_231: OK");
    return 0;
}
